# magicTupper 0.4.0-rc.7.6 — HOME caps access

- Based on rc7.5; stable GRC/audio/video paths unchanged.
- NPDM `service_access` now mirrors the proven boot2 sysmodule policy used by `mariano54/switch-ocr`: wildcard `*`.
- Keeps `service_host` empty; magicTupper does not host arbitrary services.
- HOME probe still runs before blocking `mtGrcdTransfer()`.
- `caps:sc` init result is logged and sent to the PC.
- Capture tries `ViLayerStack_Default` first (HOME/system compositor target), then `ViLayerStack_Screenshot` as a diagnostic fallback matching switch-ocr.
- Capture timeout raised to 1 s to match the known implementation.

Research basis: switch-ocr sysmodule uses `service_access: ["*"]`, `force_debug: true`, `capsscInitialize()` and `capsscCaptureJpegScreenShot(... ViLayerStack_Screenshot, 1000000000LL)`. uLaunch/uScreen independently confirms that HOME/system-settings capture is possible, although via a different architecture.
