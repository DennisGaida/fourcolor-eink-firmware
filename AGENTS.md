# AGENTS.md

Instructions for coding agents working in this repo. Full context lives in
[`README.md`](README.md) and [`CONTRIBUTING.md`](CONTRIBUTING.md) — this file
only exists to put the build/flash facts an agent needs *before* touching
anything front and center, so they don't have to be rediscovered every
session.

## Firmware build (Windows / PowerShell) — the fast path

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue   # MANDATORY, see gotcha below
. 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'
cd firmware
idf.py build
```

Flash (adjust COM port):

```powershell
idf.py -p COM9 flash
```

## Verifying changes after flashing — use serial remote control, not eyeballing the board

After flashing, don't just stare at the physical screen or fumble with the
buttons by hand — the firmware exposes a serial command interface over the
same COM port used for flashing, so you can drive the device programmatically:

```powershell
python server\serial_tool.py COM9 screenshot check.png   # grab a PNG of the current framebuffer
python server\serial_tool.py COM9 button boot_click      # inject a synthetic button press
python server\serial_tool.py COM9 page weather           # jump straight to a page (busylight/weather/gallery/settings)
```

Then `view` the saved PNG to confirm the result of your change instead of
guessing. Requires `pip install pyserial pillow`. Full protocol/wire-format
details and gotchas (notably: the device can take ~20-30s to finish booting
before commands work, and a page/button command that triggers an e-ink
refresh can take up to ~120s to settle before a follow-up screenshot shows
the final frame) are in [`docs/serial-control.md`](docs/serial-control.md).

Fresh clone/worktree one-time setup — `firmware/sdkconfig` is gitignored, so a
new checkout defaults to the wrong chip target:

```powershell
idf.py set-target esp32s3
```

**Gotchas that waste a whole session if missed:**
- Must run in the ESP-IDF PowerShell environment, **not Git Bash** — ESP-IDF
  tooling breaks under `MSYSTEM=MINGW64`.
- Clearing `Env:MSYSTEM` first is mandatory — otherwise `idf.py build`
  silently no-ops with exit code 0 and never compiles anything.
- Requires ESP-IDF v6.0 installed via the
  [idf-im-ui installer](https://github.com/espressif/idf-im-ui)
  (default path `C:\Espressif\`).
- Do **not** use `firmware/build.sh` or `firmware/build_windows.ps1` — both
  are stale/legacy (hardcoded paths, older IDF version, references to
  directories that don't exist in this repo). See
  `tmp/legacy-file-audit.md`. Use `idf.py build` directly.

## Firmware build (Linux/macOS)

```bash
cd firmware
source ~/Documents/esp/v6.0/esp-idf/export.sh
idf.py build
```

## Hardware target

- **Single board, single chip**: ESP32-S3 only. The other
  `firmware/sdkconfig.defaults.esp32*` variants (c3/c5/c6/p4) are leftovers
  from the upstream multi-board template this project was forked from —
  don't treat them as supported targets.
- Screen: ZecTrix 4.2" four-color BWRY e-ink (`ZECTRIX_EPD_PANEL_4COLOR_SSD2683`)
  by default; 1bpp black/white fallback also supported
  (`ZECTRIX_EPD_PANEL_1BPP`, toggle via `idf.py menuconfig`).

## Repo shape (see README for full detail)

- `firmware/` — ESP-IDF project (the actual product).
- `firmware/experiments/` — intentionally shelved, unfinished modules
  (chat/ebook/news/calendar), excluded from the build on purpose. Not dead
  code to clean up.
- `server/` — optional Python presence/calendar bridge + a few local
  dev/debug scripts, not required for normal device operation.
- `docs/` — design docs (LAN photo push API, weather, busy-light, serial
  remote control).

## Commit messages

Conventional Commits are required — see `CONTRIBUTING.md`. This isn't just
style: the `server/` bridge's published Docker image version is computed
automatically from commit types touching `server/`.
