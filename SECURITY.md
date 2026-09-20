# Security Notes

## Secrets

- Do not commit `server/.env` or any credential-bearing local config file — only `server/.env.example` is tracked.
- The presence/calendar bridge (`server/calendar_bridge.py` / `presence_bridge.py`) handles `HA_TOKEN` and `CALENDAR_SOURCE_SECRET`; rotate these immediately if ever pasted into an issue, PR, or commit. Both also support the `<VAR>_FILE` convention for Docker/Kubernetes secrets instead of putting raw values in `.env`.

## Device-Exposed Network Surfaces

- **AP photo transfer**: the device hosts an open Wi-Fi AP (`InkScreen-AP`, default password `12345678`) with an unauthenticated HTTP upload page while transfer mode is active. Anyone in range who knows/guesses the password can push images to the device during that window.
- **LAN photo push**: the `/upload` HTTP API exposed once "LAN Service" is enabled (see [`docs/LAN_PHOTO_PUSH_API.md`](docs/LAN_PHOTO_PUSH_API.md)) has no authentication — anything on the same LAN can push images to the device. Only enable it on a trusted network.

## Reporting

If you find a vulnerability, avoid posting raw secrets publicly. Rotate the affected secret first, then open an issue with a sanitized report.
