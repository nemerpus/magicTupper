# Third-party references

The integrated installer is implemented in `source/install.cpp`, `package.hpp`,
`cnmt.hpp` and `http_range.hpp`. Format layouts and service interfaces were
checked against libnx/Switchbrew and the MIT-licensed Adubbz/Tinfoil components
maintained in Huntereb/Awoo-Installer. The ES import and NS application-record
IPC calls follow those MIT components; their license is included below.

Reference: https://github.com/Huntereb/Awoo-Installer
Reference: https://switchbrew.github.io/libnx/

No Awoo executable, UI, artwork, audio or compression engine is bundled.
Existing linked libraries (libnx, libcurl, SDL and json-c) retain their licenses.

## Adubbz/Tinfoil MIT notice
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## USB support (0.3.1)

libusbhsfs 0.2.10, ISC build, includes FatFs. See third_party/USB-LICENSES.md for notices and source provenance. The original source archive is in switch/third_party for reproducible builds.

## Remote Play video research
The rc.6 video-preview implementation interoperates with Horizon's `grc:d` recording service. Protocol behavior and low-latency design were cross-checked against public SysDVR documentation and SysDVR-UVC-Capture research. No third-party binary is bundled in this archive. The PC preview optionally invokes a user-provided `ffplay.exe` (FFmpeg) executable.
