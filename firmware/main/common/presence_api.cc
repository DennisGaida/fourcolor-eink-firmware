/**
 * @file presence_api.cc
 * @brief Presence bridge API client implementation
 *
 * Mirrors weather_api.cc's structure: static file-scope state, esp_http_client
 * GET, cJSON parse, esp_timer periodic refresh (5 min instead of weather's 1h).
 * On fetch failure or malformed response, keeps last-known-good data instead
 * of blanking (see server/presence_bridge.py for the JSON contract).
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

// ============================================================
// Static state
// ============================================================

static std::string s_endpoint;
static PresenceCallback s_callback;
static bool s_initialized = false;
static bool s_in_progress = false;
static PresenceStatus s_last_data;
static esp_timer_handle_t s_timer = nullptr;

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

// ============================================================
// JSON parsing
// ============================================================

static bool ParsePresenceJson(const char* json, PresenceStatus* out) {
    if (!json || !out) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(kTag, "Failed to parse JSON");
        return false;
    }

    cJSON* status_item = cJSON_GetObjectItem(root, "status");
    const char* status = cJSON_IsString(status_item) ? status_item->valuestring : nullptr;
    if (!status || strcmp(status, "ok") != 0) {
        ESP_LOGE(kTag, "Bridge status != ok: %s", status ? status : "null");
        cJSON_Delete(root);
        return false;
    }

    cJSON* in_call = cJSON_GetObjectItem(root, "in_call");
    out->in_call = cJSON_IsTrue(in_call);

    cJSON* webcam = cJSON_GetObjectItem(root, "webcam_active");
    out->webcam_active = cJSON_IsTrue(webcam);

    cJSON* events = cJSON_GetObjectItem(root, "events");
    out->events.clear();
    if (cJSON_IsArray(events)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, events) {
            cJSON* start = cJSON_GetObjectItem(item, "start");
            cJSON* end = cJSON_GetObjectItem(item, "end");
            cJSON* title = cJSON_GetObjectItem(item, "title");
            const int32_t start_min = ParseHhMm(cJSON_IsString(start) ? start->valuestring : nullptr);
            const int32_t end_min = ParseHhMm(cJSON_IsString(end) ? end->valuestring : nullptr);
            if (start_min < 0 || end_min < 0) continue;

            PresenceEvent ev;
            ev.start_minutes = start_min;
            ev.end_minutes = end_min;
            ev.title = (cJSON_IsString(title) && title->valuestring) ? title->valuestring : "";
            out->events.push_back(std::move(ev));
        }
    }
    std::sort(out->events.begin(), out->events.end(),
              [](const PresenceEvent& a, const PresenceEvent& b) { return a.start_minutes < b.start_minutes; });

    out->last_updated_unix = time(nullptr);
    out->valid = true;

    cJSON_Delete(root);
    return true;
}

// ============================================================
// HTTP client
// ============================================================

static char s_response_buf[4096] = {0};
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

static bool HttpGet(const char* url) {
    s_response_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));

    esp_http_client_config_t config = {};
    config.url = url;
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
        ESP_LOGE(kTag, "HTTP status: %d for %s", status, url);
        esp_http_client_cleanup(client);
        return false;
    }

    s_response_buf[s_response_len] = '\0';
    esp_http_client_cleanup(client);
    return true;
}

static void DoFetch(void* arg) {
    (void)arg;
    if (!s_initialized || s_in_progress) return;

    if (s_endpoint.empty()) {
        ESP_LOGE(kTag, "Presence endpoint not set");
        return;
    }

    s_in_progress = true;

    ESP_LOGI(kTag, "Fetching presence: %s", s_endpoint.c_str());
    PresenceStatus data;
    if (!HttpGet(s_endpoint.c_str()) || !ParsePresenceJson(s_response_buf, &data)) {
        // Keep last-known-good data on failure or malformed response.
        ESP_LOGW(kTag, "Presence fetch failed, keeping last-known-good data");
        s_in_progress = false;
        return;
    }

    s_last_data = data;
    ESP_LOGI(kTag, "Presence: in_call=%d webcam=%d events=%d",
             data.in_call, data.webcam_active, static_cast<int>(data.events.size()));

    if (s_callback) {
        s_callback(data);
    }

    s_in_progress = false;
}

// ============================================================
// Timer callback
// ============================================================

static void TimerCallback(void* arg) {
    ESP_LOGD(kTag, "5-minute presence refresh triggered");
    DoFetch(arg);
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

    esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "presence_refresh",
        .skip_unhandled_events = true,
    };

    if (esp_timer_create(&timer_args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, 5LL * 60LL * 1000000LL);
        ESP_LOGI(kTag, "Timer started (5min interval)");
    } else {
        ESP_LOGE(kTag, "Failed to create timer");
    }

    s_initialized = true;

    DoFetch(nullptr);

    ESP_LOGI(kTag, "Presence API initialized: endpoint=%s", s_endpoint.c_str());
}

bool presence_api_fetch_now() {
    if (!s_initialized) return false;
    if (s_in_progress) return false;

    DoFetch(nullptr);
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
