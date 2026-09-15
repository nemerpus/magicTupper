# Changelog

## 0.4.0-rc.7.10-unified

- Unified release based on the validated rc7.9 HOME + game implementation.
- HOME `caps:sc`/`ViLayerStack_Default` capture now runs unpaced at the natural capture cadence; no fixed 50 ms frame scheduler.
- Removed the obsolete startup `Default + Screenshot` double-capture diagnostic; `Default` is the validated production stack.
- HOME JPEG decoding moved off the UDP receive loop to a dedicated capacity-1 latest-frame queue.
- New complete HOME frames supersede stale complete frames; newer MTJ1 fragments already supersede incomplete older frames.
- H.264 remains the priority backend and the stable game decoder pipeline is otherwise unchanged.
- HOME capture polling while H.264 is active reduced to a 10 ms responsiveness check without performing compositor captures.
- Future lower-latency path remains direct VI/NV compositor/framebuffer capture if caps:sc latency is still perceptible.

## 0.4.0-rc.7.9-unified

Unified project snapshot containing the complete current magicTupper codebase.

### Remote Play
- Stable game capture path: `grc:d` -> H.264/RTP, 720p30.
- HOME/system compositor capture: `caps:sc` + `ViLayerStack_Default` -> JPEG/MTJ1.
- HOME capture target: 20 FPS with capture time included in frame pacing.
- PC HOME path uses latest-frame-wins semantics to reduce stale JPEG latency.
- Automatic preference for H.264 while game frames are active; HOME JPEG is the fallback.
- Integrated Remote Play panel in the PC client with fullscreen, audio controls and P1-P4 controls.
- Separate network audio capture/playback path and controller transport.

### Project layout
- `pc-client/`: Windows .NET 8 desktop client.
- `switch/`: Switch NRO plus Remote Play sysmodule.
- `server/`: server/admin components.
- `deploy/`: deployment/configuration material.
- `scripts/`: build/deployment helpers.
- `docs/`: architecture, build notes and historical RC notes.

Historical experimental changelogs were moved to `docs/history/` so this archive is a single coherent project rather than a chain of detached fixes.

## Android client
- Añadido `android-client/`: cliente universal Android/Android TV para Remote Play directo a Switch.
- MediaCodec H.264, MTJ1/JPEG HOME, AudioTrack PCM y mando MTRP v2.
