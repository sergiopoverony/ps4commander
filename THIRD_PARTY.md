# Third-party code

## libsmb2
SMB2/3 client library, `third_party/libsmb2/` (unmodified sources of the files used, plus a hand-written `config.h`).
Upstream: https://github.com/sahlberg/libsmb2 (commit in `third_party/libsmb2/VERSION.txt`).
License: GNU LGPL v2.1 (`third_party/libsmb2/LICENCE-LGPL-2.1.txt`). The library is linked statically; the complete
source of the library is included in this repository, so it can be modified and relinked.

## libfuse + fusefs patch
FUSE userspace library (libfuse 2.9, LGPL-2.1) with the PlayStation 4 port from Itemzflow (LightningMods, GPL-3.0), `third_party/libfuse/` (see its NOTICE).
The kernel patch that enables fusefs and its per-firmware offset table in `src/netfs.cpp` come from PS4Mount-PS4 (gasb07, GPL-3.0), converted from Itemzflow's.
Used to show an SMB share as a folder on a USB drive, so the system installer can read a `.pkg` from it without copying. Only firmwares of that table are supported (13.52 and 6.72 were tested by the PS4Mount author).

## DejaVu Sans Mono
Bitmap/outline font used only by the gamepad PKG app (`pkg/assets/fonts/DejaVuSansMono.ttf`) to draw its own text on-screen; the web interface renders text with the browser's own fonts and does not use this file.
License: public domain / Bitstream Vera Fonts License (see `pkg/assets/fonts/LICENSE-DejaVu.txt`).

## minimp3
MP3 decoder used only by the gamepad PKG app (`pkg/src/minimp3.h`) to play music. Public domain (CC0), https://github.com/lieff/minimp3

## stb_image / stb_vorbis
Image (PNG/JPG/BMP/GIF) and OGG Vorbis decoders from the OpenOrbis toolchain (`stb/`), used only by the gamepad PKG app. Public domain / MIT (Sean Barrett), https://github.com/nothings/stb
