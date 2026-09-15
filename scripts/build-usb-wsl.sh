#!/bin/sh
set -eu
project=${1:?Project path required}
rootfs=/var/cache/magictupper-devkit/bundle/rootfs
mkdir -p "$rootfs/usbhsfs-src"
archive="$project/switch/third_party/libusbhsfs-0.2.10-src.tar.bz2"
actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
test "$actual" = a76a115521f38532d4c6e4067627953cd84ca7e50ee3a8f34ba90ecc8c89c68b
tar -xf "$archive" -C "$rootfs/usbhsfs-src"
python3 "$project/scripts/patch-usb-inquiry.py" "$rootfs/usbhsfs-src"
chroot "$rootfs" /usr/bin/env DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64 PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/usr/bin:/bin /bin/sh -c 'cd /usbhsfs-src && make BUILD_TYPE=ISC install -j2'
test -s "$rootfs/opt/devkitpro/portlibs/switch/lib/libusbhsfs.a"
