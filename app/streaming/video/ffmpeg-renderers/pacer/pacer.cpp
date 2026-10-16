#include "pacer.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>
#include <QDateTime>
#include <climits>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3

#ifdef Q_OS_LINUX
#include <time.h>

// FrogtaleTV patch: frame pacing for X11, which has no IVsyncSource. A Steam Deck runs Moonlight as
// an X11 client under gamescope, so "Frame pacing" silently fell through to rendering every frame
// the moment it was decoded (no PacerVsync thread), and any network jitter (15-60 ms on Starlink,
// Wi-Fi at home) showed up as uneven motion. GeForce Now presents by timestamp with an adaptive
// queue. This ticks at the display rate from CLOCK_MONOTONIC so the pacer releases one frame per
// refresh and keeps its 1-3 frame cushion; V-Sync (FIFO present) aligns each one to the real vblank.
// On by default; FROGTALE_SOFT_VSYNC=0 restores stock behaviour.
class SoftwareVsyncSource : public IVsyncSource
{
public:
    bool initialize(SDL_Window*, int displayFps) override
    {
        if (displayFps <= 0) {
            return false;
        }
        m_PeriodNs = 1000000000LL / displayFps;
        clock_gettime(CLOCK_MONOTONIC, &m_Next);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "FrogtaleTV: software vsync at %d Hz", displayFps);
        return true;
    }

    bool isAsync() override
    {
        return false;
    }

    void waitForVsync() override
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long next = (long long)m_Next.tv_sec * 1000000000LL + m_Next.tv_nsec + m_PeriodNs;
        long long cur = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
        if (next < cur - m_PeriodNs) {
            // We fell more than a period behind (stall, suspend): restart the cadence from now
            next = cur + m_PeriodNs;
        }
        m_Next.tv_sec = next / 1000000000LL;
        m_Next.tv_nsec = next % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &m_Next, nullptr);
    }

private:
    long long m_PeriodNs = 0;
    struct timespec m_Next = {};
};
#endif
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + PACER_MAX_CUSHION_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats),
    m_TickCount(0),
    m_LastPresentUs(0),
    m_WindowActive(false),
    m_WindowArm(false),
    m_WindowMaxGapMs(0),
    m_WindowMaxQueue(0),
    m_WindowSkips(0)
{
    // FrogtaleTV patch: frames to bank before each predicted Starlink handoff stall (0 = off), and
    // FROGTALE_CUSHION_AB=1 to use the cushion only in odd UTC minutes (A/B against the same weather)
    m_CushionFrames = SDL_clamp(qEnvironmentVariableIntValue("FROGTALE_CUSHION") ?
                                    qEnvironmentVariableIntValue("FROGTALE_CUSHION") : 5,
                                0, PACER_MAX_CUSHION_FRAMES);
    if (qEnvironmentVariable("FROGTALE_CUSHION") == "0") {
        m_CushionFrames = 0;
    }
    m_CushionAb = qEnvironmentVariableIntValue("FROGTALE_CUSHION_AB") != 0;
}

// FrogtaleTV patch: a cushion across Starlink handoffs. With the 40% host dip the stall still
// freezes the picture for a median ~60 ms every 15 s (2026-10-16). The stall's moment is
// predictable (LiFrogtaleHandoffPhaseMs), so for ~350 ms before it every third refresh repeats
// the current picture instead of advancing, banking up to m_CushionFrames decoded frames (~55 ms
// at 90 fps). During the stall those frames keep playing; afterwards the burst that arrives is
// trimmed back one frame per refresh, instead of all at once. Returns 0 normal, 1 banking,
// 2 riding the stall, 3 trimming. Also measures, per handoff, the longest gap between new
// pictures (the visible freeze) for the A/B, logged as "FrogtaleTV cushion".
int Pacer::cushionState()
{
    if (m_CushionFrames <= 0) {
        return 0;
    }
    int phase = LiFrogtaleHandoffPhaseMs();
    if (phase < 0) {
        return 0;
    }
    qint64 wall = QDateTime::currentMSecsSinceEpoch();
    int nowPhase = (int)(wall % 15000);
    int untilMs = (phase - nowPhase + 15000) % 15000;
    int sinceMs = (nowPhase - phase + 15000) % 15000;
    bool arm = !m_CushionAb || (wall / 60000) % 2 == 1;

    bool inWindow = untilMs <= 500 || sinceMs <= 1000;
    if (inWindow && !m_WindowActive) {
        m_WindowActive = true;
        m_WindowArm = arm;
        m_WindowMaxGapMs = 0;
        m_WindowMaxQueue = 0;
        m_WindowSkips = 0;
    }
    else if (!inWindow && m_WindowActive) {
        m_WindowActive = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "FrogtaleTV cushion: %s maxgap=%d ms maxqueue=%d banked=%d phase=%d",
                    m_WindowArm ? "on" : "off", m_WindowMaxGapMs, m_WindowMaxQueue, m_WindowSkips, phase);
    }

    if (!m_WindowArm || !m_WindowActive) {
        return 0;
    }
    if (untilMs <= 350) {
        return 1;
    }
    if (sinceMs <= 400) {
        return 2;
    }
    if (sinceMs <= 1000) {
        return 3;
    }
    return 0;
}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue, MAX_QUEUED_FRAMES);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    int cushion = cushionState();
    m_TickCount++;

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    if (cushion == 1 && m_PacingQueue.count() <= m_CushionFrames && m_TickCount % 3 == 0) {
        // Bank a frame: show the current picture for one more refresh
        m_WindowSkips++;
        m_FrameQueueLock.unlock();
        return;
    }

    // Catch up if we're several frames ahead (not while banking or riding the stall; one frame
    // per refresh while trimming the cushion back)
    int maxDrops = (cushion == 1 || cushion == 2) ? 0 : cushion == 3 ? 1 : INT_MAX;
    while (maxDrops-- > 0 && m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    if (m_WindowActive) {
        uint64_t nowUs = LiGetMicroseconds();
        if (m_LastPresentUs != 0) {
            m_WindowMaxGapMs = SDL_max(m_WindowMaxGapMs, (int)((nowUs - m_LastPresentUs) / 1000));
        }
        m_WindowMaxQueue = SDL_max(m_WindowMaxQueue, (int)m_PacingQueue.count());
        m_LastPresentUs = nowUs;
    }
    else {
        m_LastPresentUs = LiGetMicroseconds();
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
#ifdef Q_OS_LINUX
            if (qEnvironmentVariable("FROGTALE_SOFT_VSYNC", "1") != "0") {
                m_VsyncSource = new SoftwareVsyncSource();
                break;
            }
#endif
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    m_VideoStats->renderedFrames++;

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue, int limit)
{
    SDL_assert(queue.size() <= limit);
    if (queue.size() >= limit) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue, MAX_QUEUED_FRAMES + m_CushionFrames);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
