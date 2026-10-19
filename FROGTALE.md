# FrogtaleTV Starlink build of Moonlight for Apple TV

This is the Apple TV version of the Steam Deck Starlink build
([aesse94/moonlight-qt](https://github.com/aesse94/moonlight-qt), branch `frogtale-starlink`). It works
with the matching Apollo fork ([aesse94/Apollo](https://github.com/aesse94/Apollo), branch
`frogtale-starlink`), and installs as its own app (**Moonlight Starlink**, `com.frogtale.MoonlightStarlink`),
next to stock Moonlight.

Target device: **Apple TV 4K (2nd generation, 2021, A12)**. Any Apple TV 4K or the Apple TV HD runs it too;
the Apple TV HD is held to 1080p. (The original 2010 "Apple TV 2nd generation" can't run apps at all.)

## What carries over from the Deck build
| Change | Where | Apple TV setting |
|---|---|---|
| NACK lost video packets, re-NACK 2 × 35 ms, queueing delay reports (`FTDL`) for Apollo's adaptive bitrate | `moonlight-common-c` submodule (`aesse94/moonlight-common-c`, `frogtale-starlink`) | **NACK Lost Video Packets** (on). Needs the patched Apollo host |
| Hold later-frame packets while the current FEC block is incomplete (100 ms with NACK, 160 ms at handoffs, storm mode) | same | always on (80 ms without NACK) |
| Learn the Starlink handoff phase from video packet gaps | same | always on |
| Handoff cushion: bank frames before each predicted handoff, play them through the stall, stretch them while it lasts, trim the burst back afterwards; depth adapts 4 → 7 frames | `Limelight/Stream/VideoDecoderRenderer.m` | **Handoff Cushion** (on). Needs **Frame Pacing: Smoothest Video** |

Not ported, because the Apple TV doesn't need it: Vulkan decode (tvOS decodes with VideoToolbox), the X11
software vsync (tvOS paces off `CADisplayLink`), 10 ms Opus (no TDP limit), log to file (Xcode shows the log).

The settings are in the Apple TV's **Settings → Apps → Moonlight Starlink**. This build's defaults differ
from stock: HEVC (hardware decoded on the A12, and better than H.264 at Starlink bitrates), 15 Mbps,
Smoothest Video, both Starlink switches on.

### Cushion depth on Apple TV
The Deck banks decoded frames in Moonlight's pacer. On tvOS the pacing queue is moonlight-common-c's decode
unit queue, which flushes and requests an IDR frame at 15 frames, so the cushion is 4–7 frames
(67–117 ms at 60 fps) and catches up whenever the queue reaches 12. Modelled at 60 fps, it keeps the longest
visible freeze at ~33 ms for stalls up to ~200 ms. Stalls of 220 ms+ overflow that queue with or without
the cushion (stock tvOS Moonlight too).

The Apple TV 4K outputs at most 60 Hz, so stream at 60 fps (the Deck streams at 90).

### Logs
Each handoff logs a `FrogtaleTV cushion: maxgap=… maxqueue=… banked=… stretched=… phase=… target=…` line
to the Xcode console. Hold and delay events go to `Library/Caches/frogtale-reorder.log` in the app
container (Xcode → Devices and Simulators → the Apple TV → Moonlight Starlink → Download Container); it
starts over once it passes 4 MB.

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
