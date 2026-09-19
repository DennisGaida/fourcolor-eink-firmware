"""
env_file.py — minimal KEY=VALUE .env loader shared by the bridge scripts.

No extra dependency (e.g. python-dotenv) for something this small. Load it
before reading any os.environ.get(...) config constants, since those are
computed once at import time.
"""

import os
import sys


def load_env_file(path: str) -> None:
    """Never overrides a variable already present in the real environment,
    so `FOO=bar python3 calendar_bridge.py` still wins over server/.env."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            lines = f.readlines()
    except OSError:
        return
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def getenv(key: str, default: str = "") -> str:
    """Like os.environ.get(key, default), but also supports the long-
    standing `<KEY>_FILE` Docker/Kubernetes secrets convention: if
    `<KEY>_FILE` is set, its content is read from that path (e.g.
    /run/secrets/<name> for a `docker secret`/Compose secret) and used as
    the value instead, taking priority over `KEY` itself. This lets
    CALENDAR_SOURCE_SECRET/HA_TOKEN/etc. be supplied as files rather than
    plaintext environment variables, without adding an extra dependency.
    A trailing newline is stripped, since secret files are commonly
    written ending in one."""
    file_path = os.environ.get(f"{key}_FILE")
    if file_path:
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                return f.read().rstrip("\n")
        except OSError as exc:
            print(f"Failed to read {key}_FILE={file_path!r}: {exc}", file=sys.stderr)
            sys.exit(1)
    return os.environ.get(key, default)


def load_default_env_file(caller_file: str, arg_name: str = "--env-file") -> str:
    """Looks for `arg_name value` in sys.argv (argparse hasn't run yet at
    this point in the importing script), else falls back to a `.env` file
    next to `caller_file`. Returns the path that was (attempted to be)
    loaded, so the caller can register it as the argparse default too."""
    default_path = os.path.join(os.path.dirname(os.path.abspath(caller_file)), ".env")
    path = default_path
    if arg_name in sys.argv:
        idx = sys.argv.index(arg_name)
        if idx + 1 < len(sys.argv):
            path = sys.argv[idx + 1]
    load_env_file(path)
    return default_path
