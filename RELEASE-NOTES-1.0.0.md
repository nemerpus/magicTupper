# magicTupper 1.0.0 Stable

Primera versión marcada para distribución estable, basada en rc7.10j.

## Remote Play Android
- HOME por JPEG/MTJ1 y juegos por H.264/RTP.
- Audio PCM 48 kHz estéreo.
- Mando físico Android/Android TV y disposición Nintendo.
- Pantalla táctil remota multitáctil con refresco periódico, liberación redundante y watchdog en Switch.
- Interfaz de distribución rediseñada: identidad magicTupper, logo del NRO, barra compacta, controles consistentes y área de vídeo limpia.
- El área táctil queda limitada al display; la barra superior ya no comparte el listener táctil.
- Pantalla completa y salida integradas.

## Switch sysmodule
- Mantiene el transporte probado de rc7.10j.
- HOME resume watchdog, captura caps:sc, H.264/audio y MTRP sin cambios funcionales respecto a la base validada.
- Marcador de compilación: 1.0.0-stable.

## Auditoría de estabilidad
Se evitó alterar los caminos críticos ya validados de vídeo H.264, audio, MTJ1 y mando. Los cambios funcionales de esta entrega se concentran en UI/branding y aislamiento del listener táctil.
