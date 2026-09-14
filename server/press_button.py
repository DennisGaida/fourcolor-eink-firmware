#!/usr/bin/env python3
"""
press_button.py — injects a button event into the firmware's /button debug
endpoint (see ApTransferServer::ButtonHandler), so a page toggle (e.g. the
busy-light BOOT click) can be driven from the dev loop instead of a hand on
the physical board.

Usage:
    python3 press_button.py <device-ip> <type>

<type> is one of: boot_click, boot_double_click, boot_long_press,
up_click, up_double_click, up_long_press,
down_click, down_double_click, down_long_press
"""

import sys
from urllib.request import urlopen

VALID_TYPES = {
    "boot_click", "boot_double_click", "boot_long_press",
    "up_click", "up_double_click", "up_long_press",
    "down_click", "down_double_click", "down_long_press",
}


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in VALID_TYPES:
        print(__doc__)
        sys.exit(1)

    device_ip, button_type = sys.argv[1], sys.argv[2]
    url = f"http://{device_ip}/button?type={button_type}"
    print(f"Fetching {url} ...")
    with urlopen(url, timeout=10) as resp:
        print(resp.read().decode("utf-8"))


if __name__ == "__main__":
    main()
