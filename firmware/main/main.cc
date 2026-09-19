#include <esp_log.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_pm.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

// Persists across soft resets and deep sleep, cleared only on power-on.
// Used to avoid bouncing more than once per software reset.
static RTC_DATA_ATTR bool s_sw_reset_bounced = false;

// Dynamic frequency scaling: let the CPU idle down to 80MHz (== APB clock,
// so no XTAL clock-source switch and no APB/SPI timing impact) whenever no
// component holds an ESP_PM_CPU_FREQ_MAX/APB_FREQ_MAX lock, instead of
// staying pinned at 240MHz for the whole session. Light sleep is
// deliberately left disabled here; see tmp/optimizations-todo.md.
static void ConfigurePowerManagement() {
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = false;
    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "PM configured: DFS 80-240MHz, light sleep disabled");
    }
}

static void LogNvsStats() {
    nvs_stats_t stats = {};
    esp_err_t ret = nvs_get_stats(nullptr, &stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS stats read failed: %s", esp_err_to_name(ret));
        return;
    }
    uint32_t used = stats.used_entries;
    uint32_t total = stats.total_entries;
    uint32_t free_entries = stats.free_entries;
    uint32_t percent = (total > 0) ? (used * 100U / total) : 0;
    ESP_LOGI(TAG, "NVS stats: used=%u free=%u total=%u (%u%%) namespaces=%u",
             used, free_entries, total, percent, stats.namespace_count);
}

extern "C" void app_main(void)
{
    // Bootloader app-rollback is enabled (ota_0/ota_1), which boots each new
    // image in the "pending verify" state and reverts to the previous slot
    // after enough reboots unless something marks it valid. Nothing else in
    // this firmware calls esp_ota_mark_app_valid_cancel_rollback(), so every
    // OTA update was silently rolling back a few reboots after flashing.
    {
        esp_ota_img_states_t ota_state;
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running != nullptr &&
            esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Marked running app valid, rollback cancelled");
        }
    }

    // Some soft/external reset paths leave the Wi-Fi RF state dirty until the
    // next hardware-equivalent reset. For those reset reasons only, perform a
    // brief deep-sleep round-trip once to come back with clean radio state.
    {
        const auto reason = esp_reset_reason();
        ESP_LOGI(TAG, "Boot reset reason=%d bounced=%d", reason, s_sw_reset_bounced ? 1 : 0);
        // Keep this narrowly scoped. External reset after flashing should boot
        // like a normal hardware reset; bouncing that path through deep sleep
        // has proven flaky on this board. Only software-reset paths still get
        // the one-shot deep-sleep bounce.
        const bool need_bounce = !s_sw_reset_bounced &&
                                 (reason == ESP_RST_SW);
        if (need_bounce) {
            ESP_LOGI(TAG, "Reset reason %d — bouncing via deep sleep for clean Wi-Fi init", reason);
            s_sw_reset_bounced = true;
            esp_sleep_enable_timer_wakeup(500000ULL);  // 500 ms
            esp_deep_sleep_start();
        }
        s_sw_reset_bounced = false;
    }  // clear for next time

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    LogNvsStats();

    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
