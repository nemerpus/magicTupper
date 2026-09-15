# magicTupper Android / Android TV

Cliente nativo experimental para magicTupper Remote Play rc7.10.

- Juego: RTP/H.264 PT96 -> MediaCodec (SurfaceView)
- HOME/system UI: MTJ1/JPEG -> BitmapFactory, latest-frame
- Audio: RTP PT97 PCM S16LE 48 kHz stereo -> AudioTrack low latency
- Mando: MTRP v2 UDP/8767; Android gamepads/Android TV remotes compatibles con KeyEvent/joystick
- Un único APK para móvil, tablet y Android TV.

## Compilar
Abrir `android-client` en Android Studio o ejecutar `./gradlew assembleDebug` si se dispone de Gradle wrapper/SDK Android.
APK esperado: `app/build/outputs/apk/debug/app-debug.apk`.

## Uso
Switch y Android deben estar en la misma red. Inicia el sysmodule rc7.10, abre la app, introduce la IP de la Switch y pulsa Conectar. En Android TV conecta un mando Bluetooth/USB al televisor.

## Compilación por consola (Windows)
Desde `android-client` ejecuta `./gradlew.bat assembleDebug` en PowerShell. El bootstrap incluido descarga Gradle 8.9 la primera vez y reutiliza la copia de `%USERPROFILE%\\.gradle` en las siguientes compilaciones. Si `JAVA_HOME` no está definido, intenta usar automáticamente el JBR incluido con Android Studio.

APK debug: `app\\build\\outputs\\apk\\debug\\app-debug.apk`.


## rc7.10a Android diagnostics
Surface lifecycle guard, persistent session telemetry, RTP gap/IDR recovery, single latest-JPEG worker, H264/HOME visibility switching and detailed logcat diagnostics.


## rc7.10b
Fixes Android NetworkOnMainThreadException by moving initial UDP HELLO/START datagrams off the UI thread.


## rc7.10c
Fixes HOME MTJ1 frame-id sentinel comparison (all normal frame IDs were being discarded), moves STOP off UI thread, and adds packet classification/audio receive telemetry.


## rc7.10d
Critical Android RTP fix: Java bytes are signed, so RTP version checks using `(byte >> 6) == 2` rejected every normal RTP packet beginning with 0x80. Checks now mask with `(byte & 0xC0) == 0x80` for both video and audio.


## rc7.10e
Stability baseline keeps the proven rc7.10d RTP/audio/video path unchanged. Removes all on-screen touch controls. Physical Android/PS4/Xbox face-button positions are translated to Nintendo layout: bottom=B, right=A, left=Y, top=X.


## rc7.10f
- Adds an explicit **Pantalla completa** button. It hides the complete control bar and Android system UI; Back exits fullscreen.
- Keeps touch game controls removed.
- Expands standard Android gamepad support: Xbox/PS/8BitDo/Switch Pro/generic HID through Android KeyEvent/joystick APIs.
- START/Menu -> Switch PLUS, SELECT/Back -> Switch MINUS, MODE/Guide when exposed by Android -> Switch HOME, thumb clicks -> L3/R3.
- Face positions remain translated to Nintendo layout (bottom B, right A, left Y, top X).
- Streaming/audio/RTP pipeline remains unchanged from the working rc7.10d/e baseline.


## rc7.10g
HOME wire diagnostics: logs first unknown video datagrams and MTJ-like headers while leaving working H264/audio path unchanged. Used to identify why caps:sc HOME packets are not reaching/being recognized by Android.
