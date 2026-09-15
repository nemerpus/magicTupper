# Preparación de publicación en GitHub

Este árbol es la base estable 1.1.0. Antes de publicar la release definitiva:

1. Compilar `switch/magictupper.nro`.
2. Compilar `android-client/app/build/outputs/apk/debug/app-debug.apk` (o release firmada cuando exista).
3. Publicar el PC Client .NET 8 win-x64 self-contained.
4. Compilar el sysmodule de Remote Play y conservar la estructura `atmosphere/contents/42000000004D5452/`.
5. Validar el Compose y sus ficheros auxiliares de `deploy/`/`server/`.
6. Reempaquetar y volver a proporcionar el árbol con esos artefactos para generar README, guía de uso, instalación, arquitectura, sección Software/Descargas, release notes y hashes definitivos.

No se deben presentar como finales los binarios antiguos que puedan quedar en el árbol hasta completar esta recompilación.
