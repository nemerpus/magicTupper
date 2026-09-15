magicTupper
magicTupper es un ecosistema homebrew para Nintendo Switch con clientes para Windows, Android y Android TV, Remote Play y herramientas adicionales.
Funciones principales
Exploración y transferencia de archivos entre Switch, PC y servidor.
Remote Play por red con vídeo H.264, audio PCM y soporte de mandos.
Captura de HOME/interfaz del sistema cuando el stream H.264 no está activo.
Cliente Android para móvil, tablet y Android TV.
RCM Payload Injector para dispositivos Android compatibles con USB Host.
En Explorar, Montar XCI · próximamente aparece como función futura y no está implementada en 1.1.0.
Instalación rápida
Nintendo Switch
Todo lo necesario para Switch está en `Downloads/SD/`:
```text
Downloads/SD/
├── atmosphere/
│   └── contents/42000000004D5452/
│       ├── exefs.nsp
│       └── flags/boot2.flag
└── switch/
    └── magicTupper/
        └── magicTupper.nro
```
Copia el contenido de `Downloads/SD/` a la raíz de la microSD y reinicia completamente la consola para cargar el sysmodule. Después abre magicTupper desde Homebrew Menu.
Android / Android TV
Instala `Downloads/magicTupper-Android.apk`. El mismo APK funciona en móvil, tablet y Android TV. El RCM Payload Injector se oculta en Android TV.
Windows
Descomprime `Downloads/magicTupper-PC.zip` y ejecuta el cliente incluido. Es un paquete Windows x64 autocontenido.
Docker
`Downloads/compose_magictupper.yml` contiene un despliegue genérico. Ajusta rutas, usuarios, permisos y configuración para tu entorno.
Descargas
Elemento	Uso
`Downloads/SD/`	Paquete Switch listo para copiar a la microSD
`Downloads/magicTupper-Android.apk`	Cliente Android / Android TV
`Downloads/magicTupper-PC.zip`	Cliente Windows x64
`Downloads/compose_magictupper.yml`	Docker Compose genérico
Arquitectura
`Downloads/` — distribución lista para utilizar.
`switch/` — código fuente del NRO.
`switch/sysmodule-remoteplay/` — sysmodule de Atmosphère para Remote Play.
`pc-client/` — cliente Windows .NET 8.
`android-client/` — cliente Android y RCM Injector.
`server/` — servicio Node.js.
`deploy/` — despliegue y configuración de ejemplo.
`docs/` — documentación técnica.
Puertos
Puerto	Transporte	Uso
8765	TCP	Servicio/biblioteca opcional
8766	TCP	Intercambio y control
8767	UDP	Mandos y ACK/RTT
8768	UDP	Vídeo/control Remote Play
8769	UDP	Audio Remote Play
Compilación
Consulta `docs/BUILD.md`.
Windows: .NET 8 / win-x64.
Android: Android Studio / JDK 17.
Nintendo Switch: devkitPro / libnx.
Privacidad
Las configuraciones publicadas son genéricas. No se incluyen credenciales ni configuraciones privadas de una instalación real.
Estado
Versión estable: 1.1.0
Remote Play y RCM Payload Injector han sido validados en hardware real.