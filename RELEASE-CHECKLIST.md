# Release checklist

Antes de publicar una release:

1. Compilar el NRO desde el árbol saneado y copiarlo como `Downloads/magicTupper.nro`.
2. Publicar el PC Client desde el árbol saneado y copiarlo como `Downloads/magicTupper-pc-client.exe`.
3. Compilar la APK desde el mismo commit y copiarla como `Downloads/magicTupper.apk`.
4. Verificar `Downloads/compose_magictupper.yml`.
5. Revisar que no existan credenciales, nombres de usuario, rutas locales o direcciones de una instalación real.
6. Generar SHA-256 de los cuatro artefactos.
