#!/bin/sh
set -eu
project=${1:?Falta la ruta Linux del proyecto}
cache=/var/cache/magictupper-devkit/image
bundle=/var/cache/magictupper-devkit/bundle
rootfs=$bundle/rootfs
mod=$project/switch/sysmodule-remoteplay
titleid=42000000004D5452
out=$project/dist/atmosphere/contents/$titleid

test -f "$mod/Makefile"
test -d "$rootfs/opt/devkitpro/libnx"
mkdir -p "$rootfs/src-remoteplay/source" "$rootfs/src-remoteplay/include"
rm -rf "$rootfs/src-remoteplay/build"
cp "$mod/Makefile" "$rootfs/src-remoteplay/Makefile"
cp "$mod/magicTupper-remoteplay.json" "$rootfs/src-remoteplay/magicTupper-remoteplay.json"
cp "$mod/source/"*.cpp "$rootfs/src-remoteplay/source/"
cp "$mod/include/"*.hpp "$rootfs/src-remoteplay/include/"

if [ ! -e "$rootfs/dev/null" ]; then
    mknod "$rootfs/dev/null" c 1 3
    chmod 666 "$rootfs/dev/null"
fi

chroot "$rootfs" /usr/bin/env \
    DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/portlibs/switch/bin:/opt/devkitpro/devkitA64/bin:/usr/local/bin:/usr/bin:/bin \
    /bin/sh -c 'cd /src-remoteplay && make clean && make -j1'

test -s "$rootfs/src-remoteplay/magicTupper-remoteplay.nsp"
mkdir -p "$out/flags"
cp "$rootfs/src-remoteplay/magicTupper-remoteplay.nsp" "$out/exefs.nsp"
: > "$out/flags/boot2.flag"
cp "$rootfs/src-remoteplay/magicTupper-remoteplay.nsp" "$mod/magicTupper-remoteplay.nsp"
printf 'Sysmodule generado: %s/exefs.nsp\n' "$out"
printf 'Copia %s/dist/atmosphere al raíz de la SD y reinicia Atmosphère.\n' "$project"
