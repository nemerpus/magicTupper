# libusbhsfs 0.2.10

Official source: https://github.com/DarkMatterCore/libusbhsfs/releases/tag/v0.2.10

Archive: libusbhsfs_0.2.10-main-3b897ed-src.tar.bz2
SHA-256: a76a115521f38532d4c6e4067627953cd84ca7e50ee3a8f34ba90ecc8c89c68b

Built with BUILD_TYPE=ISC against the project devkitPro toolchain, with the local standard INQUIRY compatibility patch described below. FAT32/exFAT support; no NTFS/EXT modules linked.

## ISC license

Copyright (c) 2020-2023, DarkMatterCore <pabloacurielz@gmail.com>.
Copyright (c) 2020-2021, XorTroll.
Copyright (c) 2020-2021, Rhys Koedijk.

Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.


## FatFs license

/*----------------------------------------------------------------------------/
/  FatFs - Generic FAT Filesystem module  R0.16                               /
/-----------------------------------------------------------------------------/
/
/ Copyright (C) 2025, ChaN, all right reserved.
/
/ FatFs module is an open source software. Redistribution and use of FatFs in
/ source and binary forms, with or without modification, are permitted provided
/ that the following condition is met:

/ 1. Redistributions of source code must retain the above copyright notice,
/    this condition and the following disclaimer.
/
/ This software is provided by the copyright holder and contributors "AS IS"
/ and any warranties related to this software are DISCLAIMED.
/ The copyright owner or contributors be NOT LIABLE for any damages caused
/ by use of this software.
/
/----------------------------------------------------------------------------*/

## Local change in MagicTupper 0.3.3

Standard INQUIRY requests 36 bytes instead of 44; optional serial bytes stay zero. Exact transfer validation is unchanged. Reproducible patch: scripts/patch-usb-inquiry.py, applied after verifying the original archive SHA-256. See NOVEDADES-0.3.3.md for the observed device response.
