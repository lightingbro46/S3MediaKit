#ifndef CONTROL_ONVIFEVENTKAFKA_H
#define CONTROL_ONVIFEVENTKAFKA_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include "Common/EventRecord.h"
#include "Onvif/OnvifEvent.h"

namespace managerkit {

/**
 * ONVIF event -> event bus record (device-event schema v1) and the decision whether to send it.
 * Pure functions: the caller (OnvifEventHub) owns the state and the locking.
 */
namespace OnvifEventKafka {

constexpr int kSchemaVersion = 1;

struct Context {
    std::string mediaServerId;
    std::string projectId;
    std::string deviceId;
    std::string bootId;       // random per process run, part of eventId
    uint64_t seq = 0;         // increasing within bootId
    int64_t serverTimeMs = 0; // when the media server received the event
    int64_t lagMs = 0;        // server time - camera time, camera clock offset applied
};

// State item value -> 1 (active), 0 (inactive), -1 (unknown). Case-insensitive, trimmed.
int normalizeActive(const std::string &value);

// `resync`: a state reported again after resubscribing that differs from the value sent last;
// sent as propertyOp "Changed" (an "Initialized" event is never sent as such).
EventRecord toRecord(const mediakit::OnvifEventMessage &event, const Context &ctx, bool resync);

// Value last accepted by the event bus for each state key (OnvifEventMessage::stateKey) of one camera.
using SentStates = std::unordered_map<std::string, std::string>;

enum class Decision {
    Drop,       // "Initialized" equal to the value sent, or nothing sent yet for that state
    Send,       // "Changed", "Deleted" or a pulse event
    SendResync, // "Initialized" with a value other than the one sent: send as Changed + resync
};

Decision decide(const mediakit::OnvifEventMessage &event, const SentStates &sent);

// Records what the consumer now holds once the event bus accepted the record: "Deleted" erases the
// key, a state event stores its value, a pulse event changes nothing. A new key is not stored when
// the camera already has `max_keys` keys (0 = no limit).
void remember(const mediakit::OnvifEventMessage &event, SentStates &sent, size_t max_keys);

} // namespace OnvifEventKafka
} // namespace managerkit

#endif // CONTROL_ONVIFEVENTKAFKA_H
