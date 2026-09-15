# Remote Play — estado actual

## Funcional

- Transporte solo por red para Remote Play.
- Vídeo H.264 a través de UDP/8768.
- Audio PCM S16LE 48 kHz estéreo con socket dedicado.
- Reproducción de audio con prebuffer/rebuffer para evitar underruns continuos.
- Telemetría RX/PLAY/TRIM, buffer, gap/late/reorder/skip y vídeo loss/qdrop.
- Hasta cuatro slots de mando desde el cliente.
- Miniatura integrada en la ventana principal y pantalla completa al seleccionar el stream.
- Controles separados de vídeo, audio y mandos.
- Selector de salida de audio.
- IP reutilizada desde la conexión principal del cliente.

## Comportamiento de audio estabilizado

La rama consolidada conserva el diseño de la última revisión estable: captura alrededor del nominal PCM de 192000 B/s y evita el ciclo de underrun/trim que producía el sonido metálico de revisiones anteriores.

## Pendiente

- Validar y pulir las rascadas de audio residuales en redes/equipos distintos.
- Completar rumble generado por el juego para todos los tipos de mando físico.
- Backend alternativo capaz de capturar HOME/applications del sistema.
- Cliente Android TV todavía no incluido.
