# magicTupper 0.4.0-rc.7.7 — HOME capture probe

- Keeps the rc7.6 NPDM fix that makes `capsscInitialize()` succeed.
- Expands the sysmodule inner heap from 3 MiB to 9 MiB and the HOME JPEG workspace to 4 MiB, matching the headroom used by the working switch-ocr sysmodule reference.
- Instruments `ViLayerStack_Default` and `ViLayerStack_Screenshot` separately, logging BEGIN/END, Result, JPEG byte count, and elapsed milliseconds.
- Sends a successful Default JPEG immediately; otherwise falls back to the Screenshot result.
- Does not change the PC video decoder, GRC H.264 path, audio path, controller protocol, or ports.
