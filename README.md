# FourColor Ink

This is a personal AI assistant project for ESP32-S3 e-ink devices. The current mainline consists of three parts: ESP32 firmware, a Python backend service, and photo/todo/device management pages.

The focus of this project is not a generic npm package, but a system that actually runs on an e-ink device: voice conversation, TTS playback, todo sync, weather/news/calendar/e-book/album pages, AP photo transfer, OTA firmware management, and a RawDraw UI adapted for four-color screens.

## 2BP Four-Color Image Pipeline

![FourColor Ink 2BP BWRY architecture](README-2bp-architecture.png)

Album images can enter the server either from a PC/NAS management console or from the device's AP page, and are converted to `2BP BWRY` (black, white, red, yellow) before being pushed over Wi-Fi to the ESP32-S3 four-color e-ink screen. This repo's 2BP four-color pipeline is maintained independently from NOTE4's 4BP black/white grayscale album: panel colors, pixel formats, and refresh drivers all differ.

## Current Status

- The backend has switched to the Python service under `server/`; the old root-level Node `scripts/` has been removed.
- The firmware's main UI is rendered with RawDraw, designed by default for four-color screens, while still keeping 1bpp black/white compatibility.
- Themes currently keep a single default visual direction: a Nintendo-esque four-color theme, emphasizing the semantic use of red, yellow, black, and white.
- Image transfer supports both 1bpp black/white and 2bpp four-color BWRY formats.
- The root `.gitignore` excludes build artifacts, logs, pid files, databases, local config, and secret files.

## Directory Structure

```text
.
├── firmware/        ESP32-IDF firmware, RawDraw UI, page rendering, screen drivers, AP photo transfer
├── server/          Python backend, WebSocket conversation, TTS, discovery, image push, OTA API
├── frontend/        Management frontend source, using its own package/pnpm workflow
├── docs/            Historical design docs and implementation notes
├── documents/       Project reference material
└── package.json     Only keeps repo-level helper commands, no longer the entry point for the old Node service
```

Note: `firmware/scripts/` and `frontend/scripts/` are still in use, belonging to the firmware tooling and frontend tooling respectively; what was removed is the legacy root-level `scripts/`.

## Presence/Calendar Bridge (Busy Light)

The busy-light page polls a small HTTP bridge for `GET /live` (presence)
and `GET /calendar/today` (calendar) - see `server/calendar_bridge.py` or
`server/presence_bridge.py` for the two implementations (webhook+HA vs.
all-HA) and `server/.env.example` for the full list of configuration
variables. Point the firmware at it via the `PRESENCE_BRIDGE_ENDPOINT`
Kconfig option (`idf.py menuconfig` > Xiaozhi Assistant > Deployment
defaults).

Run it directly:

```bash
cd server
cp .env.example .env   # fill in real values
python3 calendar_bridge.py --port 8080
```

Or as a container (no third-party Python dependencies, so the image is
just a slim Python base + the two bridge scripts):

```bash
cd server
cp .env.example .env   # fill in real values
docker compose up -d --build
```

A prebuilt image is also published to `ghcr.io/<owner>/busylight-bridge`
on every change under `server/` (see
`.github/workflows/build-bridge.yml`); swap `docker-compose.yml`'s
`build:` line for `image: ghcr.io/<owner>/busylight-bridge:<version>` to
use that instead of building locally. There's no version file to bump by
hand: the published version is computed automatically from [Conventional
Commits](#commit-messages) touching `server/` since the last `bridge-v*`
git tag (`feat:` -> minor, `fix:`/`perf:` -> patch, a `!:` marker or
`BREAKING CHANGE:` footer -> major).

Every config variable also supports the standard `<VAR>_FILE` Docker/
Kubernetes secrets convention (e.g. `HA_TOKEN_FILE=/run/secrets/ha_token`)
as an alternative to putting the raw secret in `.env` - see the commented
`secrets:` example in `server/docker-compose.yml`.

## Backend Service

The backend entry point is `server/llmserve.py`, best managed via `server/start.sh`. Default service ports:

| Port | Protocol | Purpose |
| --- | --- | --- |
| `9001` | WebSocket | ESP32 voice, LLM, TTS, sync messages |
| `8766` | UDP | Device discovery |
| `8766` | HTTP | Image push, device image management, OTA API |
| `8090` | HTTP | Standalone management service, optional |

### Install Dependencies

```bash
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### Start the Service

```bash
export DASHSCOPE_API_KEY=your_dashscope_api_key
cd server
./start.sh start
```

Common commands:

```bash
cd server
./start.sh status
./start.sh logs
./start.sh restart
./start.sh stop
```

You can also invoke these from the repo root:

```bash
npm run server:start
npm run server:status
npm run server:logs
```

### Local Mock Device

```bash
cd server
python3 mock_client.py --server ws://127.0.0.1:9001
```

## Photo and Device Management

The image HTTP API is served by `server/push_image.py` on port `8766`. It supports:

- Uploading an image file, converting it, and pushing it to the device.
- Choosing between `1bpp` black/white or `2bpp` four-color BWRY format.
- Querying the device's image list.
- Deleting device images.
- Uploading firmware and serving it for OTA download.

Common endpoints:

```bash
curl http://localhost:8766/api/status
curl http://localhost:8766/api/images
```

Example image upload:

```bash
curl -X POST http://localhost:8766/api/upload_image \
  -F "image=@/path/to/photo.jpg" \
  -F "format=bwry2bpp" \
  -F "title=Photo Title"
```

Once the device enters AP photo-transfer mode, connect your phone to the device's hotspot and visit:

```text
http://192.168.4.1
```

## Firmware

The firmware lives in `firmware/`, based on ESP-IDF. It targets the ZecTrix ESP32-S3 4.2" e-ink screen by default, supporting the four-color BWRY screen while also keeping a 1bpp black/white screen configuration.

### Build (Windows / PowerShell)

Install ESP-IDF v6.0 first via the [IDF installation manager (idf-im-ui)](https://github.com/espressif/idf-im-ui)
if it isn't already set up; it creates the `Microsoft.v6.0.PowerShell_profile.ps1` environment
script referenced below (default install path: `C:\Espressif\tools\`).

This project is built on Windows using the ESP-IDF PowerShell environment (not Git Bash — the
ESP-IDF tooling breaks under `MSYSTEM=MINGW64`):

```powershell
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
. 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'
cd firmware
idf.py build
```

Flash (device attached, e.g. on `COM9`):

```powershell
idf.py -p COM9 flash
```

One-time setup for a fresh git worktree/clone (`firmware/sdkconfig` is gitignored, so a new
checkout defaults to the wrong chip target):

```powershell
idf.py set-target esp32s3
```

Gotcha worth remembering: clearing `Env:MSYSTEM` first is mandatory — without it, `idf.py build`
silently no-ops with exit code 0 and never actually compiles anything.

### Build (Linux / macOS)

```bash
cd firmware
source ~/Documents/esp/v6.0/esp-idf/export.sh
idf.py build
```

Root-level helper command:

```bash
npm run firmware:build
```

### Screen Configuration

The firmware Kconfig has a screen type selection:

```text
ZECTRIX_EPD_PANEL_4COLOR_SSD2683  Four-color BWRY screen
ZECTRIX_EPD_PANEL_1BPP            Black/white 1bpp screen
```

To flash back to the old black/white screen, switch to `1bpp black/white EPD` in `idf.py menuconfig` first, then rebuild and reflash. The RawDraw theme layer will downgrade red/yellow semantic colors to a black/white-readable style.

## UI Overview

The firmware UI currently runs on the RawDraw component system. Key pages include:

- Conversation: displays user speech, recognition status, and AI replies.
- Todos: local display, server sync, complete/delete/edit.
- Settings: volume, brightness, theme, network, sync, OTA, etc.
- Album: thumbnail list, full-image view, AP photo-transfer entry point.
- Weather/weather details, news, almanac, year progress, calendar, e-books, logs.
- Quick-switch overlay: for fast navigation between pages.

The four-color screen theme layer draws components via semantic styles; adding bare `RED/YELLOW/BLACK/WHITE` directly in business pages is discouraged. Prefer RawDraw components and theme tokens when adding new UI.

### Button Controls

The device has three physical buttons: UP, DOWN, and BOOT/CONFIRM.

| Button | Click | Long press | Double-click |
| --- | --- | --- | --- |
| UP | Context-sensitive (menu-up / previous item) | Only acts if already on Settings: exits back to Gallery | Opens/closes the quick-switch menu |
| DOWN | Context-sensitive (menu-down / next item) | Opens Settings (from any page) | Not wired to anything |
| BOOT/CONFIRM | Confirm/select (e.g. picks the highlighted quick-switch item) | Context-sensitive: exits WiFi-config-AP mode if active, else exits AP photo-transfer mode if running, else starts AP photo-transfer mode from Gallery, else voice push-to-talk | Not wired to anything (reserved for debug screenshot capture, but no hardware handler currently triggers it) |

UP + DOWN held together (long press) enters WiFi config mode (starts the device's config AP).

Note UP long-press does **not** open Settings — only DOWN long-press does. UP long-press only ever *exits* Settings back to Gallery, and is a no-op on every other page.

## Environment Variables

Common backend environment variables:

| Variable | Default | Description |
| --- | --- | --- |
| `DASHSCOPE_API_KEY` | none | DashScope (Bailian) API Key, required to start the backend |
| `LISTEN_HOST` | `0.0.0.0` | WebSocket listen address |
| `LISTEN_PORT` | `9001` | WebSocket port |
| `DISCOVERY_PORT` | `8766` | UDP discovery port |
| `PUSH_IMAGE_PORT` | `8766` | Image/OTA HTTP API port |
| `TTS_WS_CHUNK_BYTES` | `8000` | TTS push chunk size |
| `TTS_WS_CHUNK_GAP_SEC` | `0.01` | TTS chunk send interval |

Do not commit `.env`, databases, logs, pid files, build directories, or firmware artifacts.

## Git Commit Scope

Recommended to commit:

- Firmware source such as `firmware/main/`, `firmware/components/`, `firmware/partitions/`.
- `server/*.py`, `server/static/`, `server/requirements.txt`, `server/DEPLOY.md`.
- Frontend source such as `frontend/src/`, `frontend/package.json`, `frontend/pnpm-lock.yaml`.
- Root README, docs, config templates.

Do not commit:

- `firmware/build/`
- `firmware/managed_components/`
- `firmware/sdkconfig`
- `firmware/releases/`
- `server/.env`
- `server/todo.db`
- `server/*.pid`
- `server/*.log`
- `frontend/.env*`
- `frontend/dist/`
- `node_modules/`
