# Minimal container for the busy-light presence/calendar bridge
# (calendar_bridge.py by default; presence_bridge.py is also included as an
# all-Home-Assistant alternative - see the docstring at the top of each
# script for the difference).
#
# No third-party Python dependencies: both bridge scripts only use the
# standard library (http.server, urllib, json), so the image needs nothing
# beyond a slim Python base.
#
# Configuration is via environment variables only (see .env.example in this
# folder for the full list) - inject them with `docker run -e KEY=value`,
# `docker run --env-file`, or docker-compose.yml's `env_file:`. Nothing
# secret is baked into the image; .env itself is never copied in (see
# .dockerignore).
FROM python:3.12-slim

WORKDIR /app

# Only the bridge scripts + their shared loader - not the mock server or the
# local dev/debug helpers (mock_client.py, press_button.py, screenshot.py),
# which need a real device on the LAN and have no place in a deployed image.
COPY calendar_bridge.py presence_bridge.py env_file.py ./

ENV PYTHONUNBUFFERED=1
EXPOSE 8080

# /live always returns HTTP 200 by design (see calendar_bridge.py's
# docstring: internal errors fall back to "nothing going on" defaults
# rather than erroring), so this only fails if the process itself is dead
# or unresponsive - a plain liveness check, not an upstream-reachability
# check. No curl/wget in this slim image, so use Python's stdlib instead.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD python3 -c "import urllib.request as u; import sys; sys.exit(0 if u.urlopen('http://127.0.0.1:8080/live', timeout=3).status == 200 else 1)"

# calendar_bridge.py is the default (webhook calendar + HA presence). Switch
# to presence_bridge.py (all-HA) by overriding the container command, e.g.
# in docker-compose.yml: `command: ["presence_bridge.py", "--port", "8080"]`.
ENTRYPOINT ["python3"]
CMD ["calendar_bridge.py", "--port", "8080"]
