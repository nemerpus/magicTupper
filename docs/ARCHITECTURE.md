# Arquitectura consolidada

## Cliente Windows

El cliente .NET 8 contiene dos familias de funciones:

1. **Transferencia/administración** mediante los protocolos MTUSB1/MTP2 y transportes USB/TCP.
2. **Remote Play por red**, integrado en la interfaz principal.

Remote Play desacopla recepción UDP, decodificación H.264 y reproducción PCM para evitar que el trabajo del decoder bloquee la recepción de paquetes.

## Nintendo Switch

### NRO

`switch/source/` contiene la aplicación homebrew principal. Sus fuentes cubren exploración local/PC, jobs, actualización, instalación, fuentes HTTP y protocolo USB.

### Sysmodule Remote Play

`switch/sysmodule-remoteplay/` ejecuta el servicio persistente de Remote Play bajo Atmosphère.

- Title ID: `42000000004D5452`
- Vídeo: `grc:d` / H.264, actualmente orientado al juego.
- Audio: PCM S16LE, 48 kHz, estéreo; captura separada del vídeo.
- Mandos: UDP 8767, hasta cuatro slots en cliente; HDLS/`hid:dbg` en Switch.
- Failsafe: estado neutro cuando dejan de llegar paquetes y desconexión posterior del mando virtual.

## Flujo Remote Play

```text
Switch game -> grc:d video -> H.264 -> UDP/8768 -> PC -> Media Foundation -> preview/fullscreen
Switch audio -> grc:d audio -> PCM/RTP -> UDP/8769 -> PC -> jitter/buffer -> NAudio
PC controller -> UDP/8767 -> Switch sysmodule -> virtual controller
```

## Límite actual: HOME

`grc:d` no representa el compositor final de Horizon y por ello el HOME y determinados applets no aparecen en este backend. El siguiente backend experimental deberá investigar VI/Binder/NvMap/NvServices y la superficie final del compositor, manteniendo GRC como fallback estable.
