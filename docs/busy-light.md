# Busy Light

This document records where the busy-light feature (office-door presence display) currently stands: the data contract, the two mock/bridge server implementations, and what's still not built.

## Status

Contract-first, not backend-first: the firmware, the mock server, and the JSON contract between them are built and working. No real backend (Microsoft Graph, Home Assistant, Power Automate, ...) is wired up end-to-end yet — `server/presence_bridge.py` is a Home Assistant sketch, not a deployed bridge.

The firmware polls two endpoints, split because the data behind them changes at very different rates (see `firmware/main/common/presence_api.h`):

| Endpoint | Poll interval | Why |
| --- | --- | --- |
| `GET /calendar/today` | 20 min | Calendar events are near-static within a day |
| `GET /live` | 90s | In-call/webcam/presenting state can change second-to-second, but the e-paper panel takes 15-25s to refresh and locks out input during that, so sub-minute polling wouldn't be visibly faster |

Both are merged into a single `PresenceStatus` (`firmware/main/common/presence_types.h`) before reaching the renderer, so `firmware/main/ui/renderers/rawdraw/busy_light_renderer.cc` doesn't need to know about the split. On a fetch failure or a field missing from a response, the previous value for that field is kept (last-known-good) — a bad poll never blanks the display.

## Contract: `GET /calendar/today`

```json
{
  "generated_at": "2026-09-16T11:46:33+00:00",
  "time_zone": "W. Europe Standard Time",
  "days": [
    {
      "date": "2026-09-16",
      "events": [
        {
          "start": "09:30",
          "end": "10:00",
          "title": "Daily Zeiss Angebot",
          "tier": "leadership",
          "participants": "[\"More than 10 participants\"]",
          "participant_count": "14"
        }
      ]
    },
    { "date": "2026-09-17", "events": [ ] }
  ]
}
```

Field reference:

| Field | Meaning |
| --- | --- |
| `generated_at` | Full ISO-8601 timestamp with UTC offset — a staleness marker, not used for event timing |
| `time_zone` | Informational only; the firmware doesn't parse it — the device's own RTC is already set to `Europe/Berlin` (see the clock/NTP setup), and `start`/`end` are plain local `HH:MM` |
| `days` | List of `{date, events}`. The firmware only consumes the entry whose `date` matches its own local date (`YYYY-MM-DD`), falling back to `days[0]` if nothing matches (e.g. RTC not synced yet, or a TZ mismatch with the server) rather than rendering an empty page. Extra days beyond today are carried through for a possible future agenda view but aren't read yet. |
| `events[].start` / `events[].end` | Local `"HH:MM"` strings, no timezone math needed on-device |
| `events[].title` | Event title, shown as-is (or redacted to "Busy" by a bridge, before it ever leaves the source network) |
| `events[].tier` | One of `solo` / `internal` / `leadership` / `customer`. Drives the door-fill style on the detail-face day grid (outline / outline / yellow-rule / solid-red) and the "come in" / "knock" / "quiet" line on the default face. `solo` (no other attendees — e.g. "Lunch") currently renders identically to `internal`; it exists as a distinct tier for parity with the real feed, not because the renderer treats it specially yet. Unknown/missing tier defaults to `internal`, the least alarming reading. |
| `events[].participants` | JSON-encoded **string** (not a nested array) — e.g. `"[\"Alice\",\"Bob\"]"` or `"[\"More than 10 participants\"]"`. Not yet read by the firmware. |
| `events[].participant_count` | Decimal **string**, not a number — e.g. `"14"`. Not yet read by the firmware. |

The `participants`/`participant_count` shape (string-encoded rather than a real array/number) mirrors exactly what the real Microsoft Graph-backed feed sends. The mock server and firmware parser intentionally don't "fix" it into a cleaner shape, so the mock never drifts from what a real bridge would actually have to produce.

Overlapping events (a real calendar can easily have two or three concurrent invites) are handled entirely in the renderer via a greedy 2-column layout with overflow summarized as "+N more" — the contract itself makes no attempt to pre-resolve overlaps or assign priority.

## Contract: `GET /live`

```json
{
  "updated_at": "2026-09-16T10:32:05+02:00",
  "isPresenting": false,
  "isInCall": false,
  "isWebcamActive": false
}
```

Straightforward booleans, no tiering. `isPresenting` (screen-share/do-not-disturb) takes priority in the renderer: it always colors the header band red regardless of calendar tier.

## Servers

Two independent implementations of the same contract, for two different purposes:

| Script | Purpose |
| --- | --- |
| `server/mock_presence_server.py` | Pure mock — returns hardcoded data on every request, edited by hand to try scenarios. No real backend queried. This is what's actually been used for firmware dev so far. |
| `server/presence_bridge.py` | Sketch of a real bridge, backed by Home Assistant: two binary sensors for call/webcam state, plus HA's calendar API for `/calendar/today`. `tier` is derived from keyword lists matched against the event title (`HA_CUSTOMER_KEYWORDS`, `HA_LEADERSHIP_KEYWORDS`, `HA_SOLO_KEYWORDS`) since HA doesn't expose anything closer to "how important is this meeting" or attendee counts. Never deployed against a real HA instance as part of this project — treat it as a starting point, not a finished bridge. |

Run the mock server:

```bash
python3 server/mock_presence_server.py --port 8080
```

Point the firmware at it via `presence_api_set_endpoint()`, or by editing `kPresenceBridgeEndpoint` in `firmware/main/application.cc` (must be a plain LAN IP — no mDNS component is wired into this firmware).

## Not yet built

- No real backend integration (Graph, Power Automate, or a deployed HA instance) — `presence_bridge.py` is unverified against live HA data.
- The poll-vs-push decision for a real backend is still open; battery impact is the deciding factor, not settled yet.
- `participants`/`participant_count` are carried in the contract but not surfaced anywhere in the UI.
- The device-side HTTP response buffer is fixed at 8KB (`firmware/main/common/presence_api.cc`) — sized for a busy single day with long titles, not for a third `days` entry or an unusually large participant list.
