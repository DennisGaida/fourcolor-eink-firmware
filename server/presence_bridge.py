#!/usr/bin/env python3
"""
presence_bridge.py — HTTP bridge for the busy-light day-view firmware page.

Serves a single GET endpoint the ESP32 polls every 5 minutes:

    GET /presence
    {
      "status": "ok",
      "generated_at": "2026-09-12T14:32:00+02:00",
      "in_call": false,
      "webcam_active": false,
      "events": [
        {"start": "09:00", "end": "09:30", "title": "Standup"},
        {"start": "10:00", "end": "11:30", "title": "Design review"}
      ]
    }

`start`/`end` are local "HH:MM" strings — no timezone math needed on-device.
On any internal error this still returns HTTP 200 with "status" != "ok" so the
firmware's HttpGet() succeeds and ParsePresenceJson() falls back to keeping
its last-known-good data (see firmware/main/common/presence_api.cc).

This sketch queries Home Assistant's REST API for two binary sensors
(mirroring the AtomS3R busy-light project's teams_in_call / webcam_active
sensors) plus HA's calendar entity for today's events. Swap
`fetch_presence()` for a Microsoft Graph call if you'd rather skip HA.

Event titles are redacted to "Busy" here — do that redaction in this bridge,
not in the firmware, so the real subject never has to leave your network.

Usage:
    HA_URL=http://homeassistant.local:8123 \\
    HA_TOKEN=your_long_lived_access_token \\
    HA_CALL_SENSOR=binary_sensor.teams_in_call \\
    HA_WEBCAM_SENSOR=binary_sensor.webcam_active \\
    HA_CALENDAR_ENTITY=calendar.your_calendar \\
    python3 presence_bridge.py [--port 8080] [--redact-titles]
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
logger = logging.getLogger("presence_bridge")

HA_URL = os.environ.get("HA_URL", "http://homeassistant.local:8123")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
HA_CALL_SENSOR = os.environ.get("HA_CALL_SENSOR", "binary_sensor.teams_in_call")
HA_WEBCAM_SENSOR = os.environ.get("HA_WEBCAM_SENSOR", "binary_sensor.webcam_active")
HA_CALENDAR_ENTITY = os.environ.get("HA_CALENDAR_ENTITY", "")


def ha_get(path: str):
    req = Request(f"{HA_URL}{path}", headers={
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


def fetch_calendar_events(redact_titles: bool) -> list:
    if not HA_CALENDAR_ENTITY:
        return []
    now = datetime.datetime.now().astimezone()
    day_start = now.replace(hour=0, minute=0, second=0, microsecond=0)
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
        events.append({
            "start": start.strftime("%H:%M"),
            "end": end.strftime("%H:%M"),
            "title": "Busy" if redact_titles else item.get("summary", "Busy"),
        })
    return events


def fetch_presence(redact_titles: bool) -> dict:
    return {
        "status": "ok",
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "in_call": fetch_binary_sensor(HA_CALL_SENSOR),
        "webcam_active": fetch_binary_sensor(HA_WEBCAM_SENSOR),
        "events": fetch_calendar_events(redact_titles),
    }


def make_handler(redact_titles: bool):
    class PresenceHandler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            logger.info("%s - %s", self.address_string(), fmt % args)

        def do_GET(self):
            if self.path.rstrip("/") != "/presence":
                self.send_response(404)
                self.end_headers()
                return

            try:
                payload = fetch_presence(redact_titles)
            except Exception as e:
                logger.exception("Unexpected error building presence payload")
                payload = {"status": "error", "message": str(e)}

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
