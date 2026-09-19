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
