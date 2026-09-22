/**
 * @file serial_control.h
 * @brief Line-based debug command interface over the device's USB
 * Serial/JTAG port (the same COM port used for flashing and logs — this
 * board has no external UART-to-USB bridge, so physical UART0 isn't wired
 * to the PC at all).
 *
 * Mirrors the LAN debug endpoints (/screenshot, /button, /page on
 * ApTransferServer) so the same three dev-loop operations — grab a
 * framebuffer screenshot, inject a synthetic button press, jump directly to
 * a named page — also work when the LAN HTTP server is off (e.g. LAN
 * Service disabled in Settings), using only the serial link already used
 * for flashing and logs.
 *
 * Commands (newline-terminated):
 *   screenshot          -> "BEGIN_SCREENSHOT <w> <h>\n<base64 lines>\nEND_SCREENSHOT"
 *   button <type>       -> "OK" / "ERR <reason>" (types match /button's ?type=)
 *   page <name>         -> "OK" / "ERR <reason>" (names match /page's ?id=)
 *   help                -> usage text
 *
 * See server/serial_tool.py for the host-side client.
 */

#ifndef SERIAL_CONTROL_H
#define SERIAL_CONTROL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace serial_control {

// Local, UI-layer-independent mirror of ApTransferServer::FramebufferSnapshot
// so this module doesn't need to depend on ui/ headers.
struct ScreenshotSnapshot {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;  // raw 2bpp packed framebuffer, same layout as /screenshot
};

using ScreenshotCallback = std::function<ScreenshotSnapshot()>;
using ButtonCallback = std::function<void(const std::string& type)>;
using PageCallback = std::function<bool(const std::string& name)>;

/**
 * @brief Install the USB Serial/JTAG driver (if not already installed) and
 * start the serial command task. Called as early as possible in app_main,
 * before any other component runs, so the RX ring buffer is ready before
 * anything could plausibly send a command. Safe to call once. Callbacks
 * may be attached later via SetCallbacks() once their owning objects exist;
 * commands received before that report "handler unavailable".
 */
void InitUartEarly();

/**
 * @brief Attach the screenshot/button/page callbacks once their owning
 * objects (e.g. RawDrawUiManager) have been constructed.
 */
void SetCallbacks(ScreenshotCallback screenshot_cb, ButtonCallback button_cb, PageCallback page_cb);

}  // namespace serial_control

#endif  // SERIAL_CONTROL_H
