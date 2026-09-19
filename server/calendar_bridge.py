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

/live has no real source wired up yet (see server/presence_bridge.py's
Home Assistant sketch for the shape that will eventually take) — it always
returns the "nothing going on" defaults for now.

The secret is provided ONLY via the CALENDAR_SOURCE_SECRET environment
variable, set at process start — never hardcode it, never commit it.

On any internal error, /calendar/today still returns HTTP 200 with an
empty `days` list so the firmware's HttpGet() succeeds and
ParseCalendarJson() falls back to keeping last-known-good data (see
firmware/main/common/presence_api.cc) rather than erroring loudly.

Usage:
    CALENDAR_SOURCE_URL=https://example.invalid/webhook/calendar \\
    CALENDAR_SOURCE_SECRET=your_secret \\
    python3 calendar_bridge.py [--port 8080]
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

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("calendar_bridge")

CALENDAR_SOURCE_URL = os.environ.get("CALENDAR_SOURCE_URL", "")
CALENDAR_SOURCE_SECRET = os.environ.get("CALENDAR_SOURCE_SECRET", "")
CALENDAR_FETCH_TIMEOUT_SECONDS = 10

VALID_TIERS = {"solo", "internal", "leadership", "customer"}


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

    req = Request(CALENDAR_SOURCE_URL, headers={"x-calendar-secret": CALENDAR_SOURCE_SECRET})
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


def build_live() -> dict:
    return {
        "updated_at": now_iso(),
        "isPresenting": False,
        "isInCall": False,
        "isWebcamActive": False,
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
    args = parser.parse_args()

    if not CALENDAR_SOURCE_URL:
        logger.warning("CALENDAR_SOURCE_URL not set — /calendar/today will return empty")
    if not CALENDAR_SOURCE_SECRET:
        logger.warning("CALENDAR_SOURCE_SECRET not set — calendar source requests will be sent without a secret")

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler())
    logger.info("Calendar bridge listening on :%d (source=%s)",
                args.port, CALENDAR_SOURCE_URL or "<unset>")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
