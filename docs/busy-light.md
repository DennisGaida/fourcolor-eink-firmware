# Busy Light

This document records where the busy-light feature (office-door presence display) currently stands: the data contract, the two mock/bridge server implementations, and what's still not built.

## Status

Contract-first, not backend-first — but the real backend is live now. `server/calendar_bridge.py` is a real bridge (`/calendar/today` against a calendar webhook, `/live` against Home Assistant sensors), verified end-to-end against the actual webhook/HA instance and confirmed on-device (default face correctly showing FREE/CAM OFF against an empty calendar and idle sensors). `kPresenceBridgeEndpoint` in `application.cc` doesn't distinguish mock from real — it's just whatever server happens to be running at that LAN IP/port, so switching between them is an operational choice (which server process is up), not a firmware change. `server/mock_presence_server.py` is kept around for offline dev/demo.

The firmware polls two endpoints, split because the data behind them changes at very different rates (see `firmware/main/common/presence_api.h`):

| Endpoint | Poll interval | Why |
| --- | --- | --- |
| `GET /calendar/today` | 20 min | Calendar events are near-static within a day |
| `GET /live` | 90s | In-call/webcam/presenting state can change second-to-second, but the e-paper panel takes 15-25s to refresh and locks out input during that, so sub-minute polling wouldn't be visibly faster |

Both are merged into a single `PresenceStatus` (`firmware/main/common/presence_types.h`) before reaching the renderer, so `firmware/main/ui/renderers/rawdraw/busy_light_renderer.cc` doesn't need to know about the split. On a fetch failure or a field missing from a response, the previous value for that field is kept (last-known-good) — a bad poll never blanks the display.

### Redraw behavior: polling is cheap, e-ink refresh is not

Every poll used to trigger an unconditional full-screen redraw, whether or not anything actually changed — and since the panel forces a hardware full refresh every 10 partial refreshes (`EpdRefreshScheduler`, `firmware/main/ui/epd_refresh.h`), that meant a 15-25s full refresh every 10 × 90s = 15 minutes, forever, even with an unchanging calendar and idle webcam. `BusyLightRenderer::Update()` (`busy_light_renderer.cc`) now compares the incoming status (plus time-derived state — which event is active/next, how many events have already ended, whether the 5pm tomorrow-line cutoff has passed) against what's actually on screen, and skips the redraw entirely when nothing visibly changed.

When something did change, the detail face (`View::kDetail`) can usually settle for a small dirty-rect refresh of just the header strip (swatch/word/camera icon, `{0, kHeaderTop, width, kHeaderHeight}`) instead of the whole panel, since the day grid below it doesn't read any AV state. The default/hallway face has no equivalent split — AV state feeds the band, headline word, human line, and presenting banner across most of the page — so any change there still redraws full-screen.

Net effect: polling frequency and screen-refresh cost are now decoupled. A poll that finds nothing new costs a small HTTP request and nothing else; only a poll that finds something new pays for a redraw, and only sometimes pays for a full one. One deliberate trade-off this introduces: the "now" position/past-event styling only updates on a poll boundary, same as before, not continuously.

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
| `days` | List of `{date, events}`. The firmware consumes two entries out of this list: the one whose `date` matches its own local date (falling back to `days[0]` if nothing matches — e.g. RTC not synced yet, or a TZ mismatch with the server — rather than rendering an empty page), and the one matching local date + 1 day ("tomorrow"), used only for the default face's end-of-workday footer line (see below). Unlike today, tomorrow has **no** `days[0]`-style fallback: if no entry matches tomorrow's date, the firmware treats that as "unknown" and keeps its previous last-known-good tomorrow summary rather than guessing "no meetings". Any further days beyond that are carried through but not read. |
| `events[].start` / `events[].end` | Local `"HH:MM"` strings, no timezone math needed on-device |
| `events[].title` | Event title, shown as-is (or redacted to "Busy" by a bridge, before it ever leaves the source network) |
| `events[].tier` | One of `solo` / `internal` / `leadership` / `customer`. Drives the door-fill style on the detail-face day grid (outline / outline / yellow-rule / solid-red) and the "come in" / "knock" / "quiet" line on the default face. `solo` (no other attendees — e.g. "Lunch") currently renders identically to `internal`; it exists as a distinct tier for parity with the real feed, not because the renderer treats it specially yet. Unknown/missing tier defaults to `internal`, the least alarming reading. |
| `events[].participants` | JSON-encoded **string** (not a nested array) — e.g. `"[\"Alice\",\"Bob\"]"` or `"[\"More than 10 participants\"]"`. Not yet read by the firmware. |
| `events[].participant_count` | Decimal **string**, not a number — e.g. `"14"`. Not yet read by the firmware. |

The `participants`/`participant_count` shape (string-encoded rather than a real array/number) mirrors exactly what the real Microsoft Graph-backed feed sends. The mock server and firmware parser intentionally don't "fix" it into a cleaner shape, so the mock never drifts from what a real bridge would actually have to produce.

Overlapping events (a real calendar can easily have two or three concurrent invites) are handled entirely in the renderer via a greedy 2-column layout with overflow summarized as "+N more" — the contract itself makes no attempt to pre-resolve overlaps or assign priority.

### Tomorrow footer line (default face)

Starting at 17:00 local time, the default face shows one extra line above the "press for today's plan" footer hint: `"First meeting tomorrow at HH:MM"` if tomorrow's day has at least one event, or `"No meetings tomorrow"` if it has none. Before 17:00, or if a fetch hasn't yet resolved a "tomorrow" day (see the `days` field reference above), the line is omitted entirely rather than showing a stale or guessed value. The "interesting" segment — the `HH:MM` time, or "No meetings" — sits in a bold yellow highlight chip sized to the actual text width (so a longer time like `10:30` isn't clipped), matching how importance is carried elsewhere on this page (tier fills, the presenting banner); the surrounding words stay plain weight so the chip is the only thing that draws the eye.

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

Three independent implementations of the same contract, for three different purposes:

| Script | Purpose |
| --- | --- |
| `server/mock_presence_server.py` | Pure mock — returns hardcoded data on every request, edited by hand to try scenarios. No real backend queried. This is what's actually been used for firmware dev so far, and what the firmware still points at (see below). |
| `server/calendar_bridge.py` | Real bridge for both endpoints. `/calendar/today` is backed by a calendar webhook — URL and an `x-calendar-secret` header value are read from the `CALENDAR_SOURCE_URL`/`CALENDAR_SOURCE_SECRET` environment variables (never hardcoded, never logged, never committed). Deliberately source-agnostic: it only knows the webhook returns JSON already shaped like this contract, not what tool sits behind it. The upstream response is validated/rebuilt field-by-field rather than blindly proxied. `/live` is backed by Home Assistant: `isInCall`/`isWebcamActive` come from binary sensors (`HA_CALL_SENSOR`/`HA_WEBCAM_SENSOR`), `isPresenting` from comparing a text sensor's state (`HA_TEAMS_STATUS_SENSOR`) against `HA_PRESENTING_STATES`. All four entity IDs are configurable env vars since they depend on whatever publishes them into HA. Falls back to the "nothing going on" defaults if `HA_URL`/`HA_TOKEN` aren't set. |
| `server/presence_bridge.py` | Alternate all-HA sketch: same two binary sensors for call/webcam state as `calendar_bridge.py`, but also sources `/calendar/today` from HA's calendar API instead of a webhook. `tier` is derived from keyword lists matched against the event title (`HA_CUSTOMER_KEYWORDS`, `HA_LEADERSHIP_KEYWORDS`, `HA_SOLO_KEYWORDS`) since HA doesn't expose anything closer to "how important is this meeting" or attendee counts. Never deployed against a real HA instance as part of this project — treat it as a starting point, not a finished bridge. Superseded by `calendar_bridge.py` for setups that already have a calendar webhook. |

The firmware (`kPresenceBridgeEndpoint` in `firmware/main/application.cc`) is currently pointed at `mock_presence_server.py`, not `calendar_bridge.py` — the switch to real calendar data is a deliberate later step, made once the tomorrow-footer line (see above) has been through a mocked dev/test pass.

Run the mock server:

```bash
python3 server/mock_presence_server.py --port 8080
```

Point the firmware at it via `presence_api_set_endpoint()`, or by editing `kPresenceBridgeEndpoint` in `firmware/main/application.cc` (must be a plain LAN IP — no mDNS component is wired into this firmware).

## Not yet built

- `participants`/`participant_count` are carried in the contract but not surfaced anywhere in the UI.
- The device-side HTTP response buffer is fixed at 8KB (`firmware/main/common/presence_api.cc`) — sized for a busy single day with long titles, not for a third `days` entry or an unusually large participant list.
- UP/DOWN on the detail face scroll the zoomed day-grid window an hour at a time, clamped to the day's 8:00-18:00 bounds; a press that's already at the clamp signals a no-op with a rapid double-blink on the onboard LED (`Board::FlashErrorLed()`) instead of the usual single activity-pulse blink. Resets to auto-centered-on-now whenever the view is toggled. On the default face (or everywhere, when `CONFIG_BUSY_LIGHT_DEBUG_CYCLE` is off), UP/DOWN remain a no-op.
