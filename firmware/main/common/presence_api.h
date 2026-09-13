/**
 * @file presence_api.h
 * @brief HTTP client for the office-door busy-light presence bridge.
 *
 * Mirrors weather_api.h's shape: esp_http_client GET + cJSON parse +
 * periodic esp_timer refresh + callback delivery. See server/presence_bridge.py
 * for the expected JSON contract.
 *
 * Usage:
 * 1. presence_api_init("http://bridge.local:8080/presence", callback)
 * 2. Callback receives PresenceStatus on every successful fetch
 * 3. Timer triggers 5-minute auto-refresh
 */

#ifndef PRESENCE_API_H
#define PRESENCE_API_H

#include "presence_types.h"

#include <functional>

using PresenceCallback = std::function<void(const PresenceStatus&)>;

/**
 * @brief Initialize the presence API client and start the 5-minute refresh timer.
 *
 * @param endpoint Full URL of the presence bridge endpoint
 * @param callback Function called on every successful fetch
 */
void presence_api_init(const char* endpoint, PresenceCallback callback);

/**
 * @brief Trigger a manual presence fetch.
 *
 * @return true if request started, false if already in progress or not initialized
 */
bool presence_api_fetch_now();

/**
 * @brief Change the bridge endpoint URL and persist it to NVS.
 */
void presence_api_set_endpoint(const char* endpoint);

/**
 * @brief Get the current bridge endpoint URL.
 */
const char* presence_api_get_endpoint();

/**
 * @brief Check if the API client is initialized.
 */
bool presence_api_is_ready();

/**
 * @brief Get the last fetched presence status (last-known-good on fetch failure).
 */
const PresenceStatus* presence_api_get_last_data();

#endif  // PRESENCE_API_H
