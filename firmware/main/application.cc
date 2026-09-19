#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/photo_storage.h"
#include "common/presence_api.h"
#include "display.h"
#include "i18n.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"

#include <esp_mac.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <ctime>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
constexpr int kSyncIntervalDefault = CONFIG_DEFAULT_SYNC_INTERVAL_MIN;
constexpr char kGalleryNamespace[] = "gallery";
constexpr char kSlideshowIntervalKey[] = "slide_min";
constexpr int kSlideshowIntervalDefault = CONFIG_DEFAULT_SLIDESHOW_INTERVAL_MIN;
constexpr char kNetworkNamespace[] = "network";
constexpr char kLanServerEnabledKey[] = "lan_srv_on";
// Off by default: the LAN gallery/photo webserver blocks scheduled deep
// sleep for as long as it runs (see IsLocalHttpServiceRunning() below), so
// auto-starting it on every WiFi connect silently prevented the device from
// ever sleeping. Users who want it opt in via Settings, and that choice is
// persisted so it only comes back if they asked for it.
#ifdef CONFIG_DEFAULT_LAN_SERVER_ENABLED
constexpr bool kLanServerEnabledDefault = true;
#else
constexpr bool kLanServerEnabledDefault = false;
#endif
constexpr int kSettingsSlideshowIndex = 4;
constexpr int kSettingsWifiIndex = 6;
constexpr int kSettingsHttpServerIndex = 7;
constexpr int kSettingsLanIpIndex = 8;
constexpr int kSettingsQuietHoursIndex = 9;
constexpr ui::RawDrawPageId kDefaultIdlePage = ui::RawDrawPageId::BusyLight;

// "Quiet hours" scheduled deep sleep: nothing on the busy-light page (or any
// other page) needs to update overnight or over the weekend, so the device
// can deep-sleep through those windows and wake automatically instead of
// needing a BOOT press. Weeknight window: 19:00 -> next day 06:00. Weekend
// window: Friday 18:00 -> Monday 06:00 (Saturday/Sunday are fully quiet).
// All time boundaries are build-time Kconfig values (see "Deployment
// defaults" > quiet hours in Kconfig.projbuild) rather than hardcoded here.
constexpr char kQuietHoursNamespace[] = "quiet_hours";
constexpr char kQuietHoursEnabledKey[] = "enabled";
#ifdef CONFIG_DEFAULT_QUIET_HOURS_ENABLED
constexpr bool kQuietHoursEnabledDefault = true;
#else
constexpr bool kQuietHoursEnabledDefault = false;
#endif
constexpr int kQuietHoursWakeMinute =
    CONFIG_QUIET_HOURS_WAKE_HOUR * 60 + CONFIG_QUIET_HOURS_WAKE_MINUTE;
constexpr int kQuietHoursWeekdaySleepMinute =
    CONFIG_QUIET_HOURS_WEEKDAY_SLEEP_HOUR * 60 + CONFIG_QUIET_HOURS_WEEKDAY_SLEEP_MINUTE;
constexpr int kQuietHoursFridaySleepMinute =
    CONFIG_QUIET_HOURS_FRIDAY_SLEEP_HOUR * 60 + CONFIG_QUIET_HOURS_FRIDAY_SLEEP_MINUTE;
// Grace window after boot/button activity before quiet-hours auto-sleep can
// kick in again, so a manual BOOT-button wake during quiet hours (per
// requirements) leaves the device usable for a few minutes rather than
// snapping back to sleep on the very next Run() loop tick.
constexpr int64_t kQuietHoursGraceMs = CONFIG_QUIET_HOURS_GRACE_MINUTES * 60 * 1000;

// tm_wday: 0=Sun, 1=Mon, ..., 5=Fri, 6=Sat.
bool IsQuietHoursNow(const struct tm& local_tm) {
    const int wday = local_tm.tm_wday;
    const int minutes = local_tm.tm_hour * 60 + local_tm.tm_min;

    // Saturday and Sunday are entirely inside the Friday-evening-to-Monday-
    // morning weekend window.
    if (wday == 6 || wday == 0) return true;

    // Every other day: still quiet until the morning wake time - this is the
    // tail end of the previous night's window (for Monday, the tail of the
    // weekend window).
    if (minutes < kQuietHoursWakeMinute) return true;

    if (wday == 5) return minutes >= kQuietHoursFridaySleepMinute;  // Friday
    return minutes >= kQuietHoursWeekdaySleepMinute;                // Mon-Thu
}

// Computes the next local-time epoch at which quiet hours end (06:00 on the
// next day that isn't itself fully inside the weekend window).
time_t ComputeNextQuietHoursWakeEpoch(time_t now) {
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);

    struct tm wake_tm = tm_now;
    wake_tm.tm_hour = 6;
    wake_tm.tm_min = 0;
    wake_tm.tm_sec = 0;
    wake_tm.tm_mday += 1;
    time_t candidate = mktime(&wake_tm);

    for (int guard = 0; guard < 8; ++guard) {
        struct tm tm_candidate = {};
        localtime_r(&candidate, &tm_candidate);
        if (tm_candidate.tm_wday != 6 && tm_candidate.tm_wday != 0) {
            break;
        }
        struct tm next_tm = tm_candidate;
        next_tm.tm_mday += 1;
        next_tm.tm_hour = 6;
        next_tm.tm_min = 0;
        next_tm.tm_sec = 0;
        candidate = mktime(&next_tm);
    }
    return candidate;
}

// Base URL of the presence server (see server/mock_presence_server.py for
// the contract: GET <endpoint>/calendar/today + GET <endpoint>/live). Can be
// a plain LAN IP or a real DNS hostname (e.g. behind a reverse proxy) -
// esp_http_client resolves it via the normal DNS resolver either way; only
// .local/mDNS names won't work since no mDNS component is wired in. Set via
// Kconfig ("Deployment defaults" > Presence/calendar bridge base URL,
// CONFIG_PRESENCE_BRIDGE_ENDPOINT) rather than hardcoded here - update it
// (or use presence_api_set_endpoint() at runtime) to match wherever the
// bridge actually runs.
constexpr char kPresenceBridgeEndpoint[] = CONFIG_PRESENCE_BRIDGE_ENDPOINT;

std::string FormatMinutesLabel(int minutes) {
    if (minutes <= 0) return i18n::Tr(i18n::StringId::kOff);
    char buf[16];
    snprintf(buf, sizeof(buf), "%dmin", minutes);
    return buf;
}

const char* FormatMinutesLogLabel(int minutes) {
    return minutes <= 0 ? i18n::Tr(i18n::StringId::kOff) : i18n::Tr(i18n::StringId::kOn);
}

int NextSlideshowInterval(int current) {
    static constexpr int kOptions[] = {0, 5, 10, 30};
    for (size_t i = 0; i < sizeof(kOptions) / sizeof(kOptions[0]); ++i) {
        if (kOptions[i] == current) {
            return kOptions[(i + 1) % (sizeof(kOptions) / sizeof(kOptions[0]))];
        }
    }
    return 5;
}

void UpdateWifiSettingsItem(rawdraw::SettingsRenderer* renderer, bool connected,
                            const char* value = nullptr) {
    if (!renderer) return;
    renderer->UpdateChecked(kSettingsWifiIndex, connected);
    renderer->UpdateItem(kSettingsWifiIndex, value ? value : (connected ? i18n::Tr(i18n::StringId::kConnected) : i18n::Tr(i18n::StringId::kDisconnected)));
}

void UpdateHttpServerSettingsItem(rawdraw::SettingsRenderer* renderer, bool running,
                                  const std::string& ip_address = "") {
    if (!renderer) return;
    std::string value;
    if (running && !ip_address.empty()) {
        value = "http://" + ip_address;
    } else if (!ip_address.empty()) {
        value = ip_address;
    } else {
        value = running ? i18n::Tr(i18n::StringId::kOn2) : i18n::Tr(i18n::StringId::kOff2);
    }
    renderer->UpdateChecked(kSettingsHttpServerIndex, running);
    renderer->UpdateItem(kSettingsHttpServerIndex, value);
}

void UpdateLanIpSettingsItem(rawdraw::SettingsRenderer* renderer, const std::string& ip_address) {
    if (!renderer) return;
    renderer->UpdateItem(kSettingsLanIpIndex, ip_address.empty() ? i18n::Tr(i18n::StringId::kNotObtained) : ip_address);
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "europe.pool.ntp.org");
    esp_sntp_setservername(2, "time.google.com");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().UpdateStatusBarForUi();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=Europe/Berlin servers=pool.ntp.org,europe.pool.ntp.org,time.google.com");
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

void StartPresenceApiOnce() {
    static bool s_started = false;
    if (s_started) return;
    s_started = true;

    presence_api_init(kPresenceBridgeEndpoint, [](const PresenceStatus& status) {
        auto* manager = Application::GetInstance().GetRawDrawUiManager();
        if (manager) {
            manager->UpdatePresenceStatus(status);
        }
    });
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
        esp_timer_delete(sleep_timer_);
        sleep_timer_ = nullptr;
    }
    if (wifi_ps_settle_timer_ != nullptr) {
        esp_timer_stop(wifi_ps_settle_timer_);
        esp_timer_delete(wifi_ps_settle_timer_);
        wifi_ps_settle_timer_ = nullptr;
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        Settings gallery_nvs(kGalleryNamespace, false);
        int slideshow_interval = gallery_nvs.GetInt(kSlideshowIntervalKey, kSlideshowIntervalDefault);
        if (slideshow_interval != 0 && slideshow_interval != 5 &&
            slideshow_interval != 10 && slideshow_interval != 30) {
            slideshow_interval = kSlideshowIntervalDefault;
        }
        ESP_LOGI(kTag, "Startup gallery fullscreen slideshow: %s, interval=%s",
                 FormatMinutesLogLabel(slideshow_interval),
                 FormatMinutesLabel(slideshow_interval).c_str());
        rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(slideshow_interval);

        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({i18n::Tr(i18n::StringId::kSystem), "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({i18n::Tr(i18n::StringId::kRestart), i18n::Tr(i18n::StringId::kRun), nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({i18n::Tr(i18n::StringId::kLanguage),
                         i18n::Tr(i18n::StringId::kLanguageDisplayName),
                         nullptr, rawdraw::SettingsItemType::Action, false,
                         []() {
                             // Rebuilding every settings label/value in place would
                             // also require re-deriving live WiFi/HTTP-server/IP
                             // state (currently only pushed by network callbacks,
                             // not re-derivable on demand), so switching language
                             // restarts the device instead - same as the "重启/
                             // Restart" action above.
                             i18n::SetLanguage(i18n::GetLanguage() == i18n::Language::kEnUS
                                 ? i18n::Language::kZhCN : i18n::Language::kEnUS);
                             esp_restart();
                         }});
        items.push_back({i18n::Tr(i18n::StringId::kGallery), "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({i18n::Tr(i18n::StringId::kSlideshowInterval), FormatMinutesLabel(slideshow_interval), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this, sr]() {
                             Settings nvs(kGalleryNamespace, true);
                             const int current = nvs.GetInt(kSlideshowIntervalKey, kSlideshowIntervalDefault);
                             const int next = NextSlideshowInterval(current);
                             nvs.SetInt(kSlideshowIntervalKey, next);
                             if (rawdraw_ui_manager_) {
                                 rawdraw_ui_manager_->SetGallerySlideshowIntervalMinutes(next);
                             }
                             if (next > 0 && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while gallery slideshow is enabled");
                             } else if (next <= 0 &&
                                        (wifi_connected_.load(std::memory_order_acquire) ||
                                         WifiManager::GetInstance().IsConnected())) {
                                 ArmSyncSleepTimer();
                             }
                             sr->UpdateItem(kSettingsSlideshowIndex, FormatMinutesLabel(next));
                         }});
        items.push_back({i18n::Tr(i18n::StringId::kNetwork), "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"Wi-Fi", i18n::Tr(i18n::StringId::kDisconnected), nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             auto& wifi = WifiManager::GetInstance();
                             if (wifi_connected_.load(std::memory_order_acquire) || wifi.IsConnected()) {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled OFF");
                                 if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                     rawdraw_ui_manager_->StopLanHttpServer();
                                     UpdateHttpServerSettingsItem(sr, false);
                                 }
                                 wifi.StopStation();
                                 wifi_connected_.store(false, std::memory_order_release);
                                 UpdateWifiSettingsItem(sr, false);
                                 UpdateLanIpSettingsItem(sr, "");
                             } else {
                                 ESP_LOGI(kTag, "Wi-Fi setting toggled ON");
                                 UpdateWifiSettingsItem(sr, false, i18n::Tr(i18n::StringId::kConnecting));
                                 wifi.StartStation();
                             }
                             UpdateStatusBarForUi();
                         }});
        items.push_back({i18n::Tr(i18n::StringId::kLanServer), i18n::Tr(i18n::StringId::kOff2), nullptr, rawdraw::SettingsItemType::Checkbox, false,
                         [this, sr]() {
                             if (!rawdraw_ui_manager_) return;
                             if (rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                                 ESP_LOGI(kTag, "LAN HTTP server toggled OFF");
                                 Settings(kNetworkNamespace, true).SetBool(kLanServerEnabledKey, false);
                                 rawdraw_ui_manager_->StopLanHttpServer();
                                 UpdateHttpServerSettingsItem(sr, false);
                                 UpdateStatusBarForUi();
                                 if (wifi_connected_.load(std::memory_order_acquire) ||
                                     WifiManager::GetInstance().IsConnected()) {
                                     ArmSyncSleepTimer();
                                 }
                                 return;
                             }

                             auto& wifi = WifiManager::GetInstance();
                             if (!wifi_connected_.load(std::memory_order_acquire) && !wifi.IsConnected()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires WiFi connection");
                                 UpdateHttpServerSettingsItem(sr, false, i18n::Tr(i18n::StringId::kConnectWifiFirst));
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const std::string ip = wifi.GetIpAddress();
                             if (ip.empty()) {
                                 ESP_LOGW(kTag, "LAN HTTP server requires station IP");
                                 UpdateHttpServerSettingsItem(sr, false, i18n::Tr(i18n::StringId::kWaitingForIp));
                                 UpdateStatusBarForUi();
                                 return;
                             }
                             const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                             ESP_LOGI(kTag, "LAN HTTP server toggled ON: started=%d url=http://%s/",
                                      started ? 1 : 0, ip.c_str());
                             if (started) {
                                 Settings(kNetworkNamespace, true).SetBool(kLanServerEnabledKey, true);
                             }
                             if (started && sleep_timer_ != nullptr) {
                                 esp_timer_stop(sleep_timer_);
                                 ESP_LOGI(kTag, "Sync sleep timer paused while LAN HTTP server is running");
                             }
                             UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                             UpdateLanIpSettingsItem(sr, started ? ip : WifiManager::GetInstance().GetIpAddress());
                             UpdateStatusBarForUi();
                         }});
        items.push_back({i18n::Tr(i18n::StringId::kLanIp), i18n::Tr(i18n::StringId::kNotObtained), nullptr, rawdraw::SettingsItemType::Normal, false});
        {
            const bool quiet_hours_enabled = Settings(kQuietHoursNamespace, false)
                .GetBool(kQuietHoursEnabledKey, kQuietHoursEnabledDefault);
            items.push_back({"Quiet Hours",
                             quiet_hours_enabled ? i18n::Tr(i18n::StringId::kOn2) : i18n::Tr(i18n::StringId::kOff2),
                             nullptr, rawdraw::SettingsItemType::Checkbox, quiet_hours_enabled,
                             [this, sr]() {
                                 Settings nvs(kQuietHoursNamespace, true);
                                 const bool enabled = !nvs.GetBool(kQuietHoursEnabledKey, kQuietHoursEnabledDefault);
                                 nvs.SetBool(kQuietHoursEnabledKey, enabled);
                                 ESP_LOGI(kTag, "Quiet hours toggled %s", enabled ? "ON" : "OFF");
                                 sr->UpdateChecked(kSettingsQuietHoursIndex, enabled);
                                 sr->UpdateItem(kSettingsQuietHoursIndex,
                                                enabled ? i18n::Tr(i18n::StringId::kOn2) : i18n::Tr(i18n::StringId::kOff2));
                             }});
        }
        items.push_back({i18n::Tr(i18n::StringId::kPowerSaving), i18n::Tr(i18n::StringId::kEnterManually), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [this]() {
                             ESP_LOGI(kTag, "Manual sleep requested from settings");
                             EnterManualSleep();
                         }});
        items.push_back({i18n::Tr(i18n::StringId::kAbout), "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({i18n::Tr(i18n::StringId::kFirmware), PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    NoteQuietHoursActivity();
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(kTag, "Wake from deep sleep: flash activity LED and refresh UI");
        board.FlashActivityLed();
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->RequestActivePageRefresh();
        }
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                StartPresenceApiOnce();
                if (rawdraw_ui_manager_ && !rawdraw_ui_manager_->IsLanHttpServerRunning() &&
                    Settings(kNetworkNamespace, false).GetBool(kLanServerEnabledKey, kLanServerEnabledDefault)) {
                    const std::string ip = data.empty() ? WifiManager::GetInstance().GetIpAddress() : data;
                    if (!ip.empty()) {
                        const bool started = rawdraw_ui_manager_->StartLanHttpServer(ip);
                        ESP_LOGI(kTag, "LAN HTTP server auto-start after WiFi: started=%d url=http://%s/",
                                 started ? 1 : 0, ip.c_str());
                        if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
                            UpdateHttpServerSettingsItem(sr, started, started ? ip : "");
                            UpdateLanIpSettingsItem(sr, ip);
                        }
                    }
                }
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(kDefaultIdlePage);
                }
                UpdateStatusBarForUi();
                ArmSyncSleepTimer();
                ArmWifiPowerSaveSettleTimer();
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                if (wifi_ps_settle_timer_ != nullptr) {
                    esp_timer_stop(wifi_ps_settle_timer_);
                }
                if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
                    rawdraw_ui_manager_->StopLanHttpServer();
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer &&
                    !rawdraw_ui_manager_->IsApTransferModeRunning()) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(kDefaultIdlePage);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    // Start network (non-blocking, WiFi connects asynchronously)
    board.RequestNetwork();

    SetDeviceState(kDeviceStateIdle);
}

void Application::OnUpClick() {
    ESP_LOGI(kTag, "UP click");
    NoteQuietHoursActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    ESP_LOGI(kTag, "DOWN click");
    NoteQuietHoursActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpDoubleClick() {
    ESP_LOGI(kTag, "UP double click");
    NoteQuietHoursActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpDoubleClick});
    }
}

void Application::OnUpLongPress() {
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        ESP_LOGI(kTag, "UP long press - leaving settings");
        rawdraw_ui_manager_->SwitchPage(kDefaultIdlePage);
    }
}

void Application::OnDownLongPress() {
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    ESP_LOGI(kTag, "BOOT click");
    NoteQuietHoursActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootLongPress() {
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(kDefaultIdlePage);
        }
        WifiManager::GetInstance().StartStation();
        return;
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::NoteButtonActivity() {
    NoteQuietHoursActivity();
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::ArmSyncSleepTimer() {
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while local HTTP transfer service is running");
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        if (sleep_timer_ != nullptr) {
            esp_timer_stop(sleep_timer_);
        }
        ESP_LOGI(kTag, "Sync sleep timer skipped while gallery slideshow is enabled");
        return;
    }

    Settings nvs(kSyncNamespace, false);
    const int interval_minutes = nvs.GetInt(kSyncIntervalKey, kSyncIntervalDefault);
    if (interval_minutes <= 0) {
        ESP_LOGI(kTag, "Sync sleep interval: off");
        return;
    }
    if (sleep_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->EnterScheduledSleep();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_sync_sleep";
        ESP_ERROR_CHECK(esp_timer_create(&args, &sleep_timer_));
    }
    esp_timer_stop(sleep_timer_);
    const int64_t delay_us = static_cast<int64_t>(interval_minutes) * 60 * 1000 * 1000;
    ESP_LOGI(kTag, "Sync sleep interval: %d minutes", interval_minutes);
    ESP_LOGI(kTag, "Scheduling sleep after sync interval: %d minutes", interval_minutes);
    ESP_ERROR_CHECK(esp_timer_start_once(sleep_timer_, delay_us));
}

// WIFI_PS_NONE (full radio power) is the connect-time default because
// modem sleep during association/DHCP previously caused slow TCP
// handshakes and a high HTTP failure rate (see wifi_station.cc). Once the
// station has a stable IP, presence/calendar polling and any LAN transfers
// are steady-state traffic rather than a fresh handshake, so we dial back
// to BALANCED (WIFI_PS_MIN_MODEM) a few seconds after connecting to save
// power without touching the connect-path behavior that was fixed before.
//
// TODO(power-optimizations): this is unverified on real hardware. Watch
// device logs for ~60 minutes after flashing for HTTP failures / timeouts
// from presence_api or the LAN server; if BALANCED reintroduces the old
// failure mode, revert to leaving WIFI_PS_NONE for the whole session.
void Application::ArmWifiPowerSaveSettleTimer() {
    if (wifi_ps_settle_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->ApplySteadyStateWifiPowerSave();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_wifi_ps_settle";
        ESP_ERROR_CHECK(esp_timer_create(&args, &wifi_ps_settle_timer_));
    }
    esp_timer_stop(wifi_ps_settle_timer_);
    constexpr int64_t kSettleDelayUs = 10 * 1000 * 1000;  // 10s
    ESP_ERROR_CHECK(esp_timer_start_once(wifi_ps_settle_timer_, kSettleDelayUs));
}

void Application::ApplySteadyStateWifiPowerSave() {
    if (!wifi_connected_.load(std::memory_order_acquire)) {
        return;
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        // ap_transfer_server forces WIFI_PS_NONE for the duration of a
        // transfer session; don't fight it back down to BALANCED.
        ESP_LOGI(kTag, "Skipping WiFi power-save switch: LAN transfer server is running");
        return;
    }
    ESP_LOGI(kTag, "WiFi settled after connect; switching to BALANCED power save");
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::BALANCED);
}

void Application::EnterScheduledSleep() {
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: local HTTP transfer service is running");
        ArmSyncSleepTimer();
        return;
    }
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        ESP_LOGI(kTag, "Scheduled sleep skipped: gallery slideshow is enabled");
        ArmSyncSleepTimer();
        return;
    }

    ESP_LOGI(kTag, "Entering deep sleep after sync interval; BOOT wakes device");
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering manual deep sleep; stopping local services and WiFi");
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsHttpServerRunning()) {
        rawdraw_ui_manager_->StopApTransferMode();
    }
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    UpdateStatusBarForUi();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        MaybeEnterQuietHoursSleep();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void Application::NoteQuietHoursActivity() {
    quiet_hours_active_until_ms_ = esp_timer_get_time() / 1000 + kQuietHoursGraceMs;
}

// Independent of the generic sync-interval nap timer (ArmSyncSleepTimer /
// EnterScheduledSleep) above, which some users disable entirely (sync
// interval = 0) to keep pages like busy-light continuously live. Quiet
// hours needs to work regardless of that setting, so it gets its own
// per-second check and its own deep-sleep entry point.
void Application::MaybeEnterQuietHoursSleep() {
    if (!Settings(kQuietHoursNamespace, false).GetBool(kQuietHoursEnabledKey, kQuietHoursEnabledDefault)) {
        return;
    }

    time_t now = 0;
    time(&now);
    struct tm local_tm = {};
    localtime_r(&now, &local_tm);
    if (local_tm.tm_year + 1900 < 2024) {
        // System clock not yet SNTP-synced (or RTC not seeded); don't guess.
        return;
    }
    if (!IsQuietHoursNow(local_tm)) {
        return;
    }
    if (esp_timer_get_time() / 1000 < quiet_hours_active_until_ms_) {
        // Recent boot or button press: leave the device usable for a short
        // grace window before considering it idle again.
        return;
    }
    if (IsLocalHttpServiceRunning(rawdraw_ui_manager_.get())) {
        return;
    }
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->GetGallerySlideshowIntervalMinutes() > 0) {
        return;
    }

    ESP_LOGI(kTag, "Quiet hours: entering deep sleep");
    EnterQuietHoursSleep();
}

void Application::EnterQuietHoursSleep() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
    }
    if (wifi_ps_settle_timer_ != nullptr) {
        esp_timer_stop(wifi_ps_settle_timer_);
    }

    time_t now = 0;
    time(&now);
    const time_t wake_epoch = ComputeNextQuietHoursWakeEpoch(now);
    const double seconds_until_wake = difftime(wake_epoch, now);

    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (seconds_until_wake > 0) {
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds_until_wake) * 1000000ULL);
    }
    // BOOT still wakes the device early, same as the other sleep paths.
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    ESP_LOGI(kTag, "Quiet hours: sleeping for ~%.0f minutes (BOOT wakes early)",
             seconds_until_wake / 60.0);
    esp_deep_sleep_start();
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return false;
}

void Application::UpdateStatusBarForUi() {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        const bool http_server_running = rawdraw_ui_manager_->IsHttpServerRunning();
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = http_server_running;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        UpdateWifiSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), wifi_connected);
        const std::string lan_ip = wifi_connected ? WifiManager::GetInstance().GetIpAddress() : "";
        UpdateLanIpSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(), lan_ip);
        UpdateHttpServerSettingsItem(rawdraw_ui_manager_->GetSettingsRenderer(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning(),
                                     rawdraw_ui_manager_->IsLanHttpServerRunning()
                                         ? lan_ip
                                         : "");
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return;
}
