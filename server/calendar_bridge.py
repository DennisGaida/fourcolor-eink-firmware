#!/usr/bin/env python3
"""
calendar_bridge.py — HTTP bridge for the busy-light day-view firmware page,
backed by a real calendar-source webhook.

Serves the same two GET endpoints as server/mock_presence_server.py (see
that file for the full contract docs):

    GET /calendar/today   — today + tomorrow, near-static within a day
    GET /live              — presence state, can change second-to-second

Deliberately source-agnostic: this bridge doesn't know or care what tool
sits behind CALENDAR_SOURCE_URL (n8n, a Power Automate flow, a Graph proxy,
whatever) — it only knows that GET'ing that URL with an
`x-calendar-secret` header returns JSON already shaped exactly like the
`/calendar/today` contract (`generated_at`/`time_zone`/`days[].events[]`,
with `tier`/`participants`/`participant_count` as in the mock server's
docs). The response is re-validated and rebuilt field-by-field here rather than
blindly proxied, even though it's expected to already match the contract —
a malformed or unexpected upstream response (wrong types, an unknown
`tier`, extra fields) can't crash this handler or reach the firmware
unsanitized.

/live is backed by Home Assistant: two binary sensors (in-call, webcam
active) plus a text sensor whose state names the current Teams presence
(read to detect "Presenting"). Entity IDs are configurable via env vars
since they're specific to whatever publishes them into HA (a Teams status
add-in, an MQTT bridge, etc.) — see server/presence_bridge.py's docstring
for the same sensors used as an all-HA alternative to this webhook+HA
split. If HA_URL/HA_TOKEN aren't set, /live falls back to the "nothing
going on" defaults rather than erroring.

Secrets are provided ONLY via environment variables, set at process
start — never hardcode them, never commit them.

On any internal error, /calendar/today still returns HTTP 200 with an
empty `days` list, and /live still returns HTTP 200 with the "nothing
going on" defaults, so the firmware's HttpGet() succeeds and
ParseCalendarJson()/ParseLiveJson() fall back to keeping last-known-good
data (see firmware/main/common/presence_api.cc) rather than erroring loudly.

Usage: copy server/.env.example to server/.env, fill in real values, then
just run the script — it loads server/.env itself (no `source`/pip install
needed). A real environment variable of the same name always wins over the
file, so `FOO=bar python3 calendar_bridge.py` still overrides server/.env.

    python3 calendar_bridge.py [--port 8080] [--env-file path/to/.env]
"""

import argparse
import datetime
import json
import logging
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

from env_file import load_default_env_file

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("calendar_bridge")

_default_env_file = load_default_env_file(__file__)

CALENDAR_SOURCE_URL = os.environ.get("CALENDAR_SOURCE_URL", "")
CALENDAR_SOURCE_SECRET = os.environ.get("CALENDAR_SOURCE_SECRET", "")
CALENDAR_FETCH_TIMEOUT_SECONDS = 10

HA_URL = os.environ.get("HA_URL", "").rstrip("/")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
HA_CALL_SENSOR = os.environ.get("HA_CALL_SENSOR", "binary_sensor.teams_in_call")
HA_WEBCAM_SENSOR = os.environ.get("HA_WEBCAM_SENSOR", "binary_sensor.pw0q6czd_webcamactive")
HA_TEAMS_STATUS_SENSOR = os.environ.get("HA_TEAMS_STATUS_SENSOR", "sensor.teams_status")
# Comma-separated, case-insensitive: HA_TEAMS_STATUS_SENSOR's state is
# compared against this list to derive isPresenting — there's no dedicated
# "presenting" binary sensor, just this text state.
HA_PRESENTING_STATES = {
    s.strip().lower()
    for s in os.environ.get("HA_PRESENTING_STATES", "Presenting").split(",")
    if s.strip()
}
HA_FETCH_TIMEOUT_SECONDS = 5

VALID_TIERS = {"solo", "internal", "leadership", "customer"}

# Some reverse proxies (Cloudflare included) block urllib's default
# "Python-urllib/3.x" User-Agent outright (HTTP 403 with no useful body) —
# a plain browser-looking one avoids that without meaning anything else.
_HTTP_USER_AGENT = {"User-Agent": "Mozilla/5.0 (compatible; fourcolor-eink-firmware/calendar_bridge)"}


def now_iso() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def empty_calendar_payload() -> dict:
    return {"generated_at": now_iso(), "time_zone": "", "days": []}


def _coerce_str(value, default=""):
    if isinstance(value, str):
        return value
    return default


def _sanitize_event(raw: dict) -> dict:
    """Keeps only the fields the firmware contract defines, defaulting/
    coercing anything missing or oddly-typed rather than passing it through
    as-is — mirrors how ParseCalendarJson in presence_api.cc treats a
    malformed field (ignored, not a fetch failure)."""
    tier = _coerce_str(raw.get("tier"), "internal").lower()
    if tier not in VALID_TIERS:
        tier = "internal"
    return {
        "start": _coerce_str(raw.get("start")),
        "end": _coerce_str(raw.get("end")),
        "title": _coerce_str(raw.get("title")),
        "tier": tier,
        # Carried through as-is (JSON-encoded strings, not real array/number
        # types) — that's the contract's actual shape, mirroring what the
        # real feed sends. Not yet read by the firmware.
        "participants": _coerce_str(raw.get("participants"), "[]"),
        "participant_count": _coerce_str(raw.get("participant_count"), "0"),
    }


def _sanitize_day(raw: dict) -> dict:
    events_in = raw.get("events")
    events_out = []
    if isinstance(events_in, list):
        for item in events_in:
            if isinstance(item, dict):
                events_out.append(_sanitize_event(item))
    return {"date": _coerce_str(raw.get("date")), "events": events_out}


def fetch_calendar_today() -> dict:
    if not CALENDAR_SOURCE_URL:
        logger.error("CALENDAR_SOURCE_URL not set — returning empty calendar")
        return empty_calendar_payload()

    req = Request(CALENDAR_SOURCE_URL, headers={**_HTTP_USER_AGENT, "x-calendar-secret": CALENDAR_SOURCE_SECRET})
    try:
        with urlopen(req, timeout=CALENDAR_FETCH_TIMEOUT_SECONDS) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except (URLError, HTTPError, ValueError) as e:
        logger.warning("Failed to fetch calendar from source: %s", e)
        return empty_calendar_payload()

    if not isinstance(payload, dict):
        logger.warning("Calendar source returned a non-object payload — returning empty calendar")
        return empty_calendar_payload()

    days_in = payload.get("days")
    days_out = []
    if isinstance(days_in, list):
        for day in days_in:
            if isinstance(day, dict):
                days_out.append(_sanitize_day(day))

    return {
        "generated_at": _coerce_str(payload.get("generated_at"), now_iso()),
        "time_zone": _coerce_str(payload.get("time_zone")),
        "days": days_out,
    }


def _ha_get_state(entity_id: str):
    if not entity_id or not HA_URL or not HA_TOKEN:
        return None
    req = Request(
        f"{HA_URL}/api/states/{entity_id}",
        headers={**_HTTP_USER_AGENT, "Authorization": f"Bearer {HA_TOKEN}"},
    )
    try:
        with urlopen(req, timeout=HA_FETCH_TIMEOUT_SECONDS) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        return payload.get("state")
    except (URLError, HTTPError, ValueError) as e:
        logger.warning("Failed to read %s from Home Assistant: %s", entity_id, e)
        return None


def _ha_get_binary(entity_id: str) -> bool:
    return _ha_get_state(entity_id) == "on"


def build_live() -> dict:
    teams_status = (_ha_get_state(HA_TEAMS_STATUS_SENSOR) or "").strip().lower()
    return {
        "updated_at": now_iso(),
        "isPresenting": teams_status in HA_PRESENTING_STATES,
        "isInCall": _ha_get_binary(HA_CALL_SENSOR),
        "isWebcamActive": _ha_get_binary(HA_WEBCAM_SENSOR),
    }


ROUTES = {
    "/calendar/today": fetch_calendar_today,
    "/live": build_live,
}


def make_handler():
    class CalendarBridgeHandler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            logger.info("%s - %s", self.address_string(), fmt % args)

        def do_GET(self):
            builder = ROUTES.get(self.path.rstrip("/"))
            if builder is None:
                self.send_response(404)
                self.end_headers()
                return

            try:
                payload = builder()
            except Exception:
                logger.exception("Unexpected error building %s payload", self.path)
                payload = {}

            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return CalendarBridgeHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--env-file", default=_default_env_file,
                         help="Path to a KEY=VALUE .env file to load (already loaded by the time this runs)")
    args = parser.parse_args()

    if not CALENDAR_SOURCE_URL:
        logger.warning("CALENDAR_SOURCE_URL not set — /calendar/today will return empty")
    if not CALENDAR_SOURCE_SECRET:
        logger.warning("CALENDAR_SOURCE_SECRET not set — calendar source requests will be sent without a secret")
    if not HA_URL or not HA_TOKEN:
        logger.warning("HA_URL/HA_TOKEN not set — /live will always report the 'nothing going on' defaults")

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler())
    logger.info("Calendar bridge listening on :%d (calendar_source=%s, ha_url=%s)",
                args.port, CALENDAR_SOURCE_URL or "<unset>", HA_URL or "<unset>")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
