/**
 * @file presence_api.cc
 * @brief Presence bridge API client implementation
 *
 * Mirrors weather_api.cc's structure: static file-scope state, esp_http_client
 * GET, cJSON parse, esp_timer periodic refresh. Two timers instead of one —
 * see presence_api.h and server/mock_presence_server.py for why the contract
 * is split into /calendar/today (slow) and /live (fast). On fetch failure or
 * malformed/partial response, keeps last-known-good data instead of blanking.
 */

#include "presence_api.h"

#include "settings.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <ctime>

static const char* kTag = "PresenceApi";
static const char* kSettingsNamespace = "presence";
static const char* kEndpointKey = "endpoint";

// /live changes second-to-second in principle, but the e-ink panel takes
// 15-25s to refresh and locks out input during that, so sub-minute polling
// wouldn't be visibly faster. /calendar/today is near-static within a day.
static constexpr int64_t kLivePollIntervalUs = 90LL * 1000000LL;            // 90s
static constexpr int64_t kCalendarPollIntervalUs = 20LL * 60LL * 1000000LL;  // 20min

// ============================================================
// Static state
// ============================================================

static std::string s_endpoint;  // base URL, no trailing slash or path
static PresenceCallback s_callback;
static bool s_initialized = false;
static bool s_live_in_progress = false;
static bool s_calendar_in_progress = false;
static PresenceStatus s_last_data;
static esp_timer_handle_t s_live_timer = nullptr;
static esp_timer_handle_t s_calendar_timer = nullptr;

// ============================================================
// Time parsing
// ============================================================

// Parses "HH:MM" into minutes since midnight. Returns -1 on malformed input.
static int32_t ParseHhMm(const char* text) {
    if (!text) return -1;
    int hh = 0, mm = 0;
    if (sscanf(text, "%d:%d", &hh, &mm) != 2) return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
    return hh * 60 + mm;
}

// Unknown/missing tier defaults to internal — the least alarming reading.
static PresenceEventTier ParseTier(const char* text) {
    if (!text) return PresenceEventTier::kInternal;
    if (strcmp(text, "customer") == 0) return PresenceEventTier::kCustomer;
    if (strcmp(text, "leadership") == 0) return PresenceEventTier::kLeadership;
    if (strcmp(text, "solo") == 0) return PresenceEventTier::kSolo;
    return PresenceEventTier::kInternal;
}

// "YYYY-MM-DD" for local today, or empty on an unsynced RTC — used to pick
// today's entry out of the /calendar/today "days" array.
static std::string TodayDateString() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2020) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    return buf;
}

// ============================================================
// JSON parsing — one function per resource, each only touches the fields
// its resource owns so a failed/partial fetch of one never blanks the other.
// ============================================================

static bool ParseCalendarJson(const char* json, PresenceStatus* out) {
    if (!json || !out) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(kTag, "Failed to parse /calendar/today JSON");
        return false;
    }

    // Contract is now multi-day ("days": [{date, events}, ...]) so a single
    // fetch can also feed a future agenda view; today only consumes the one
    // entry matching the device's local date. Falls back to days[0] if
    // nothing matches (e.g. RTC not synced yet or a TZ mismatch with the
    // server) rather than rendering an empty page.
    cJSON* days = cJSON_GetObjectItem(root, "days");
    cJSON* today_day = nullptr;
    if (cJSON_IsArray(days)) {
        const std::string today = TodayDateString();
        cJSON* day = nullptr;
        cJSON_ArrayForEach(day, days) {
            cJSON* date = cJSON_GetObjectItem(day, "date");
            if (!today.empty() && cJSON_IsString(date) && today == date->valuestring) {
                today_day = day;
                break;
            }
        }
        if (!today_day) {
            today_day = cJSON_GetArrayItem(days, 0);
            if (today_day) {
                ESP_LOGW(kTag, "No day matched local date '%s', using first day in response", today.c_str());
            }
        }
    }

    cJSON* events = today_day ? cJSON_GetObjectItem(today_day, "events") : nullptr;
    if (cJSON_IsArray(events)) {
        std::vector<PresenceEvent> parsed;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, events) {
            cJSON* start = cJSON_GetObjectItem(item, "start");
            cJSON* end = cJSON_GetObjectItem(item, "end");
            cJSON* title = cJSON_GetObjectItem(item, "title");
            cJSON* tier = cJSON_GetObjectItem(item, "tier");
            const int32_t start_min = ParseHhMm(cJSON_IsString(start) ? start->valuestring : nullptr);
            const int32_t end_min = ParseHhMm(cJSON_IsString(end) ? end->valuestring : nullptr);
            if (start_min < 0 || end_min < 0) continue;

            PresenceEvent ev;
            ev.start_minutes = start_min;
            ev.end_minutes = end_min;
            ev.title = (cJSON_IsString(title) && title->valuestring) ? title->valuestring : "";
            ev.tier = ParseTier(cJSON_IsString(tier) ? tier->valuestring : nullptr);
            // "participants"/"participant_count" aren't consumed yet — no
            // renderer surfaces them — but cJSON parsing ignores unread
            // fields, so they're harmless to leave in the payload.
            parsed.push_back(std::move(ev));
        }
        std::sort(parsed.begin(), parsed.end(),
                  [](const PresenceEvent& a, const PresenceEvent& b) { return a.start_minutes < b.start_minutes; });
        out->events = std::move(parsed);
    }
    // No matching day / no "events" key (vs. an explicit empty array) — leave
    // out->events untouched rather than treating a malformed/partial
    // response as "no events today".

    out->last_updated_unix = time(nullptr);
    out->valid = true;

    cJSON_Delete(root);
    return true;
}

static bool ParseLiveJson(const char* json, PresenceStatus* out) {
    if (!json || !out) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(kTag, "Failed to parse /live JSON");
        return false;
    }

    cJSON* presenting = cJSON_GetObjectItem(root, "isPresenting");
    if (cJSON_IsBool(presenting)) out->presenting = cJSON_IsTrue(presenting);

    cJSON* in_call = cJSON_GetObjectItem(root, "isInCall");
    if (cJSON_IsBool(in_call)) out->in_call = cJSON_IsTrue(in_call);

    cJSON* webcam = cJSON_GetObjectItem(root, "isWebcamActive");
    if (cJSON_IsBool(webcam)) out->webcam_active = cJSON_IsTrue(webcam);

    out->last_updated_unix = time(nullptr);
    out->valid = true;

    cJSON_Delete(root);
    return true;
}

// ============================================================
// HTTP client
// ============================================================

// 8KB: the multi-day /calendar/today contract (days[].events[] with
// participants/participant_count strings) runs bigger than the old
// single-day shape — a busy single day with long titles was already flirting
// with 4KB. We don't expect more than today+tomorrow in `days`, but a
// heavier meeting day (more events, long titles/participant lists) is
// plausible, so size for that rather than day count.
static char s_response_buf[8192] = {0};
static int s_response_len = 0;

static esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (s_response_len + evt->data_len < static_cast<int>(sizeof(s_response_buf))) {
                memcpy(s_response_buf + s_response_len, evt->data, evt->data_len);
                s_response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool HttpGet(const std::string& url) {
    s_response_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEventHandler;
    config.timeout_ms = 10000;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status: %d for %s", status, url.c_str());
        esp_http_client_cleanup(client);
        return false;
    }

    s_response_buf[s_response_len] = '\0';
    esp_http_client_cleanup(client);
    return true;
}

static std::string BuildUrl(const char* path) {
    return s_endpoint + path;
}

static void DoFetchCalendar(void* arg) {
    (void)arg;
    if (!s_initialized || s_calendar_in_progress) return;
    if (s_endpoint.empty()) {
        ESP_LOGE(kTag, "Presence endpoint not set");
        return;
    }

    s_calendar_in_progress = true;
    const std::string url = BuildUrl("/calendar/today");
    ESP_LOGI(kTag, "Fetching calendar: %s", url.c_str());
    if (!HttpGet(url) || !ParseCalendarJson(s_response_buf, &s_last_data)) {
        ESP_LOGW(kTag, "Calendar fetch failed, keeping last-known-good data");
        s_calendar_in_progress = false;
        return;
    }

    ESP_LOGI(kTag, "Calendar: events=%d", static_cast<int>(s_last_data.events.size()));
    if (s_callback) s_callback(s_last_data);
    s_calendar_in_progress = false;
}

static void DoFetchLive(void* arg) {
    (void)arg;
    if (!s_initialized || s_live_in_progress) return;
    if (s_endpoint.empty()) {
        ESP_LOGE(kTag, "Presence endpoint not set");
        return;
    }

    s_live_in_progress = true;
    const std::string url = BuildUrl("/live");
    ESP_LOGI(kTag, "Fetching live status: %s", url.c_str());
    if (!HttpGet(url) || !ParseLiveJson(s_response_buf, &s_last_data)) {
        ESP_LOGW(kTag, "Live fetch failed, keeping last-known-good data");
        s_live_in_progress = false;
        return;
    }

    ESP_LOGI(kTag, "Live: in_call=%d webcam=%d presenting=%d",
             s_last_data.in_call, s_last_data.webcam_active, s_last_data.presenting);
    if (s_callback) s_callback(s_last_data);
    s_live_in_progress = false;
}

// ============================================================
// Public API
// ============================================================

void presence_api_init(const char* endpoint, PresenceCallback callback) {
    if (s_initialized) {
        ESP_LOGW(kTag, "Already initialized");
        return;
    }

    Settings settings(kSettingsNamespace, false);
    s_endpoint = settings.GetString(kEndpointKey, endpoint ? endpoint : "");
    s_callback = callback;

    esp_timer_create_args_t live_timer_args = {
        .callback = DoFetchLive,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "presence_live",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&live_timer_args, &s_live_timer) == ESP_OK) {
        esp_timer_start_periodic(s_live_timer, kLivePollIntervalUs);
    } else {
        ESP_LOGE(kTag, "Failed to create live timer");
    }

    esp_timer_create_args_t calendar_timer_args = {
        .callback = DoFetchCalendar,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "presence_calendar",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&calendar_timer_args, &s_calendar_timer) == ESP_OK) {
        esp_timer_start_periodic(s_calendar_timer, kCalendarPollIntervalUs);
    } else {
        ESP_LOGE(kTag, "Failed to create calendar timer");
    }

    s_initialized = true;

    DoFetchLive(nullptr);
    DoFetchCalendar(nullptr);

    ESP_LOGI(kTag, "Presence API initialized: endpoint=%s (live=%llds, calendar=%llds)",
             s_endpoint.c_str(),
             static_cast<long long>(kLivePollIntervalUs / 1000000LL),
             static_cast<long long>(kCalendarPollIntervalUs / 1000000LL));
}

bool presence_api_fetch_now() {
    if (!s_initialized) return false;
    if (s_live_in_progress || s_calendar_in_progress) return false;

    DoFetchLive(nullptr);
    DoFetchCalendar(nullptr);
    return true;
}

void presence_api_set_endpoint(const char* endpoint) {
    s_endpoint = endpoint ? endpoint : "";

    Settings settings(kSettingsNamespace, true);
    settings.SetString(kEndpointKey, s_endpoint);

    presence_api_fetch_now();
}

const char* presence_api_get_endpoint() {
    return s_endpoint.c_str();
}

bool presence_api_is_ready() {
    return s_initialized;
}

const PresenceStatus* presence_api_get_last_data() {
    return &s_last_data;
}
