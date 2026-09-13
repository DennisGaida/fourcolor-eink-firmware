/**
 * @file presence_types.h
 * @brief Plain data model for the busy-light presence page.
 *
 * No ESP-IDF includes here on purpose: this struct is also consumed by the
 * SDL host preview harness, which does not link ESP-IDF.
 */

#ifndef PRESENCE_TYPES_H
#define PRESENCE_TYPES_H

#include <stdint.h>
#include <string>
#include <vector>

struct PresenceEvent {
    int32_t start_minutes = 0;  // Minutes since local midnight
    int32_t end_minutes = 0;
    std::string title;
};

struct PresenceStatus {
    std::vector<PresenceEvent> events;  // Today's events, sorted by start
    bool in_call = false;
    bool webcam_active = false;
    int64_t last_updated_unix = 0;
    bool valid = false;  // false until first successful fetch
};

#endif  // PRESENCE_TYPES_H
