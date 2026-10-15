# FrogtaleTV Starlink build of Moonlight

This fork makes game streaming from Moonlight to a **Steam Deck over Starlink** smooth. It works with the
matching Apollo fork ([aesse94/Apollo](https://github.com/aesse94/Apollo), branch `frogtale-starlink`), and
installs as its own Flatpak (`com.frogtale.MoonlightStarlink`), next to stock Moonlight.

## The problem
The Starlink downlink stalls for roughly 15–80 ms at every satellite reconfiguration, which happens at
:12/:27/:42/:57 past each minute, and sometimes at other moments too. The packets in flight during a stall
are lost, not delayed. Stock Moonlight can only answer a lost frame with an RFI, and the host usually turns
that into an IDR: a full round trip plus a large keyframe, felt as about 2 stutters a minute. Two things did
not help: raising the host's FEC (whole frames are lost) and waiting for the packets (they never arrive).

## What this build changes
| Change | Where | Switch (Flatpak env) |
|---|---|---|
| NACK lost video packets: every forward jump in RTP sequence numbers is sent to the host as `FTNK` + ranges | `moonlight-common-c/src/VideoStream.c` | `FROGTALE_NACK=1` |
| Re-NACK whatever is still missing, 2 × 35 ms | same | `FROGTALE_NACK_RETRY_MS` |
| Hold packets of later frames while the current FEC block is incomplete, so resends (and the host's handoff copies) can finish it. 100 ms with NACK. After a timeout, no holding for 1 s | same | `FROGTALE_REORDER_MS` (0 = stock) |
| Report queueing delay to the host every 250 ms (`FTDL`), so Apollo steps the bitrate down when the link starts queueing, before packets are lost (needs `frogtale_adaptive_bitrate = 1` on the host) | same | on with NACK; `FROGTALE_DELAY=0` turns it off |
| Poll the video socket every 10 ms while holding, so holds can't overrun during a stall | same | — |
| Try Vulkan Video decoding before VAAPI (decoded frames stay in Vulkan; no per-frame DRM_PRIME import) | `app/streaming/video/ffmpeg.cpp` | `FROGTALE_VULKAN_DECODE=1`, plus `RADV_PERFTEST=video_decode` on Mesa 26.2 RADV |
| Request 10 ms Opus packets (fewer audio wakeups at low TDP) | `app/streaming/audio/audio.cpp` | `FROGTALE_SLOW_OPUS=1` |
| Frame pacing on X11: a 90 Hz software vsync clock, so the pacer releases one frame per refresh instead of drawing each frame the moment it decodes (stock Moonlight has no X11 vsync source, so "Frame pacing" did nothing on a Deck) | `app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp` | on; `FROGTALE_SOFT_VSYNC=0` turns it off |
| Write the log to a file on Linux (Game Mode discards stdout) | `app/main.cpp` | — (`/tmp/Moonlight-*.log` inside the sandbox) |

Hold events go to `$XDG_CACHE_HOME/frogtale-reorder.log` as `epoch_ms outcome held_ms packets frame`.
Delay reports of 15 ms or more are logged there as `epoch_ms delay queue_ms samples`.

## Results (Steam Deck OLED, Starlink, 90 fps HDR, 12 Mbps, 4 W)
| | stutter events | median handoff pause |
|---|---|---|
| Stock Moonlight + stock Apollo (18 Mbps) | ~2.1 IDR/min | — |
| NACK | ~0 IDR | 64 ms |
| NACK + host copies at 30/60 ms around handoffs | ~0 IDR | **9 ms** |

With Vulkan decode at a 3 W TDP: Moonlight CPU 131% → 84% of a core, GPU 46% → 30%.

## Build
```
cd flatpak
flatpak run --filesystem=$HOME org.flatpak.Builder --state-dir=$HOME/fpb-state --force-clean --ccache \
  --disable-rofiles-fuse --repo=$HOME/mlrepo $HOME/mlbuilddir com.frogtale.MoonlightStarlink.json
flatpak build-bundle $HOME/mlrepo MoonlightStarlink.flatpak com.frogtale.MoonlightStarlink \
  --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
```
Use `org.flatpak.Builder`, not Ubuntu's `flatpak-builder` 1.4.2. The older one installs cmake and meson
modules into `/app/lib64`, and qmake then can't find SDL2_ttf.

Moonlight is GPL-3.0, and so is everything here.
