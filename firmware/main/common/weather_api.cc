/**
 * @file weather_api.cc
 * @brief OpenWeatherMap One Call 3.0 client implementation
 *
 * Single HTTPS GET to /data/3.0/onecall (hourly/minutely excluded) returns
 * current conditions, an 8-day daily forecast, and any active real
 * government weather alerts in one response - typically only ~5KB.
 *
 *   https://api.openweathermap.org/data/3.0/onecall?lat=LAT&lon=LON&appid=KEY&units=metric|imperial&exclude=minutely,hourly
 *
 * `daily[0]` is today (used for the hi/lo + alert scan), `daily[1..3]` are
 * the next 3 days shown as forecast cards.
 */

#include "weather_api.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

static const char* kTag = "WeatherApi";

#if defined(CONFIG_WEATHER_UNITS_FAHRENHEIT) && CONFIG_WEATHER_UNITS_FAHRENHEIT
static constexpr bool kUseFahrenheit = true;
#else
static constexpr bool kUseFahrenheit = false;
#endif

// Alert-bar thresholds - tune here if they turn out too chatty/quiet.
namespace {
constexpr int kRainChanceThresholdPct = 40;   // percent (OWM pop is 0-1, x100)
constexpr int kSnowChanceThresholdPct = 40;   // percent
constexpr double kWindThresholdKph = 40.0;    // ~Beaufort 6 / moderate gale
constexpr double kWindThresholdMph = 25.0;
constexpr double kHeatThresholdC = 30.0;
constexpr double kHeatThresholdF = 86.0;

// ============================================================
// Static state
// ============================================================

char s_api_key[64] = {0};
char s_location[64] = {0};  // "lat,lon"
WeatherCallback s_callback;
bool s_initialized = false;
bool s_in_progress = false;
WeatherData s_last_data;
esp_timer_handle_t s_timer = nullptr;

// One Call 3.0's response (current + 8-day daily + alerts, hourly/minutely
// excluded) runs ~4-8KB - far smaller than WeatherAPI.com's old ~50-60KB
// forecast.json, so a much smaller buffer suffices. Still allocated from
// PSRAM to keep internal RAM/.bss free.
constexpr size_t kResponseBufCapacity = 16 * 1024;
char* s_response_buf = nullptr;
int s_response_len = 0;

// The HTTP/TLS fetch itself runs on a dedicated task with a generous stack.
// It must NOT run inline on the esp_event loop task ("sys_evt", 4KB stack)
// or the esp_timer task (3.5KB stack) - both are too small for an
// esp_http_client HTTPS request through the mbedTLS cert bundle and will
// stack-overflow and reboot the device mid-handshake. Both the initial
// fetch and the hourly timer just notify this task to do the work.
constexpr uint32_t kFetchTaskStackSize = 8192;
constexpr UBaseType_t kFetchTaskPriority = 4;
TaskHandle_t s_fetch_task = nullptr;

// ============================================================
// Small helpers
// ============================================================

std::string ToUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// `epoch` here is expected to already be location-local (dt + timezone_offset
// from the API response) - gmtime_r is used deliberately instead of
// localtime_r so the device's own timezone setting is never applied on top,
// which would double-adjust the already-local timestamp.
std::string WeekdayLabel3(time_t epoch) {
    struct tm tm_val = {};
    gmtime_r(&epoch, &tm_val);
    char buf[8] = {0};
    strftime(buf, sizeof(buf), "%a", &tm_val);
    return ToUpper(buf);
}

std::string FullDateLabel(time_t epoch) {
    struct tm tm_val = {};
    gmtime_r(&epoch, &tm_val);
    char buf[40] = {0};
    strftime(buf, sizeof(buf), "%A / %b %d", &tm_val);
    return ToUpper(buf);
}

double GetNum(cJSON* obj, const char* key, double fallback = 0) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

std::string GetStr(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

struct DayAlertInputs {
    std::string label;
    int chance_rain = 0;   // percent
    int chance_snow = 0;   // percent
    double max_wind = 0;   // configured unit (kph or mph)
    double max_temp = 0;   // configured unit (C or F)
};

WeatherAlert EvaluateHeuristicAlert(const std::vector<DayAlertInputs>& days) {
    const double wind_threshold = kUseFahrenheit ? kWindThresholdMph : kWindThresholdKph;
    const double heat_threshold = kUseFahrenheit ? kHeatThresholdF : kHeatThresholdC;

    // Checked as separate passes (not one pass picking the first match) so
    // priority is rain > snow > wind > heat regardless of which day it
    // occurs on - a rainy day 2 days out still outranks a windy today.
    for (const auto& day : days) {
        if (day.chance_rain >= kRainChanceThresholdPct) return {WeatherAlertType::kRain, day.label, ""};
    }
    for (const auto& day : days) {
        if (day.chance_snow >= kSnowChanceThresholdPct) return {WeatherAlertType::kSnow, day.label, ""};
    }
    for (const auto& day : days) {
        if (day.max_wind >= wind_threshold) return {WeatherAlertType::kWind, day.label, ""};
    }
    for (const auto& day : days) {
        if (day.max_temp >= heat_threshold) return {WeatherAlertType::kHeat, day.label, ""};
    }
    return {WeatherAlertType::kNone, "", ""};
}

// Classifies a real OWM alerts[] entry into rain/snow/wind/heat/kOfficial
// by keyword-matching its event name, so the existing alert-bar icon set
// can still be used even though the wording is arbitrary official text.
WeatherAlertType ClassifyOfficialAlertIcon(const std::string& event_upper) {
    if (event_upper.find("SNOW") != std::string::npos || event_upper.find("ICE") != std::string::npos ||
        event_upper.find("FROST") != std::string::npos) {
        return WeatherAlertType::kSnow;
    }
    if (event_upper.find("RAIN") != std::string::npos || event_upper.find("FLOOD") != std::string::npos ||
        event_upper.find("THUNDERSTORM") != std::string::npos) {
        return WeatherAlertType::kRain;
    }
    if (event_upper.find("WIND") != std::string::npos || event_upper.find("GALE") != std::string::npos ||
        event_upper.find("GUST") != std::string::npos || event_upper.find("STORM") != std::string::npos) {
        return WeatherAlertType::kWind;
    }
    if (event_upper.find("HEAT") != std::string::npos || event_upper.find("HOT") != std::string::npos) {
        return WeatherAlertType::kHeat;
    }
    return WeatherAlertType::kOfficial;  // no keyword match - still show it, just no themed icon
}

// ============================================================
// JSON parsing
// ============================================================

bool ParseOneCallJson(const char* json, WeatherData* out) {
    if (!json || !out) return false;

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(kTag, "Failed to parse JSON response");
        return false;
    }

    cJSON* cod = cJSON_GetObjectItem(root, "cod");
    if (cJSON_IsNumber(cod) || cJSON_IsString(cod)) {
        std::string msg = GetStr(root, "message");
        ESP_LOGE(kTag, "API error: %s", msg.empty() ? "unknown" : msg.c_str());
        cJSON_Delete(root);
        return false;
    }

    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsObject(current) || !cJSON_IsArray(daily) || cJSON_GetArraySize(daily) == 0) {
        ESP_LOGE(kTag, "Response missing current/daily[]");
        cJSON_Delete(root);
        return false;
    }

    // One Call 3.0 gives dt in UTC epoch + a timezone_offset (seconds) for
    // the location - adding them yields the location's local wall-clock
    // time, which WeekdayLabel3/FullDateLabel read with gmtime_r.
    const double timezone_offset = GetNum(root, "timezone_offset");

    out->location_name.clear();  // One Call 3.0 doesn't return a place name

    time_t current_dt = static_cast<time_t>(GetNum(current, "dt"));
    time_t local_epoch = current_dt + static_cast<time_t>(timezone_offset);
    out->date_label = FullDateLabel(local_epoch);

    out->temp = static_cast<int32_t>(std::lround(GetNum(current, "temp")));
    cJSON* weather_arr = cJSON_GetObjectItem(current, "weather");
    cJSON* weather0 = cJSON_IsArray(weather_arr) ? cJSON_GetArrayItem(weather_arr, 0) : nullptr;
    if (cJSON_IsObject(weather0)) {
        out->condition_text = GetStr(weather0, "description");
        out->condition_code = static_cast<int32_t>(GetNum(weather0, "id"));
    }

    std::vector<DayAlertInputs> alert_days;
    const int day_count = cJSON_GetArraySize(daily);

    cJSON* today_entry = cJSON_GetArrayItem(daily, 0);
    if (cJSON_IsObject(today_entry)) {
        cJSON* today_temp = cJSON_GetObjectItem(today_entry, "temp");
        out->temp_max_today = static_cast<int32_t>(std::lround(GetNum(today_temp, "max")));
        out->temp_min_today = static_cast<int32_t>(std::lround(GetNum(today_temp, "min")));

        cJSON* today_weather_arr = cJSON_GetObjectItem(today_entry, "weather");
        cJSON* today_weather0 = cJSON_IsArray(today_weather_arr) ? cJSON_GetArrayItem(today_weather_arr, 0) : nullptr;
        const int32_t today_code = cJSON_IsObject(today_weather0)
                                        ? static_cast<int32_t>(GetNum(today_weather0, "id"))
                                        : 0;
        const WeatherIcon today_icon = WeatherIconForCode(today_code);

        DayAlertInputs today_input;
        today_input.label = "TODAY";
        const double pop_pct = GetNum(today_entry, "pop") * 100.0;
        today_input.chance_rain = (today_icon == WeatherIcon::Rain || today_icon == WeatherIcon::Thunder)
                                       ? static_cast<int>(pop_pct) : 0;
        today_input.chance_snow = (today_icon == WeatherIcon::Snow) ? static_cast<int>(pop_pct) : 0;
        double wind = GetNum(today_entry, "wind_speed");
        today_input.max_wind = kUseFahrenheit ? wind : wind * 3.6;  // OWM metric wind_speed is m/s
        today_input.max_temp = GetNum(today_temp, "max");
        alert_days.push_back(today_input);
    }

    out->forecast.clear();
    for (int i = 1; i < day_count && out->forecast.size() < 3; ++i) {
        cJSON* day_entry = cJSON_GetArrayItem(daily, i);
        if (!cJSON_IsObject(day_entry)) continue;

        WeatherForecastDay item;
        cJSON* dt_item = cJSON_GetObjectItem(day_entry, "dt");
        if (cJSON_IsNumber(dt_item)) {
            time_t day_local = static_cast<time_t>(dt_item->valuedouble) + static_cast<time_t>(timezone_offset);
            item.weekday_label = WeekdayLabel3(day_local);
        }

        cJSON* day_weather_arr = cJSON_GetObjectItem(day_entry, "weather");
        cJSON* day_weather0 = cJSON_IsArray(day_weather_arr) ? cJSON_GetArrayItem(day_weather_arr, 0) : nullptr;
        if (cJSON_IsObject(day_weather0)) {
            item.condition_text = GetStr(day_weather0, "description");
            item.condition_code = static_cast<int32_t>(GetNum(day_weather0, "id"));
        }
        cJSON* day_temp = cJSON_GetObjectItem(day_entry, "temp");
        item.temp_max = static_cast<int32_t>(std::lround(GetNum(day_temp, "max")));
        item.temp_min = static_cast<int32_t>(std::lround(GetNum(day_temp, "min")));
        out->forecast.push_back(item);

        const WeatherIcon day_icon = WeatherIconForCode(item.condition_code);
        DayAlertInputs day_input;
        day_input.label = item.weekday_label;
        const double pop_pct = GetNum(day_entry, "pop") * 100.0;
        day_input.chance_rain = (day_icon == WeatherIcon::Rain || day_icon == WeatherIcon::Thunder)
                                     ? static_cast<int>(pop_pct) : 0;
        day_input.chance_snow = (day_icon == WeatherIcon::Snow) ? static_cast<int>(pop_pct) : 0;
        double wind = GetNum(day_entry, "wind_speed");
        day_input.max_wind = kUseFahrenheit ? wind : wind * 3.6;
        day_input.max_temp = GetNum(day_temp, "max");
        alert_days.push_back(day_input);
    }

    // Prefer a real, authority-issued alert (e.g. a national weather
    // service warning) over our own heuristic guess whenever OWM returns
    // one for this location.
    cJSON* alerts = cJSON_GetObjectItem(root, "alerts");
    cJSON* first_alert = (cJSON_IsArray(alerts) && cJSON_GetArraySize(alerts) > 0)
                              ? cJSON_GetArrayItem(alerts, 0) : nullptr;
    if (cJSON_IsObject(first_alert)) {
        std::string event = GetStr(first_alert, "event");
        std::string event_upper = ToUpper(event);
        out->alert.type = ClassifyOfficialAlertIcon(event_upper);
        out->alert.day_label = "";
        out->alert.event_text = event;
    } else {
        out->alert = EvaluateHeuristicAlert(alert_days);
    }

    cJSON_Delete(root);
    return true;
}

// ============================================================
// HTTP client
// ============================================================

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_response_buf &&
            s_response_len + evt->data_len < static_cast<int>(kResponseBufCapacity)) {
            memcpy(s_response_buf + s_response_len, evt->data, evt->data_len);
            s_response_len += evt->data_len;
        } else if (s_response_buf) {
            ESP_LOGW(kTag, "Response exceeds %u byte buffer, truncating", (unsigned)kResponseBufCapacity);
        }
    }
    return ESP_OK;
}

bool HttpGet(const char* url) {
    if (!s_response_buf) {
        ESP_LOGE(kTag, "Response buffer not allocated");
        return false;
    }
    s_response_len = 0;
    s_response_buf[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEventHandler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // HTTPS - use the built-in CA bundle

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
    esp_http_client_cleanup(client);
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status %d (response may still contain an API error message)", status);
        // OpenWeatherMap returns a JSON {"cod": ..., "message": ...} body
        // alongside 4xx statuses - fall through so ParseOneCallJson can log it.
    }
    s_response_buf[s_response_len] = '\0';
    return true;
}

void DoFetch(void* arg) {
    (void)arg;
    if (!s_initialized || s_in_progress) return;
    if (!s_api_key[0]) {
        ESP_LOGW(kTag, "No API key configured (CONFIG_WEATHER_API_KEY) - skipping fetch");
        return;
    }
    if (!s_location[0]) {
        ESP_LOGE(kTag, "No location configured (CONFIG_WEATHER_LOCATION)");
        return;
    }

    // s_location is "lat,lon" - split it for the two separate query params
    // One Call 3.0 requires.
    char location_copy[64];
    strncpy(location_copy, s_location, sizeof(location_copy) - 1);
    location_copy[sizeof(location_copy) - 1] = '\0';
    char* comma = strchr(location_copy, ',');
    if (!comma) {
        ESP_LOGE(kTag, "CONFIG_WEATHER_LOCATION must be \"lat,lon\", got '%s'", s_location);
        return;
    }
    *comma = '\0';
    const char* lat = location_copy;
    const char* lon = comma + 1;

    s_in_progress = true;

    char url[320];
    snprintf(url, sizeof(url),
             "https://api.openweathermap.org/data/3.0/onecall?lat=%s&lon=%s&appid=%s&units=%s&exclude=minutely,hourly",
             lat, lon, s_api_key, kUseFahrenheit ? "imperial" : "metric");
    ESP_LOGI(kTag, "Fetching forecast for '%s'", s_location);

    WeatherData data;
    if (HttpGet(url) && ParseOneCallJson(s_response_buf, &data)) {
        s_last_data = data;
        ESP_LOGI(kTag, "Weather: %s %d%s (hi %d / lo %d), alert=%d/%s",
                 data.condition_text.c_str(), static_cast<int>(data.temp),
                 kUseFahrenheit ? "F" : "C", static_cast<int>(data.temp_max_today),
                 static_cast<int>(data.temp_min_today), static_cast<int>(data.alert.type),
                 data.alert.day_label.c_str());
        if (s_callback) s_callback(data);
    } else {
        ESP_LOGE(kTag, "Failed to fetch or parse weather data");
    }

    s_in_progress = false;
}

void TimerCallback(void* arg) {
    (void)arg;
    ESP_LOGD(kTag, "Hourly weather refresh triggered");
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}

// Dedicated task body: blocks until notified (by the initial fetch trigger
// or the hourly timer), then performs the fetch on this task's own stack.
void FetchTask(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        DoFetch(nullptr);
    }
}

}  // namespace

// ============================================================
// Public API
// ============================================================

WeatherIcon WeatherIconForCode(int code) {
    // Thunderstorm group (200-232) - checked first, the most visually
    // distinctive condition.
    if (code >= 200 && code < 300) return WeatherIcon::Thunder;

    // Snow group (600-622)
    if (code >= 600 && code < 700) return WeatherIcon::Snow;

    // Drizzle (300-321) - light precipitation, distinct from full rain.
    if (code >= 300 && code < 400) return WeatherIcon::Drizzle;

    // Rain group (500-531): split light/moderate from heavy/extreme/freezing.
    if (code == 500 || code == 501 || code == 520 || code == 521 || code == 531) return WeatherIcon::Rain;
    if (code == 502 || code == 503 || code == 504 || code == 511 || code == 522) return WeatherIcon::HeavyRain;

    // Clear sky
    if (code == 800) return WeatherIcon::Sunny;

    // Clouds: few/scattered (<=50% cover) read as "partly cloudy" (keeps the
    // sun visible), broken/overcast (804) is fully "cloudy".
    if (code == 801 || code == 802) return WeatherIcon::PartlyCloudy;
    if (code == 803 || code == 804) return WeatherIcon::Cloudy;

    // Atmosphere group (701-781): mist/smoke/haze/dust/sand/ash get a
    // dedicated "fog" glyph; sand/dust whirls, squalls and tornado still
    // have no dedicated glyph and fall back to Cloudy.
    if (code == 701 || code == 711 || code == 721 || code == 731 || code == 741 || code == 751 || code == 761 ||
        code == 762) {
        return WeatherIcon::Fog;
    }
    if (code >= 700 && code < 800) return WeatherIcon::Cloudy;

    return WeatherIcon::Unknown;
}

void weather_api_init(const char* api_key, const char* location, WeatherCallback callback) {
    if (s_initialized) {
        ESP_LOGW(kTag, "Already initialized");
        return;
    }

    s_response_buf = static_cast<char*>(
        heap_caps_malloc(kResponseBufCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_response_buf) {
        ESP_LOGE(kTag, "Failed to allocate %u byte response buffer from PSRAM",
                 (unsigned)kResponseBufCapacity);
        return;
    }

    if (api_key) strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    if (location) strncpy(s_location, location, sizeof(s_location) - 1);
    s_callback = callback;
    s_initialized = true;

    xTaskCreate(FetchTask, "weather_fetch", kFetchTaskStackSize, nullptr,
                kFetchTaskPriority, &s_fetch_task);

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = TimerCallback;
    timer_args.arg = nullptr;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "weather_refresh";
    timer_args.skip_unhandled_events = true;

    if (esp_timer_create(&timer_args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, 3600LL * 1000000LL);  // hourly
        ESP_LOGI(kTag, "Timer started (1h interval)");
    } else {
        ESP_LOGE(kTag, "Failed to create refresh timer");
    }

    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);

    ESP_LOGI(kTag, "Weather API initialized: location=%s", s_location);
}

bool weather_api_fetch_now() {
    if (!s_initialized || s_in_progress || !s_fetch_task) return false;
    xTaskNotifyGive(s_fetch_task);
    return true;
}

bool weather_api_is_ready() {
    return s_initialized;
}

const WeatherData* weather_api_get_last_data() {
    return &s_last_data;
}

