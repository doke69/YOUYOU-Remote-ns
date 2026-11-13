# Third-party software

Application code is licensed under GPL-3.0-or-later; see LICENSE. Linked dependencies retain their own licenses. The supplied devkitPro FFmpeg build is GPL-enabled. Exact source archive revisions and hashes are recorded in scripts/sources.lock.json; SDK package versions and hashes are in scripts/sdk-windows.lock.json.

| Dependency | Upstream | License |
| --- | --- | --- |
| Noto Sans SC Regular / Bold (bundled, unmodified) | https://github.com/notofonts/noto-cjk/tree/main/Sans/SubsetOTF/SC | SIL Open Font License 1.1; see assets/fonts/OFL.txt |
| deko3d | https://github.com/devkitPro/deko3d | zlib; see licenses/deko3d-zlib.txt |
| dekotools / uam (build tool) | https://github.com/devkitPro/dekotools | GPL-3.0; see licenses/dekotools-GPL-3.0.txt |
| libnx | https://github.com/switchbrew/libnx | ISC |
| SDL2 / SDL2_ttf | https://github.com/libsdl-org | zlib |
| FFmpeg (Horizon/NVTEGRA build) | https://github.com/devkitPro/pacman-packages/tree/master/switch/ffmpeg | GPL-2.0-or-later build |
| libdatachannel / libjuice | https://github.com/paullouisageneau | MPL-2.0 |
| Mbed TLS | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 OR GPL-2.0-or-later |
| usrsctp | https://github.com/sctplab/usrsctp | BSD-3-Clause |
| libSRTP | https://github.com/cisco/libsrtp | BSD-3-Clause |
| plog | https://github.com/SergiusTheBest/plog | MIT |
| nlohmann/json | https://github.com/nlohmann/json | MIT |
| QR Code generator | https://github.com/nayuki/QR-Code-generator | MIT |
| Experimental WebRTC receive bitrate estimator subset | https://webrtc.googlesource.com/src/+/218b56e516386cd57c7513197528c3124bcd7ef3 | BSD-3-Clause; PATENTS and AUTHORS retained |
| Abseil for the estimator subset | https://chromium.googlesource.com/chromium/src/third_party/+/fc733299410a7104a0848539baab0131b8a616b8/abseil-cpp/ | Apache-2.0 |
| Opus | https://opus-codec.org | BSD-3-Clause |
| libcurl | https://curl.se | curl license |
| FreeType / HarfBuzz | https://freetype.org / https://harfbuzz.github.io | FTL or GPL / MIT |
| Mesa / libdrm | https://www.mesa3d.org / https://gitlab.freedesktop.org/mesa/drm | MIT and notices in source |
| dav1d / libpng / zlib / bzip2 | devkitPro portlibs package sources | BSD-2-Clause / libpng / Zlib / bzip2 license |

The native client protocol is ported from the user-provided local UURemote-web reference. No original UU binaries, APK, extracted UI textures, or signing constant are committed to this repository. Local builds still embed locally supplied signing material; this is not a claim of binary redistribution authorization. UU names, services, and protocol belong to their respective owners. This is an unofficial client.

The optional receive estimator is a pinned compatibility reference, not an identified exact UU TV WebRTC revision. The algorithms remain upstream; the platform thread-ID bridge and build integration are local adaptations. See `cmake/webrtc-rbe/upstream.lock.json`, `scripts/fetch_rbe_sources.py`, and `docs/tv-adaptation-r3-candidate-2026-11-08.md`. Matching notices are under `licenses/webrtc-*` and `licenses/abseil-Apache-2.0.txt`, and are packed into RBE-enabled local NROs. Disabling the RBE option does not introduce the estimator dependency.


The optional batch-receive scratch adapter in `source/switch_batch_rx.c` is adapted from libnx v4.12.0 `nx/source/runtime/devices/socket.c` (SHA256 `090e8e40f843e2cfd6f9614229a946774ddb713a0b79456ffeb584ab60cbf1ae`). It retains libnx's ISC notice in the source and in `licenses/libnx-ISC.txt`, also packed into the local NRO. The installed SDK and its other socket functions are not replaced.

## v52 additions
- deko3d/dekotools/uam: devkitPro upstream; use their licenses when distributing linked builds. See docs/archive/v52/build-v52.md for historical pinned source references.
- scripts/dependency-overrides contains modified libdatachannel/libjuice source under MPL-2.0, retaining upstream notices and original pinned archive references.
- shaders/ and the geometric UI fallback are project adaptation code. Reference APKs, extracted UU TV textures and branding, credentials, and Nintendo firmware are not included in this source import.

## Current source publication (0.3.9)

Attributions and research-only references: [docs/credits.md](docs/credits.md). Distribution boundaries: [docs/distribution.md](docs/distribution.md). Verified dependency overlays retain individual upstream notices and are recorded in scripts/dependency-overrides/manifest.json. Fonts are unmodified Noto Sans SC under OFL-1.1. No official UU UI textures or fonts are included in this source snapshot.
