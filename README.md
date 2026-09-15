@'
# magicTupper

magicTupper reúne una aplicación homebrew para Nintendo Switch, clientes para Windows y Android/Android TV y un servicio opcional desplegable con Docker Compose.

## Funciones principales

- Exploración y transferencia de archivos entre Switch, PC y servidor.
- Remote Play por red con vídeo H.264, audio PCM y soporte de mandos.
- Captura de HOME/interfaz del sistema cuando el stream H.264 no está activo.
- Cliente Android para móvil, tablet y Android TV.
- RCM Payload Injector en Android móvil/tablet.
- En **Explorar**, **Montar XCI · próximamente** aparece como función futura y no está implementada en 1.1.0.

## Instalación rápida

### Nintendo Switch

La carpeta `SD/` contiene todo lo necesario:

```text
SD/
├── atmosphere/
│   └── contents/42000000004D5452/
│       ├── exefs.nsp
│       └── flags/boot2.flag
└── switch/
    └── magicTupper/
        └── magicTupper.nro