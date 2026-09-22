# Serial Remote Control

The firmware exposes a small command interface over the same USB/serial port
used for flashing and logs (e.g. `COM9` on Windows, `/dev/ttyACM0` on
Linux/macOS). It lets you grab a screenshot, inject a synthetic button press,
or jump directly to a page — without touching the device — even when the LAN
debug HTTP server (`/screenshot`, `/button`, `/page` on `ApTransferServer`,
see below) is disabled, since it's opt-in via a Settings toggle and off by
default.

This is especially useful as a **dev-loop speedup after flashing**: instead
of physically looking at the device or pressing buttons by hand, take a
screenshot to confirm the result, or drive it entirely via serial commands.

## Requirements

```bash
pip install pyserial pillow
```

## Host-side CLI: `server/serial_tool.py`

```bash
python server/serial_tool.py <port> screenshot [output.png]
python server/serial_tool.py <port> button <type>
python server/serial_tool.py <port> page <name>
```

Examples (Windows, device on `COM9`):

```powershell
python server\serial_tool.py COM9 screenshot check.png
python server\serial_tool.py COM9 button boot_click
python server\serial_tool.py COM9 page weather
```

`<type>` is one of:
`boot_click`, `boot_double_click`, `boot_long_press`,
`up_click`, `up_double_click`, `up_long_press`,
`down_click`, `down_double_click`, `down_long_press`.

`<name>` is one of: `busylight`, `weather`, `gallery`, `settings`.

Each invocation opens a fresh connection, runs one command, and exits —
there is no persistent session to keep open.

### Important: don't open the port yourself without care

Opening a serial port normally asserts DTR/RTS, which pulses the ESP32-S3's
EN pin via its auto-reset circuit (the same mechanism `esptool` uses to
reset into the bootloader) — this would reboot an already-running device
before your command is even sent. `serial_tool.py`'s `open_port()` already
works around this by constructing the `Serial` object unopened, setting
`dtr = False` / `rts = False` *before* calling `.open()`. If you write your
own client against this interface, replicate that pattern — setting
DTR/RTS low *after* an auto-opened `serial.Serial(port, ...)` constructor is
too late, since the reset pulse already happened during `.open()`.

## Wire protocol

Commands are newline-terminated ASCII lines sent to the port; responses are
also newline-terminated lines. ESP_LOG output is interleaved on the same
port (this is the same port used for the console log), so any client must
skip lines that aren't part of the expected response framing — the host CLI
already does this (only lines starting with `OK`/`ERR`, or the
`BEGIN_SCREENSHOT`/`END_SCREENSHOT` markers, are treated as responses).

| Command | Response |
| --- | --- |
| `screenshot` | `BEGIN_SCREENSHOT <w> <h>` line, then base64-encoded raw 2bpp framebuffer data (same packed layout as the LAN `/screenshot` endpoint) wrapped across multiple lines, then `END_SCREENSHOT` |
| `button <type>` | `OK` or `ERR <reason>` |
| `page <name>` | `OK` or `ERR <reason>` |
| `help` | Usage text |

Unknown commands, missing arguments, or an unrecognized `<type>`/`<name>`
all return `ERR <reason>`.

## Notes on timing

- Right after a flash or reset, the device can take up to ~20-30s before the
  command handlers are wired up (audio codec, photo storage, display, and
  Wi-Fi all initialize first) — commands sent too early return
  `ERR ... handler unavailable`, not a timeout. Wait for the device to
  finish booting (or just retry).
- The 4-color e-ink panel's busy-wait during a refresh can take up to ~120s
  in the worst case; a `page`/`button` command that triggers a refresh will
  return its `OK`/`ERR` immediately, but a *subsequent* `screenshot` may show
  a mid-refresh or stale frame if issued while the panel is still busy.

## Implementation

- Firmware side: `firmware/main/common/serial_control.h` / `.cc`. Uses the
  native USB Serial/JTAG peripheral directly
  (`usb_serial_jtag_read_bytes`/`write_bytes` from `driver/usb_serial_jtag.h`)
  rather than stdio over UART0 — this board has no external UART-to-USB
  bridge, so physical UART0 is never actually connected to the host PC, only
  the native USB peripheral is (confirmed via `sdkconfig`'s
  `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`).
- `serial_control::InitUartEarly()` installs the driver and starts the
  command task as the very first statement in `app_main()`, before anything
  else runs.
- `serial_control::SetCallbacks(...)` is wired up later in
  `Application::Initialize()`, once `RawDrawUiManager` exists, delegating to
  its `CaptureFramebufferSnapshot()`, `InjectButtonEvent()`, and
  `SwitchToPageByName()` methods — the same shared implementations used by
  the LAN HTTP `/screenshot`, `/button`, and `/page` endpoints on
  `ApTransferServer` (see `firmware/main/ui/renderers/rawdraw/ap_transfer_server.h`).
- Host side: `server/serial_tool.py`.
