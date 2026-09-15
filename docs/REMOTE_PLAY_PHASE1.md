# MagicTupper Remote Play — Fase 1 (mando por red)

Esta fase valida el plano de entrada antes de implementar la captura de vídeo y la inyección de un mando virtual en Horizon.

## Arquitectura

- `TCP/8766`: se mantiene para el protocolo actual de archivos/control.
- `UDP/8767`: nuevo canal de baja latencia para estados de mando.
- PC: `RemotePlayForm` lee XInput y transmite un estado normalizado aproximadamente cada 8 ms.
- Switch: `remoteInputWorker` recibe paquetes `MTRP`, actualiza telemetría y devuelve un ACK por paquete.
- El PC calcula RTT a partir del timestamp eco del ACK.

El receptor de Switch **todavía no inyecta** botones en juegos. Esta separación es intencionada: primero medimos estabilidad, pérdida y latencia del camino PC -> Switch. La inyección se implementará en un sysmodule para que permanezca activa mientras el juego está en primer plano.

## Formato MTRP v1 / INPUT

Todos los enteros son little-endian. Tamaño fijo: 40 bytes.

| Offset | Tamaño | Campo |
|---:|---:|---|
| 0 | 4 | `MTRP` |
| 4 | 1 | versión = 1 |
| 5 | 1 | tipo = 1 (input) |
| 6 | 2 | tamaño = 40 |
| 8 | 4 | secuencia |
| 12 | 8 | timestamp monotónico del cliente, microsegundos |
| 20 | 4 | máscara de botones |
| 24 | 2 | LX int16 |
| 26 | 2 | LY int16 |
| 28 | 2 | RX int16 |
| 30 | 2 | RY int16 |
| 32 | 2 | LT uint16 |
| 34 | 2 | RT uint16 |
| 36 | 4 | reservado |

La Switch responde `MTRP`, versión 1, tipo `0x81`, tamaño 24, secuencia, timestamp original y `SDL_GetTicks()`.

## Prueba

1. Compilar el NRO y el PC Client.
2. Copiar `magictupper.nro` a `sd:/switch/magictupper/` y ejecutarlo.
3. Asegurar que PC y Switch están en la misma LAN.
4. En el NRO abrir la ayuda de conexión USB/RED. Debe aparecer `REMOTE PAD: esperando · UDP 8767`.
5. En PC indicar la IP de la Switch y abrir `Remote Play / mando`.
6. Conectar un mando Xbox/XInput por USB o Bluetooth.
7. Pulsar `Iniciar mando remoto`.
8. En Switch debe cambiar a `REMOTE PAD: ACTIVO` y aumentar `seq`.
9. En PC debe aumentar `TX` y `ACK`; anotar el RTT.

Para la primera prueba interesa: RTT medio, si ACK queda muy por detrás de TX, tipo de conexión de la Switch (Ethernet/Wi-Fi) y modelo de mando.

## Seguridad ante desconexión

El PC envía tres estados neutros al detener el emisor. Además la Switch considera el mando inactivo si no recibe paquetes durante 1 segundo. La futura capa de inyección tendrá un watchdog más corto para liberar botones si desaparece el cliente.
