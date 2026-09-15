# Remote Play — current implementation (rc7.9 unified)

magicTupper currently has two video capture paths sharing the same integrated PC Remote Play UI.

## Game video

The primary game path uses Nintendo Switch `grc:d` capture and sends H.264 over RTP/UDP. The PC receive/decode path is deliberately kept isolated from the HOME JPEG work because it is the known-good low-latency game path.

## HOME and system UI

HOME/system compositor capture uses `caps:sc` with `ViLayerStack_Default`. The sysmodule captures JPEG frames and fragments them into the magicTupper `MTJ1` UDP format. The PC reassembles them and feeds the resulting bitmap to the same Remote Play panel.

Observed on the current test console, a HOME JPEG capture normally takes roughly 40–45 ms. rc7.10 removes artificial HOME frame pacing entirely: a new caps:sc capture starts as soon as the previous capture returns. The PC reassembles UDP immediately and moves JPEG decoding to a dedicated capacity-1 latest-frame queue, so neither incomplete nor complete stale HOME frames are intentionally queued.

When recent H.264 game frames are present, the PC gives them priority and the HOME path is suspended/fallback-oriented. When game capture is unavailable, HOME JPEG becomes visible automatically.

## Current limitation

HOME capture is screenshot/JPEG based, so its latency cannot be expected to match the `grc:d` H.264 game path. Further HOME latency work should first optimize frame age/queueing; a larger architectural improvement would require investigating direct compositor/framebuffer capture rather than repeatedly increasing the JPEG target FPS.
