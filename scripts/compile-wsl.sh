#!/bin/sh
set -eu
project=${1:?Falta la ruta Linux del proyecto}
cache=/var/cache/magictupper-devkit/image
bundle=/var/cache/magictupper-devkit/bundle
test -f "$project/switch/Makefile"
command -v skopeo >/dev/null
command -v python3 >/dev/null
python3 "$project/scripts/check-utf8.py" "$project"
python3 - "$project/switch/assets/fondo.bmp" "$project/switch/assets/rick.bmp" "$project/switch/source/assets.hpp" <<'PY'
import pathlib
import sys

def emit(name, source):
    data = pathlib.Path(source).read_bytes()
    values = ','.join(str(value) for value in data)
    return f'static const unsigned char {name}[] = {{{values}}};\nstatic const size_t {name}_len = sizeof({name});\n'

pathlib.Path(sys.argv[3]).write_text('#pragma once\n#include <cstddef>\n' + emit('magictupper_fondo_bmp', sys.argv[1]) + emit('magictupper_rick_bmp', sys.argv[2]))
PY
if [ ! -f "$bundle/.magictupper-ready" ]; then
    if [ ! -f "$cache/manifest.json" ]; then
        mkdir -p "$cache"
        skopeo copy --override-os linux --override-arch amd64 docker://docker.io/devkitpro/devkita64:20260219 "dir:$cache"
    fi
    mkdir -p "$bundle/rootfs"
    # Sequential extraction keeps memory use low in this isolated build filesystem.
    # Overlay whiteout marker files are excluded from the extracted toolchain.
    for layer in $(python3 -c 'import json,sys; print(" ".join(x["digest"].split(":")[1] for x in json.load(open(sys.argv[1]))["layers"]))' "$cache/manifest.json"); do
        printf 'Extrayendo capa %.12s...\n' "$layer"
        tar -xf "$cache/$layer" -C "$bundle/rootfs" --exclude='dev/*' --exclude='*/.wh.*'
    done
    chroot "$bundle/rootfs" /opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ --version
    touch "$bundle/.magictupper-ready"
fi
rootfs=$bundle/rootfs
test -s "$rootfs/opt/devkitpro/portlibs/switch/lib/libusbhsfs.a" || { echo 'Falta libusbhsfs. Ejecuta scripts/build-usb-wsl.sh primero.'; exit 1; }
mkdir -p "$rootfs/src/source"
cp "$project/switch/Makefile" "$rootfs/src/Makefile"
cp "$project/switch/icon.jpg" "$rootfs/src/icon.jpg"
cp "$project/switch/source/"*.cpp "$project/switch/source/"*.hpp "$rootfs/src/source/"
mkdir -p "$project/dist/switch/magictupper/assets"
cp "$project/switch/assets/"*.bmp "$project/dist/switch/magictupper/assets/"
cp "$project/switch/assets/cacert.pem" "$project/dist/switch/magictupper/assets/"
cp "$project/switch/assets/CA-NOTICE.md" "$project/dist/switch/magictupper/"
if [ ! -e "$rootfs/dev/null" ]; then
    mknod "$rootfs/dev/null" c 1 3
    chmod 666 "$rootfs/dev/null"
fi
chroot "$rootfs" /usr/bin/env \
    USB_DEBUG="${USB_DEBUG:-0}" \
    DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:/opt/devkitpro/devkitA64/bin:/usr/local/bin:/usr/bin:/bin \
    /bin/sh -c 'cd /src && make USBHSFS=1 -j1'
test -s "$rootfs/src/magictupper.nro"
mkdir -p "$project/dist/switch/magictupper"
cp "$rootfs/src/magictupper.nro" "$project/switch/magictupper.nro"
cp "$rootfs/src/magictupper.nro" "$project/dist/switch/magictupper/magictupper.nro"
# Copy NACP for proper Atmosphere/hbmenu recognition
cp "$rootfs/src/magictupper.nacp" "$project/switch/magictupper.nacp"
cp "$rootfs/src/magictupper.nacp" "$project/dist/switch/magictupper/magictupper.nacp"
printf 'NRO generado: %s/dist/switch/magictupper/magictupper.nro\n' "$project"
