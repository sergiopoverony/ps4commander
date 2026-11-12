# PS4 Commander

A two-panel (Norton/Total Commander style) file manager for the PS4, running as a payload with a web interface. Open `http://<PS4 IP>:8484/` in a browser on any device in your network.

Russian version: [docs/README_ru.md](docs/README_ru.md)

## Features
- Two panels, keyboard control (F-keys, Tab, Insert/Space, `+`/`-` select by mask, `*` invert) and touch buttons.
- Copy, move, delete, rename, new folder, new file, permissions (chmod), folder size, with progress and cancel.
- Text viewer / hex viewer, image/audio/video preview, built-in text editor (up to 1 MB).
- Download a folder (or several items) as a ZIP built on the fly, no temporary files (Ctrl+D).
- Network folders over SMB (SMB2/3, Windows/Samba/NAS shares) shown as `/smb/<name>`: browse, view, download, copy/move to and from the console, ZIP, search. Press `N` to add a share; passwords are stored on the console in `/data/ps4commander/smb.json`.
- Upload (button or drag-and-drop) and download.
- Recursive file search with masks.
- Install `.pkg` files via the system installer (BGFT), with a queue and progress (Enter on a `.pkg`).
- Safety: protected system paths, an explicit "unsafe" mode for system areas, CSRF header, DNS-rebinding protection.
- Interface languages: English, Russian.

## Status
Early development (v0.1). A gamepad-driven PKG application is planned.

## Development
Host tests: `python3 tests/test_api.py build/host_test`, `python3 tests/test_ui.py build/host_test` (see `Makefile` and `tools/embed.py`).

## License
GPLv3, see [LICENSE](LICENSE).
