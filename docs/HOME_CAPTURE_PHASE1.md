# HOME capture – Phase 1

Experimental proof of path for capturing HOME/system UI without disturbing the stable `grc:d` game stream.

## Design

- Games: unchanged `grc:d` H.264 720p30.
- When `grc:d` has no active game, the sysmodule opens `caps:sc`.
- It calls `capsscCaptureJpegScreenShot(..., ViLayerStack_Default, ...)`.
- `ViLayerStack_Default` is the default compositor layer stack.
- JPEG frames are fragmented as `MTJ1` UDP datagrams and reassembled by the Windows client.
- Probe rate is intentionally limited to ~2 FPS. This is a validation path, not the final streaming backend.

## Goal

If HOME appears correctly, Phase 2 will replace JPEG polling with a lower-level VI/NV buffer path suitable for real-time hardware encoding. If capture fails, the exact result code is sent to the PC and logged in `sdmc:/config/magicTupper/remoteplay-sysmodule.log`.
