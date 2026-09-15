# magicTupper

magicTupper reÃºne una aplicaciÃ³n homebrew para Nintendo Switch, clientes para Windows y Android/Android TV y un servicio opcional desplegable con Docker Compose.

## Funciones principales

- ExploraciÃ³n y transferencia de archivos entre Switch, PC y servidor.
- Remote Play por red con vÃ­deo H.264, audio PCM y soporte de mandos.
- Captura de HOME/interfaz del sistema cuando el stream H.264 no estÃ¡ activo.
- Cliente Android para mÃ³vil, tablet y Android TV.
- RCM Payload Injector en Android mÃ³vil/tablet.
- En **Explorar**, **Montar XCI Â· prÃ³ximamente** aparece como funciÃ³n futura y no estÃ¡ implementada en 1.1.0.

## InstalaciÃ³n rÃ¡pida

### Nintendo Switch

La carpeta `SD/` contiene todo lo necesario:

```text
SD/
â”œâ”€â”€ atmosphere/
â”‚   â””â”€â”€ contents/42000000004D5452/
â”‚       â”œâ”€â”€ exefs.nsp
â”‚       â””â”€â”€ flags/boot2.flag
â””â”€â”€ switch/
    â””â”€â”€ magicTupper/
        â””â”€â”€ magicTupper.nro
