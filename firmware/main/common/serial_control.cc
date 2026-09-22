/**
 * @file serial_control.cc
 * @brief Implementation of the USB Serial/JTAG debug command interface.
 *
 * See serial_control.h for the command list and why this uses the native
 * USB Serial/JTAG peripheral directly (driver/usb_serial_jtag.h) instead of
 * stdin/stdout on the UART console: this board exposes only USB (no
 * external UART-to-USB bridge), so physical UART0 is never connected to
 * the host PC, and going through the raw read/write byte API avoids any
 * interaction with however ESP-IDF's primary/secondary console VFS plumbing
 * happens to be configured.
 */

#include "serial_control.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

namespace serial_control {

namespace {

constexpr const char* kTag = "SerialControl";
constexpr int kLineBufSize = 96;
constexpr int kBase64LineChars = 200;  // chars printed per output line

ScreenshotCallback g_screenshot_cb;
ButtonCallback g_button_cb;
PageCallback g_page_cb;

void WriteLine(const char* text) {
    usb_serial_jtag_write_bytes(text, strlen(text), portMAX_DELAY);
    static const char kNewline[] = "\r\n";
    usb_serial_jtag_write_bytes(kNewline, 2, portMAX_DELAY);
}

void WriteLine(const std::string& text) { WriteLine(text.c_str()); }

void WriteFormatted(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    WriteLine(buf);
}

void PrintUsage() {
    WriteLine("Commands:");
    WriteLine("  screenshot          dump current framebuffer as base64");
    WriteLine("  button <type>       inject a button event (boot_click, up_long_press, ...)");
    WriteLine("  page <name>         switch to a page (busylight, weather, gallery, settings)");
    WriteLine("  help                show this message");
}

void HandleScreenshot() {
    if (!g_screenshot_cb) {
        WriteLine("ERR screenshot handler unavailable");
        return;
    }
    ScreenshotSnapshot snap = g_screenshot_cb();
    if (snap.data.empty() || snap.width <= 0 || snap.height <= 0) {
        WriteLine("ERR empty framebuffer");
        return;
    }

    const size_t out_cap = 4 * ((snap.data.size() + 2) / 3) + 4;
    std::unique_ptr<unsigned char[]> encoded(new (std::nothrow) unsigned char[out_cap]);
    if (!encoded) {
        WriteLine("ERR out of memory encoding screenshot");
        return;
    }
    size_t out_len = 0;
    const int rc = mbedtls_base64_encode(encoded.get(), out_cap, &out_len, snap.data.data(), snap.data.size());
    if (rc != 0) {
        WriteFormatted("ERR base64 encode failed (%d)", rc);
        return;
    }

    WriteFormatted("BEGIN_SCREENSHOT %d %d", snap.width, snap.height);
    for (size_t offset = 0; offset < out_len; offset += kBase64LineChars) {
        const size_t chunk = std::min(static_cast<size_t>(kBase64LineChars), out_len - offset);
        usb_serial_jtag_write_bytes(encoded.get() + offset, chunk, portMAX_DELAY);
        static const char kNewline[] = "\r\n";
        usb_serial_jtag_write_bytes(kNewline, 2, portMAX_DELAY);
    }
    WriteLine("END_SCREENSHOT");
}

void HandleButton(const char* arg) {
    if (!arg || *arg == '\0') {
        WriteLine("ERR missing button type");
        return;
    }
    if (!g_button_cb) {
        WriteLine("ERR button handler unavailable");
        return;
    }
    g_button_cb(arg);
    WriteLine("OK");
}

void HandlePage(const char* arg) {
    if (!arg || *arg == '\0') {
        WriteLine("ERR missing page name");
        return;
    }
    if (!g_page_cb) {
        WriteLine("ERR page handler unavailable");
        return;
    }
    if (g_page_cb(arg)) {
        WriteLine("OK");
    } else {
        WriteFormatted("ERR unknown page '%s'", arg);
    }
}

void DispatchLine(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char* saveptr = nullptr;
    const char* cmd = strtok_r(line, " \t", &saveptr);
    if (!cmd) return;
    const char* arg = strtok_r(nullptr, " \t", &saveptr);

    if (strcmp(cmd, "screenshot") == 0) {
        HandleScreenshot();
    } else if (strcmp(cmd, "button") == 0) {
        HandleButton(arg);
    } else if (strcmp(cmd, "page") == 0) {
        HandlePage(arg);
    } else if (strcmp(cmd, "help") == 0) {
        PrintUsage();
    } else {
        WriteFormatted("ERR unknown command '%s' (try 'help')", cmd);
    }
}

void SerialControlTask(void*) {
    char line[kLineBufSize];
    size_t len = 0;
    ESP_LOGI(kTag, "Serial control task ready (send 'help' over the USB serial port)");
    while (true) {
        uint8_t byte = 0;
        const int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (byte == '\n' || byte == '\r') {
            if (len > 0) {
                line[len] = '\0';
                DispatchLine(line);
                len = 0;
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = static_cast<char>(byte);
        }
        // Silently drop overlong lines' extra bytes; DispatchLine on the
        // next newline will still act on whatever fit in the buffer.
    }
}

}  // namespace

void InitUartEarly() {
    // The secondary console (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
    // already uses this peripheral for log output, but that doesn't
    // necessarily install the full interrupt-driven driver needed for
    // usb_serial_jtag_read_bytes(). Install it ourselves; if something else
    // already did, this just returns an error we ignore.
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&config);

    xTaskCreate(SerialControlTask, "serial_ctrl", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
}

void SetCallbacks(ScreenshotCallback screenshot_cb, ButtonCallback button_cb, PageCallback page_cb) {
    g_screenshot_cb = std::move(screenshot_cb);
    g_button_cb = std::move(button_cb);
    g_page_cb = std::move(page_cb);
}

}  // namespace serial_control
