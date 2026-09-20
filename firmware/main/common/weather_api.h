/**
 * @file weather_api.h
 * @brief OpenWeatherMap One Call 3.0 client for ESP32
 *
 * Fetches current conditions + an 8-day daily forecast + any active
 * government weather alerts in a single HTTP GET to OpenWeatherMap's One
 * Call 3.0 endpoint. Uses esp_http_client.
 *
 * API docs: https://openweathermap.org/api/one-call-3
 *
 * Chosen over WeatherAPI.com (the original provider) because its response
 * is roughly 10x smaller (~5KB vs ~50KB+ for a comparable forecast window,
 * friendlier to the device's limited RAM) and it can surface real
 * authority-issued alerts (e.g. national weather service warnings) instead
 * of a purely heuristic guess.
 *
 * Usage:
 * 1. weather_api_init(api_key, location, callback) - location must be
 *    "lat,lon" (decimal degrees) - One Call 3.0 only accepts coordinates,
 *    not a free-form city/query string.
 * 2. Callback receives WeatherData on each successful fetch.
 * 3. A timer triggers an hourly auto-refresh; weather_api_fetch_now() can
 *    trigger one on demand (e.g. a manual refresh button).
 */

#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <functional>
#include <vector>

// ============================================================
// Weather data model
// ============================================================

/**
 * @brief Icon bucket for rendering. Still far smaller than OpenWeatherMap's
 * ~50 condition IDs, but expanded beyond the original 5 glyphs (see
 * weather_icons.h / weather_icons_extra_48.c) to distinguish a few visually
 * distinct cases that were previously collapsed into Cloudy/Rain.
 */
enum class WeatherIcon {
    Sunny,        // clear/sunny
    PartlyCloudy, // few/scattered clouds (<=50% cover)
    Cloudy,       // broken/overcast clouds, tornado, squalls, etc. (fallback)
    Fog,          // mist/fog/haze/smoke/dust/sand/ash
    Drizzle,      // light drizzle
    Rain,         // moderate rain/showers
    HeavyRain,    // heavy/very heavy/extreme/freezing rain, heavy showers
    Snow,         // any snow/sleet/ice
    Thunder,      // thunderstorms
    Unknown,      // fallback
};

/**
 * @brief Map an OpenWeatherMap condition ID to our reduced icon set
 * (see https://openweathermap.org/weather-conditions)
 */
WeatherIcon WeatherIconForCode(int condition_code);

/**
 * @brief Bottom alert-bar condition, evaluated across today + the 3-day
 * forecast (whichever triggers first, in this priority order: rain > snow
 * > wind > heat), unless OpenWeatherMap has an active real government
 * alert for the location - that always takes priority (kOfficial).
 * day_label is "TODAY" or an upper-case weekday name.
 */
enum class WeatherAlertType {
    kNone,
    kRain,
    kSnow,
    kWind,
    kHeat,
    kOfficial,  // real alert from OWM's alerts[] (e.g. a national weather service warning)
};

struct WeatherAlert {
    WeatherAlertType type = WeatherAlertType::kNone;
    std::string day_label;  // "TODAY" / "MONDAY" / ...
    std::string event_text; // real alert headline (kOfficial only), e.g. "Gale Force Gusts"
};

struct WeatherForecastDay {
    std::string weekday_label;   // "SUN", "MON", ... (upper-case, 3-letter)
    std::string condition_text;  // e.g. "Partly cloudy"
    int32_t condition_code = 0;
    int32_t temp_max = 0;   // rounded, in the configured unit (C or F)
    int32_t temp_min = 0;
};

struct WeatherData {
    std::string location_name;   // e.g. "Hamburg"
    std::string date_label;      // e.g. "SATURDAY / SEP 20"

    int32_t temp = 0;            // current temp, rounded, configured unit
    std::string condition_text;  // e.g. "Partly cloudy"
    int32_t condition_code = 0;
    int32_t temp_max_today = 0;
    int32_t temp_min_today = 0;

    std::vector<WeatherForecastDay> forecast;  // next 3 days (today excluded)
    WeatherAlert alert;
};

// ============================================================
// API interface
// ============================================================

using WeatherCallback = std::function<void(const WeatherData&)>;

/**
 * @brief Initialize the OpenWeatherMap One Call 3.0 client
 *
 * Sets up the hourly auto-refresh timer and fetches immediately.
 *
 * @param api_key OpenWeatherMap API key (CONFIG_WEATHER_API_KEY)
 * @param location "lat,lon" decimal-degree coordinates (CONFIG_WEATHER_LOCATION)
 * @param callback Called with fresh WeatherData on each successful fetch
 */
void weather_api_init(const char* api_key, const char* location, WeatherCallback callback);

/**
 * @brief Trigger a manual weather data fetch
 * @return true if a request was started, false if one was already in progress
 */
bool weather_api_fetch_now();

/**
 * @brief Check if the API client has been initialized
 */
bool weather_api_is_ready();

/**
 * @brief Get the last successfully fetched weather data
 */
const WeatherData* weather_api_get_last_data();

#endif  // WEATHER_API_H
