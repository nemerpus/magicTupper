# Compilación

## Requisitos del cliente Windows

- Windows x64
- .NET 8 SDK

```powershell
cd "$env:USERPROFILE\Desktop\magicTupper-unified\pc-client"
dotnet clean; Remove-Item -Recurse -Force .\bin -ErrorAction SilentlyContinue; Remove-Item -Recurse -Force .\obj -ErrorAction SilentlyContinue; dotnet restore; dotnet publish .\MagicTupper-PC-Client.csproj -c Release -r win-x64 --self-contained true /p:PublishSingleFile=true
```

## Requisitos del sysmodule/NRO

- WSL
- devkitPro/devkitA64
- libnx compatible

Sysmodule:

```powershell
wsl -u root bash -lc "cd /mnt/c/Users/usuario/Desktop/magicTupper && ./scripts/build-remoteplay-sysmodule-wsl.sh /mnt/c/Users/usuario/Desktop/magicTupper"
```

NRO/homebrew:

```powershell
wsl -u root bash -lc "cd /mnt/c/Users/usuario/Desktop/magicTupper && ./scripts/build-usb-wsl.sh /mnt/c/Users/usuario/Desktop/magicTupper"
```

## Instalación del sysmodule

Destino en SD:

```text
atmosphere/contents/42000000004D5452/exefs.nsp
atmosphere/contents/42000000004D5452/flags/boot2.flag
```

Después de sustituir el sysmodule debe realizarse un reinicio completo de Atmosphère.
