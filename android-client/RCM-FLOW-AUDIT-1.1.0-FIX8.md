# RCM FLOW AUDIT — 1.1.0 FIX8

FIX8 is rebuilt from the original RCM implementation that previously detected APX on the test Motorola.

- UsbManager.getDeviceList() is the source of truth.
- 500 ms foreground polling detects APX appearance/disappearance without Activity restart.
- No dependency on USB attach/detach broadcasts.
- Permission request is emitted only after 0955:7321 is visible.
- Permission PendingIntent is mutable on Android 12+ so UsbManager can return permission extras.
- Permission state is reset when APX disappears/re-enumerates.
- UI exposes visible USB device count, APX presence and permission state.
- Payload injection remains manual.
- Remote Play transport, H264, audio, touch and Switch sysmodule are unchanged.
