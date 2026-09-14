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

// How much this event matters to the person standing at the door: drives the
// fill style on the detail-face day grid (outline/yellow-rule/solid-red) and
// the human sub-line on the default face ("come in" / "knock" / "quiet").
enum class PresenceEventTier {
    kInternal,    // colleague meeting — outline, "come in"
    kLeadership,  // meeting with leadership — yellow fill + heavy rule, "knock"
    kCustomer,    // external/customer call — solid red, "quiet please"
};

struct PresenceEvent {
    int32_t start_minutes = 0;  // Minutes since local midnight
    int32_t end_minutes = 0;
    std::string title;
    PresenceEventTier tier = PresenceEventTier::kInternal;
};

struct PresenceStatus {
    std::vector<PresenceEvent> events;  // Today's events, sorted by start
    bool in_call = false;
    bool webcam_active = false;
    bool presenting = false;  // screen-sharing / do-not-disturb override
    int64_t last_updated_unix = 0;
    bool valid = false;  // false until first successful fetch
};

#endif  // PRESENCE_TYPES_H
