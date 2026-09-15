# magicTupper 1.1.0 — RCM Payload Injector

Android incorpora un inyector RCM independiente de Remote Play.

- Detecta NVIDIA APX `0955:7321`.
- Preset Hekate 6.5.3 (predeterminado).
- Preset Atmosphère fusee 1.11.2.
- Selector de cualquier `.bin` local.
- Los presets oficiales se descargan una sola vez desde GitHub y quedan en caché privada de la app.
- Muestra SHA-256 antes de inyectar.
- Nunca auto-inyecta: requiere pulsar **Inyectar payload**.
- El motor RCM usa el flujo Fusée Gelée/Intermezzo para Erista vulnerable.

## Compatibilidad

Hekate 6.5.3 y Atmosphère 1.11.2 soportan HOS hasta 22.5.0. HOS 23.0.0 aún estaba en proceso de soporte cuando se preparó esta versión (15-09-2026), por lo que no se etiqueta como compatible.

El inyector RCM no convierte una consola parcheada en vulnerable. Mariko/OLED/Lite y Erista parcheada requieren su método de arranque correspondiente.

## Android build

El módulo usa una pequeña biblioteca JNI para el trigger USB RCM. Android SDK debe tener CMake y NDK instalados.
