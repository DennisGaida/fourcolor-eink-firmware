#!/usr/bin/env python3
"""
serial_tool.py — drives the firmware's serial (COM port) debug command
interface (see firmware/main/common/serial_control.cc) so the same
screenshot / button-press / page-switch dev-loop operations that
screenshot.py and press_button.py do over LAN HTTP also work when the LAN
HTTP server is off, using only the USB-serial link already used for
flashing and logs.

Requires pyserial:
    pip install pyserial

Usage:
    python3 serial_tool.py <port> screenshot [output.png]
    python3 serial_tool.py <port> button <type>
    python3 serial_tool.py <port> page <name>

<type> is one of: boot_click, boot_double_click, boot_long_press,
                  up_click, up_double_click, up_long_press,
                  down_click, down_double_click, down_long_press
<name> is one of: busylight, weather, gallery, settings

The console UART also carries ESP_LOG lines interleaved with command
responses; this script simply ignores any line that isn't part of the
expected response framing (BEGIN_SCREENSHOT/END_SCREENSHOT markers, or a
line starting with OK/ERR).
"""

import base64
import sys
import time

import serial  # pyserial

# Same palette as screenshot.py, kept in sync manually since this is a
# small standalone dev-loop script (see server/screenshot.py for the
# LAN-HTTP equivalent of the /screenshot decode logic).
PALETTE = {
    0: (20, 20, 20),      # BLACK
    1: (245, 245, 240),   # WHITE
    2: (235, 200, 40),    # YELLOW
    3: (200, 30, 30),     # RED
}

VALID_BUTTON_TYPES = {
    "boot_click", "boot_double_click", "boot_long_press",
    "up_click", "up_double_click", "up_long_press",
    "down_click", "down_double_click", "down_long_press",
}
VALID_PAGE_NAMES = {"busylight", "weather", "gallery", "settings"}

BAUD_RATE = 115200


def open_port(port: str) -> serial.Serial:
    # Opening a serial port on Windows/pyserial asserts DTR/RTS by default,
    # which pulses the ESP32-S3's EN pin via its auto-reset circuit (the
    # same mechanism esptool uses to reset into the bootloader) — every new
    # connection would otherwise reboot the device before the command is
    # even sent. Construct the Serial object without auto-opening so DTR/RTS
    # can be deasserted *before* the port is actually opened (setting them
    # after an auto-opened constructor is too late — the reset pulse has
    # already happened by then).
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD_RATE
    ser.timeout = 2
    ser.dsrdtr = False
    ser.rtscts = False
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def send_command(ser: serial.Serial, command: str) -> None:
    ser.write((command + "\n").encode("utf-8"))
    ser.flush()


def read_line(ser: serial.Serial, timeout_s: float = 5.0) -> str:
    """Read one line, ignoring interleaved ESP_LOG output (log lines start
    with 'I (', 'W (', 'E (', etc. and never collide with our markers)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        return raw.decode("utf-8", errors="replace").rstrip("\r\n")
    raise TimeoutError("No response from device")


def read_response_line(ser: serial.Serial, timeout_s: float = 5.0) -> str:
    """Like read_line, but skips any interleaved ESP_LOG lines and returns
    only the first line that looks like a command response (OK/ERR)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = read_line(ser, timeout_s=max(0.1, deadline - time.time()))
        if line.startswith("OK") or line.startswith("ERR"):
            return line
    raise TimeoutError("No command response from device")


def cmd_screenshot(ser: serial.Serial, out_path: str) -> None:
    send_command(ser, "screenshot")
    header = None
    b64_chunks = []
    deadline = time.time() + 15.0
    while time.time() < deadline:
        line = read_line(ser)
        if line.startswith("BEGIN_SCREENSHOT"):
            header = line.split()
            continue
        if line == "END_SCREENSHOT":
            break
        if line.startswith("ERR"):
            print(line)
            sys.exit(1)
        if header is not None:
            b64_chunks.append(line)
    else:
        raise TimeoutError("Screenshot did not complete in time")

    if header is None:
        raise RuntimeError("Never saw BEGIN_SCREENSHOT")

    width, height = int(header[1]), int(header[2])
    raw = base64.b64decode("".join(b64_chunks))

    from PIL import Image  # lazy import: only needed for this subcommand

    bytes_per_row = (width * 2 + 7) // 8
    img = Image.new("RGB", (width, height))
    pixels = img.load()
    for y in range(height):
        row_off = y * bytes_per_row
        for x in range(width):
            byte = raw[row_off + (x >> 2)]
            shift = 6 - ((x & 0x03) << 1)
            code = (byte >> shift) & 0x03
            pixels[x, y] = PALETTE[code]
    img.save(out_path)
    print(f"Saved {width}x{height} screenshot to {out_path}")


def cmd_button(ser: serial.Serial, button_type: str) -> None:
    if button_type not in VALID_BUTTON_TYPES:
        print(f"Unknown button type '{button_type}'. Valid: {sorted(VALID_BUTTON_TYPES)}")
        sys.exit(1)
    send_command(ser, f"button {button_type}")
    print(read_response_line(ser))


def cmd_page(ser: serial.Serial, page_name: str) -> None:
    if page_name not in VALID_PAGE_NAMES:
        print(f"Unknown page '{page_name}'. Valid: {sorted(VALID_PAGE_NAMES)}")
        sys.exit(1)
    send_command(ser, f"page {page_name}")
    print(read_response_line(ser))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    port, action = sys.argv[1], sys.argv[2]
    ser = open_port(port)
    try:
        if action == "screenshot":
            out_path = sys.argv[3] if len(sys.argv) > 3 else "screenshot.png"
            cmd_screenshot(ser, out_path)
        elif action == "button":
            if len(sys.argv) < 4:
                print(__doc__)
                sys.exit(1)
            cmd_button(ser, sys.argv[3])
        elif action == "page":
            if len(sys.argv) < 4:
                print(__doc__)
                sys.exit(1)
            cmd_page(ser, sys.argv[3])
        else:
            print(__doc__)
            sys.exit(1)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
