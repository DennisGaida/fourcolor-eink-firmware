#!/usr/bin/env python3
"""
mock_presence_server.py — mock implementation of the busy-light data contract.

Serves two GET endpoints, split because the data behind them changes at very
different rates:

    GET /calendar/today   — today's events, near-static within a day
    {
      "date": "2026-09-14",
      "generated_at": "2026-09-14T08:00:00+02:00",
      "events": [
        {"id": "evt1", "start": "09:00", "end": "09:30", "title": "Standup", "tier": "internal"}
      ]
    }

    GET /live              — presence state, can change second-to-second
    {
      "updated_at": "2026-09-14T10:32:05+02:00",
      "isPresenting": false,
      "isInCall": false,
      "isWebcamActive": false
    }

`tier` is one of "internal" / "leadership" / "customer" (drives the door-fill
style on the firmware's detail-face day grid).

This is a mock, not a bridge: both endpoints return the same hardcoded data
below every time (edit the constants to try other scenarios). No real
backend (Home Assistant, Power Automate, Microsoft Graph, ...) is queried
here — that's a separate, later step, implemented as its own script against
this same contract.

Usage:
    python3 server/mock_presence_server.py [--port 8080]
"""

import argparse
import datetime
import json
import logging
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("mock_presence_server")

# Mirrors the "Busy Light Display" design doc's example day (also used as the
# on-device mock in firmware/main/application.cc's BuildMockPresenceStatus).
MOCK_EVENTS = [
    {"id": "evt1", "start": "09:00", "end": "09:30", "title": "Standup", "tier": "internal"},
    {"id": "evt2", "start": "10:00", "end": "11:30", "title": "Acme Corp — QBR", "tier": "customer"},
    {"id": "evt3", "start": "11:45", "end": "12:15", "title": "Board prep — CFO", "tier": "leadership"},
    {"id": "evt4", "start": "13:00", "end": "14:00", "title": "Sprint planning", "tier": "internal"},
    {"id": "evt5", "start": "15:30", "end": "16:00", "title": "1:1 with Sam", "tier": "internal"},
]

MOCK_IS_PRESENTING = True
MOCK_IS_IN_CALL = False
MOCK_IS_WEBCAM_ACTIVE = True


def now_iso() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def build_calendar_today() -> dict:
    return {
        "date": datetime.date.today().isoformat(),
        "generated_at": now_iso(),
        "events": MOCK_EVENTS,
    }


def build_live() -> dict:
    return {
        "updated_at": now_iso(),
        "isPresenting": MOCK_IS_PRESENTING,
        "isInCall": MOCK_IS_IN_CALL,
        "isWebcamActive": MOCK_IS_WEBCAM_ACTIVE,
    }


ROUTES = {
    "/calendar/today": build_calendar_today,
    "/live": build_live,
}


def make_handler():
    class MockPresenceHandler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            logger.info("%s - %s", self.address_string(), fmt % args)

        def do_GET(self):
            path = self.path.rstrip("/")
            builder = ROUTES.get(path)
            if builder is None:
                self.send_response(404)
                self.end_headers()
                return

            try:
                payload = builder()
            except Exception:
                logger.exception("Unexpected error building %s payload", path)
                payload = {}

            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return MockPresenceHandler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler())
    logger.info("Mock presence server listening on :%d (routes: %s)",
                args.port, ", ".join(ROUTES))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
