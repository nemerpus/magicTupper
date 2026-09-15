# magicTupper

magicTupper reúne una aplicación homebrew para Nintendo Switch, clientes para Windows y Android/Android TV y un servicio opcional desplegable con Docker Compose.

## Funciones principales

- Exploración y transferencia de archivos entre Switch, PC y servidor.
- Remote Play por red con vídeo H.264, audio PCM y soporte de mandos.
- Captura de HOME/interfaz del sistema mediante JPEG cuando el stream H.264 no está activo.
- Cliente Android para teléfono, tablet y Android TV.
- RCM Payload Injector en Android móvil/tablet compatible con USB Host. La opción se oculta en Android TV.
- En **Explorar**, la entrada **Montar XCI · próximamente** reserva la siguiente línea de desarrollo; en 1.1.0 no realiza ningún montaje.

## Descargas

Los artefactos de una release se publican con nombres estables:

| Archivo | Uso |
| --- | --- |
| `magicTupper.nro` | Aplicación homebrew de Nintendo Switch |
| `magicTupper.apk` | Cliente Android / Android TV |
| `magicTupper-pc-client.exe` | Cliente Windows x64 autocontenido |
| `compose_magictupper.yml` | Despliegue genérico del servicio magicTupper |

> El Compose y los ejemplos del repositorio usan datos ficticios. Ajusta rutas, usuario, permisos y configuración a tu entorno antes de desplegar.

## Arquitectura

- `switch/` — código del NRO.
- `switch/sysmodule-remoteplay/` — sysmodule de Atmosphère para Remote Play.
- `pc-client/` — cliente Windows .NET 8.
- `android-client/` — cliente Android y RCM Injector.
- `server/` — servicio Node.js.
- `deploy/` — Compose y configuración de ejemplo.
- `docs/` — arquitectura, compilación y documentación técnica.
- `Downloads/` — artefactos listos para distribuir en cada release.

## Puertos

| Puerto | Transporte | Uso |
| --- | --- | --- |
| 8765 | TCP | servicio/biblioteca opcional |
| 8766 | TCP | intercambio y control principal |
| 8767 | UDP | mandos y ACK/RTT |
| 8768 | UDP | vídeo/control Remote Play |
| 8769 | UDP | audio Remote Play |

## Compilación

Consulta [`docs/BUILD.md`](docs/BUILD.md). El cliente Windows se publica para `win-x64` como ejecutable autocontenido; Android requiere Android Studio/JDK 17; NRO y sysmodule requieren devkitPro/libnx.

## Privacidad y configuración

El repositorio público no incluye credenciales, hashes de contraseñas, nombres de equipos ni rutas de una instalación real. `server/config.example.json` y `deploy/config/config.example.json` son plantillas que deben copiarse y personalizarse localmente.

## Estado

Versión estable de referencia: **1.1.0**. Remote Play y RCM Injector FIX8 han sido validados en hardware real.
