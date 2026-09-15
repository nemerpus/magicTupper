# MagicTupper PC Client

Cliente Windows portable para el futuro modo **Conectar SD al PC**.

El ejecutable final se llamará `MagicTupper-PC-Client.exe`. El protocolo `MTUSB1` usa tramas con cabecera, versión, comando y longitud; las rutas son siempre relativas a `sdmc:/` y no aceptan `..`, barras invertidas ni rutas absolutas.

Esta fase define el formato de mensajes. El transporte USB y el servidor de comandos del NRO deben completarse antes de que el cliente pueda navegar la SD.

Compilación:

```powershell
dotnet publish .\MagicTupper-PC-Client.csproj -c Release -r win-x64 --self-contained true /p:PublishSingleFile=true
```

## Remote Play LAB (fase 1)

La versión de desarrollo incorpora un canal de mando remoto por UDP/8767. Abre **Remote Play / mando**, usa la IP de la Switch y conecta un mando XInput. Esta fase mide RTT y valida transporte; todavía no inyecta el mando en juegos. Consulta `../docs/REMOTE_PLAY_PHASE1.md`.
