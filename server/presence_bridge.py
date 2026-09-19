#!/usr/bin/env python3
"""
presence_bridge.py — HTTP bridge for the busy-light day-view firmware page.

Serves the same two GET endpoints as server/mock_presence_server.py (see
that file for the full contract docs), backed by real Home Assistant data
instead of hardcoded mock values:

    GET /calendar/today   — today + tomorrow, near-static within a day
    {
      "generated_at": "2026-09-16T11:46:33+00:00",
      "time_zone": "W. Europe Standard Time",
      "days": [
        {"date": "2026-09-16", "events": [
          {"start": "09:00", "end": "09:30", "title": "Standup", "tier": "internal"}
        ]},
        {"date": "2026-09-17", "events": [...]}
      ]
    }

    GET /live              — presence state, can change second-to-second
    {
      "updated_at": "2026-09-16T10:32:05+02:00",
      "isPresenting": false,
      "isInCall": false,
      "isWebcamActive": false
    }

`tier` is one of "solo" / "internal" / "leadership" / "customer" and drives
the door-fill style on the firmware's detail-face day grid ("solo" renders
the same as "internal" — it's carried through for parity with the real
Microsoft Graph-backed feed, not because HA can tell them apart yet).

`start`/`end` are local "HH:MM" strings — no timezone math needed on-device.
This bridge doesn't emit `participants`/`participant_count`: the firmware
doesn't read them yet, and Home Assistant's calendar API doesn't expose
attendee data to fabricate them from (unlike the real Graph-backed feed the
mock server mirrors) — omitting the fields entirely is safe, since cJSON
parsing on the firmware side just skips keys it doesn't ask for.

On any internal error, each endpoint still returns HTTP 200 with an empty
`days`/omitted live fields so the firmware's HttpGet() succeeds and
ParseCalendarJson()/ParseLiveJson() fall back to keeping last-known-good
data (see firmware/main/common/presence_api.cc) rather than erroring loudly.

This sketch queries Home Assistant's REST API for two binary sensors
(mirroring the AtomS3R busy-light project's teams_in_call / webcam_active
sensors) plus HA's calendar entity for today's and tomorrow's events. Swap
`fetch_calendar_days()` for a Microsoft Graph call if you'd rather skip HA.

Event titles are redacted to "Busy" here — do that redaction in this bridge,
not in the firmware, so the real subject never has to leave your network.

Usage: copy server/.env.example to server/.env, fill in real values, then
just run the script — it loads server/.env itself (no `source`/pip install
needed). A real environment variable of the same name always wins over the
file. Any variable also accepts a `<VAR>_FILE` counterpart (e.g.
HA_TOKEN_FILE) to read its value from a file instead — the standard
Docker/Kubernetes secrets convention (see env_file.py's getenv()).

    python3 presence_bridge.py [--port 8080] [--redact-titles] [--env-file path/to/.env]
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

from env_file import load_default_env_file, getenv

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("presence_bridge")

_default_env_file = load_default_env_file(__file__)

HA_URL = getenv("HA_URL", "http://homeassistant.local:8123").rstrip("/")
HA_TOKEN = getenv("HA_TOKEN", "")
HA_CALL_SENSOR = getenv("HA_CALL_SENSOR", "binary_sensor.teams_in_call")
HA_WEBCAM_SENSOR = getenv("HA_WEBCAM_SENSOR", "binary_sensor.webcam_active")
HA_PRESENTING_SENSOR = getenv("HA_PRESENTING_SENSOR", "")
HA_CALENDAR_ENTITY = getenv("HA_CALENDAR_ENTITY", "")
HA_TIME_ZONE = getenv("HA_TIME_ZONE", "W. Europe Standard Time")
# Comma-separated, case-insensitive substrings matched against the raw event
# title (before --redact-titles strips it) to pick the door-fill tier the
# firmware renders. No HA signal maps cleanly to "how important is this
# meeting" or "is anyone else actually on this", so all three lists (customer/
# leadership/solo) are keyword guesses — tune them for your calendar. The
# real Microsoft Graph-backed feed derives "solo" from actual attendee count;
# HA's calendar API doesn't expose attendees, so solo here is a title guess
# same as the other two tiers, not a real headcount.
HA_CUSTOMER_KEYWORDS = [k.strip().lower() for k in getenv("HA_CUSTOMER_KEYWORDS", "customer,client").split(",") if k.strip()]
HA_LEADERSHIP_KEYWORDS = [k.strip().lower() for k in getenv("HA_LEADERSHIP_KEYWORDS", "ceo,cfo,coo,leadership,board").split(",") if k.strip()]
HA_SOLO_KEYWORDS = [k.strip().lower() for k in getenv("HA_SOLO_KEYWORDS", "lunch,focus,personal,doctor,dentist").split(",") if k.strip()]


def classify_tier(title: str) -> str:
    lowered = title.lower()
    if any(k in lowered for k in HA_CUSTOMER_KEYWORDS):
        return "customer"
    if any(k in lowered for k in HA_LEADERSHIP_KEYWORDS):
        return "leadership"
    if any(k in lowered for k in HA_SOLO_KEYWORDS):
        return "solo"
    return "internal"


def ha_get(path: str):
    # Some reverse proxies (Cloudflare included) block urllib's default
    # "Python-urllib/3.x" User-Agent outright (HTTP 403 with no useful
    # body) — a plain browser-looking one avoids that without meaning
    # anything else.
    req = Request(f"{HA_URL}{path}", headers={
        "User-Agent": "Mozilla/5.0 (compatible; fourcolor-eink-firmware/presence_bridge)",
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json",
    })
    with urlopen(req, timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_binary_sensor(entity_id: str) -> bool:
    if not entity_id:
        return False
    try:
        state = ha_get(f"/api/states/{entity_id}")
        return state.get("state") == "on"
    except (URLError, HTTPError, ValueError) as e:
        logger.warning("Failed to read %s: %s", entity_id, e)
        return False


def fetch_calendar_day(day_start: datetime.datetime, redact_titles: bool) -> list:
    if not HA_CALENDAR_ENTITY:
        return []
    day_end = day_start + datetime.timedelta(days=1)
    start_param = day_start.isoformat()
    end_param = day_end.isoformat()

    try:
        items = ha_get(
            f"/api/calendars/{HA_CALENDAR_ENTITY}"
            f"?start={start_param}&end={end_param}"
        )
    except (URLError, HTTPError, ValueError) as e:
        logger.warning("Failed to read calendar %s: %s", HA_CALENDAR_ENTITY, e)
        return []

    events = []
    for item in items:
        try:
            start = datetime.datetime.fromisoformat(item["start"]["dateTime"]).astimezone()
            end = datetime.datetime.fromisoformat(item["end"]["dateTime"]).astimezone()
        except (KeyError, ValueError):
            continue  # skip all-day events (no dateTime, only date)
        summary = item.get("summary", "Busy")
        events.append({
            "start": start.strftime("%H:%M"),
            "end": end.strftime("%H:%M"),
            "title": "Busy" if redact_titles else summary,
            "tier": classify_tier(summary),
        })
    return events


def build_calendar_today(redact_titles: bool) -> dict:
    today_start = datetime.datetime.now().astimezone().replace(hour=0, minute=0, second=0, microsecond=0)
    tomorrow_start = today_start + datetime.timedelta(days=1)
    return {
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "time_zone": HA_TIME_ZONE,
        "days": [
            {"date": today_start.date().isoformat(), "events": fetch_calendar_day(today_start, redact_titles)},
            {"date": tomorrow_start.date().isoformat(), "events": fetch_calendar_day(tomorrow_start, redact_titles)},
        ],
    }


def build_live() -> dict:
    return {
        "updated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "isPresenting": fetch_binary_sensor(HA_PRESENTING_SENSOR),
        "isInCall": fetch_binary_sensor(HA_CALL_SENSOR),
        "isWebcamActive": fetch_binary_sensor(HA_WEBCAM_SENSOR),
    }


def make_handler(redact_titles: bool):
    routes = {
        "/calendar/today": lambda: build_calendar_today(redact_titles),
        "/live": build_live,
    }

    class PresenceHandler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            logger.info("%s - %s", self.address_string(), fmt % args)

        def do_GET(self):
            builder = routes.get(self.path.rstrip("/"))
            if builder is None:
                self.send_response(404)
                self.end_headers()
                return

            try:
                payload = builder()
            except Exception:
                logger.exception("Unexpected error building %s payload", self.path)
                # Empty-but-well-formed body: the firmware parses whichever
                # keys are present and keeps last-known-good for the rest
                # (see ParseCalendarJson/ParseLiveJson in presence_api.cc) —
                # an HTTP error status would achieve the same fallback, but
                # this also surfaces in --redact-titles logs without a scary
                # non-200 in access logs downstream.
                payload = {}

            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return PresenceHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--redact-titles", action="store_true",
                        help="Replace real event titles with 'Busy' before sending to the device")
    parser.add_argument("--env-file", default=_default_env_file,
                         help="Path to a KEY=VALUE .env file to load (already loaded by the time this runs)")
    args = parser.parse_args()

    if not HA_TOKEN:
        logger.warning("HA_TOKEN not set — binary sensor / calendar reads will fail")

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(args.redact_titles))
    logger.info("Presence bridge listening on :%d (HA_URL=%s)", args.port, HA_URL)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
