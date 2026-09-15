# Third-party RCM credits

The Android RCM implementation follows the publicly documented Fusée Gelée (CVE-2018-6242) host flow and the Android USB approach demonstrated by David Buchanan’s NXLoader. The 92-byte `intermezzo.bin` included under `android-client/app/src/main/assets/` is the relocator used by NXLoader.

- NXLoader: https://github.com/DavidBuchanan314/NXLoader (MIT)
- Fusée Launcher / Fusée Gelée research: https://github.com/reswitched/fusee-launcher
- Hekate preset is downloaded at runtime from the official CTCaer/hekate release.
- Atmosphère fusee preset is downloaded at runtime from the official Atmosphere-NX/Atmosphere release.

Hekate and Atmosphère binaries are not redistributed inside this source archive.
