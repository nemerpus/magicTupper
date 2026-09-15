# Remote Play Phase 2 — mando virtual en segundo plano

## Objetivo

`magicTupper-remoteplay` se ejecuta como sysmodule de Atmosphère. El PC Client continúa enviando MTRP v1 a UDP/8767, pero ya no es necesario mantener abierto el NRO.

## Flujo

```text
DS4 / Xbox / HID -> PC Client -> UDP/8767 -> sysmodule -> HDLS virtual Pro Controller -> juego
```

## Instalación

1. Compila con `scripts/build-remoteplay-sysmodule-wsl.sh`.
2. Copia `dist/atmosphere/` al raíz de la microSD.
3. Reinicia la consola completamente/Atmosphère.
4. Abre un juego. No abras magicTupper.nro para esta prueba.
5. En PC Client abre Remote Play LAB, introduce la IP de la Switch y pulsa **Iniciar mando remoto**.
6. El juego debería detectar un Pro Controller adicional.

## Seguridad de entrada

- 250 ms sin paquetes: estado neutro.
- 2 s sin paquetes: mando virtual desconectado.
- Paquetes fuera de orden no reemplazan un estado más reciente.

## Limitación rc.5

Esta fase valida exclusivamente el mando virtual. Vídeo/audio todavía no forman parte del sysmodule.
