# FrogtaleTV Starlink build of Moonlight for Apple TV

This is the Apple TV version of the Steam Deck Starlink build
([aesse94/moonlight-qt](https://github.com/aesse94/moonlight-qt), branch `frogtale-starlink`). It works
with the matching Apollo fork ([aesse94/Apollo](https://github.com/aesse94/Apollo), branch
`frogtale-starlink`), and installs as its own app (**Moonlight Starlink**, `com.frogtale.MoonlightStarlink`),
next to stock Moonlight.

Target device: **Apple TV 4K (2nd generation, 2021, A12)**. Any Apple TV 4K or the Apple TV HD runs it too;
the Apple TV HD is held to 1080p. (The original 2010 "Apple TV 2nd generation" can't run apps at all.)

## What this build changes
| Change | Where | Apple TV setting |
|---|---|---|
| NACK lost video packets, re-NACK 2 × 35 ms, queueing delay reports (`FTDL`, with the received rate) for Apollo's adaptive bitrate | `moonlight-common-c` submodule (`aesse94/moonlight-common-c`, `frogtale-starlink`, the same commit as the Deck) | **NACK Lost Video Packets** (on). Needs the patched Apollo host |
| Hold later-frame packets while the current FEC block is incomplete: 100 ms with NACK, 160 ms at handoffs, longer while the link queues, short holds after a timeout, storm mode, 10 ms poll while holding | same | always on (80 ms without NACK) |
| Learn the Starlink handoff phase from video packet gaps (needs a real rhythm, forgotten after 2 min, hysteresis) | same | always on |
| Handoff cushion: bank frames before each predicted handoff, play them through the stall, stretch them while it lasts, trim the burst back afterwards; depth adapts 4 → 7 frames | `Limelight/Stream/VideoDecoderRenderer.m` | **Handoff Cushion** (on). Needs **Frame Pacing: Smoothest Video** |
| Cushion A/B test: the cushion runs only in odd UTC minutes, and each handoff's log line says `on` or `off` | same | **Cushion A/B Test** (off) |
| Write the log to a file (the console is gone unless Xcode is attached) | `Limelight/Utility/Logger.m`, `ClLogMessage` in `Connection.m` | — (`Library/Caches/Moonlight.log`) |
| Write hold and delay events to `frogtale-reorder.log` (there's no `XDG_CACHE_HOME` on tvOS) | `Connection.m` | — (`Library/Caches/frogtale-reorder.log`) |

The settings are in the Apple TV's **Settings → Apps → Moonlight Starlink**. This build's defaults differ
from stock: HEVC (hardware decoded on the A12, and better than H.264 at Starlink bitrates), 15 Mbps,
Smoothest Video, both Starlink switches on.

### Every Deck change, and where it went
moonlight-common-c: all 15 commits (holds, NACK, retries, storm mode, FTDL, 160 ms handoff holds,
queue-aware and short holds, handoff phase learning) are in this build unchanged, through the submodule.
Their tuning knobs keep the Deck's defaults (`FROGTALE_NACK_RETRY_MS` 35, `FROGTALE_REORDER_MS` 100 with
NACK, `FROGTALE_DELAY` following NACK).

moonlight-qt (25 commits):
| Deck change | Apple TV |
|---|---|
| Pacer handoff cushion, adaptive depth, stretch | ported (depth 4–7 frames instead of 5–10, see below) |
| `FROGTALE_CUSHION_AB` | ported as **Cushion A/B Test**. The separate adapt/stretch A/B switches and the depth knobs were not ported; adapt and stretch are always on |
| Log to file on Linux | ported (log file) |
| Point the `moonlight-common-c` submodule at the fork | done |
| X11 software vsync for frame pacing | not needed: tvOS paces off `CADisplayLink`, the real display refresh |
| Prefer Vulkan Video decode | not needed: tvOS decodes in hardware with VideoToolbox |
| Opt-in 10 ms Opus (`FROGTALE_SLOW_OPUS`, off on the Deck too) | not needed: the Apple TV has no 3–4 W TDP to save |
| Flatpak, gamescope Vulkan layer, libplacebo patch, Flatpak CI | Linux packaging; the Apple TV build is the Xcode project |

Host side (Apollo) needs nothing new: this client sends the same `FTNK` and `FTDL` messages as the Deck.

### Cushion depth on Apple TV
The Deck banks decoded frames in Moonlight's pacer. On tvOS the pacing queue is moonlight-common-c's decode
unit queue, which flushes and requests an IDR frame at 15 frames, so the cushion is 4–7 frames
(67–117 ms at 60 fps) and catches up whenever the queue reaches 12. Modelled at 60 fps, it keeps the longest
visible freeze at ~33 ms for stalls up to ~200 ms. Stalls of 220 ms+ overflow that queue with or without
the cushion (stock tvOS Moonlight too).

The Apple TV 4K outputs at most 60 Hz, so stream at 60 fps (the Deck streams at 90).

### Logs
`Library/Caches/Moonlight.log` holds the whole log, Moonlight's and moonlight-common-c's, with wall clock
times; it's replaced on each launch. Each handoff logs a
`FrogtaleTV cushion: on|off maxgap=… maxqueue=… banked=… stretched=… phase=… target=…` line. Hold and delay
events go to `Library/Caches/frogtale-reorder.log` as on the Deck; it starts over once it passes 4 MB.
To get them: Xcode → Window → Devices and Simulators → the Apple TV → Moonlight Starlink → Download
Container, then Show Package Contents on the downloaded file.

## Build and install on your Apple TV
You need a Mac with Xcode and an Apple ID. A free Apple ID works, but apps it signs expire after 7 days
(just run it from Xcode again). A paid Apple Developer account makes them last a year.

1. Clone it with its submodules (for now it lives on the `appletv-starlink` branch of the moonlight-qt
   fork; it's the moonlight-ios codebase, not Qt):
   ```
   git clone --recursive -b appletv-starlink https://github.com/aesse94/moonlight-qt.git moonlight-appletv
   ```
2. Open `Moonlight.xcodeproj`. If Xcode asks, download the tvOS platform (Xcode → Settings → Components).
3. Choose the **Moonlight TV** scheme. In the project's **Moonlight TV** target → Signing & Capabilities,
   set Team to your Apple ID. If Xcode says the bundle identifier isn't available, change it to something
   unique, such as `com.<yourname>.MoonlightStarlink`.
4. Pair the Apple TV (same network as the Mac): on the Apple TV open Settings → Remotes and Devices →
   Remote App and Devices; in Xcode open Window → Devices and Simulators, select the Apple TV and enter the
   code it shows.
5. Product → Scheme → Edit Scheme → Run → set Build Configuration to **Release** (Debug is slower).
6. Pick the Apple TV as the run destination and press Run. The app appears on the Home Screen as
   **Moonlight Starlink**.

Moonlight is GPL-3.0, and so is everything here.
