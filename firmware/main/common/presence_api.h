/**
 * @file presence_api.h
 * @brief HTTP client for the busy-light presence contract.
 *
 * Mirrors weather_api.h's shape: esp_http_client GET + cJSON parse +
 * periodic esp_timer refresh + callback delivery. The contract is split
 * into two independently-polled resources because they change at very
 * different rates — see server/mock_presence_server.py for the JSON shapes:
 *
 *   GET <endpoint>/calendar/today  — today's events, polled every 20 min
 *   GET <endpoint>/live            — isPresenting/isInCall/isWebcamActive,
 *                                     polled every 90s
 *
 * Both are merged into a single PresenceStatus, so callers don't need to
 * know about the split. On fetch failure or a field missing from a
 * response, the previous value for that field is kept (last-known-good),
 * never blanked.
 *
 * Usage:
 * 1. presence_api_init("http://bridge-host:8080", callback)
 * 2. Callback receives PresenceStatus on every successful fetch of either resource
 */

#ifndef PRESENCE_API_H
#define PRESENCE_API_H

#include "presence_types.h"

#include <functional>

using PresenceCallback = std::function<void(const PresenceStatus&)>;

/**
 * @brief Initialize the presence API client and start the refresh timers.
 *
 * @param endpoint Base URL of the presence server, no trailing slash or path
 *                 (e.g. "http://192.168.178.37:8080")
 * @param callback Function called on every successful fetch
 */
void presence_api_init(const char* endpoint, PresenceCallback callback);

/**
 * @brief Trigger a manual fetch of both resources.
 *
 * @return true if requests started, false if already in progress or not initialized
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
