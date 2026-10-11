#ifndef COMMON_EVENTRECORD_H
#define COMMON_EVENTRECORD_H

#include <string>
#include <utility>
#include <vector>

namespace managerkit {

// One message for the event bus: Kafka key, headers and value. The topic is not part of it: the
// record is sent (or replayed from the spool) to the topic configured at that time.
struct EventRecord {
    std::string key;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string value;
};

} // namespace managerkit

#endif // COMMON_EVENTRECORD_H
