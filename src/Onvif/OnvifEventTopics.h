#ifndef S3MEDIAKIT_ONVIFEVENTTOPICS_H
#define S3MEDIAKIT_ONVIFEVENTTOPICS_H

#include <string>
#include <vector>

namespace mediakit {

/**
 * One ONVIF event topic the media server understands. Supporting a new topic means adding a row to
 * OnvifEventTopics::rules(): the camera capabilities, the subscription filter and the event parser
 * all follow the table.
 */
struct OnvifEventTopicRule {
    const char *eventType; // normalized type reported to the API: "MOTION", "TAMPER"...
    const char *path;      // topic path without namespace prefixes, compared case-insensitively
    const char *stateItem; // Data SimpleItem carrying the state ("" for a non-state event)
};

class OnvifEventTopics {
public:
    // The normalized topics, in the order event types are reported.
    static const std::vector<OnvifEventTopicRule> &rules();

    // "tns1:RuleEngine/tnsx:CellMotionDetector/Motion" -> "RuleEngine/CellMotionDetector/Motion".
    // Prefixes are dropped because cameras name the ONVIF namespace differently (tns1, tt, ns2...).
    static std::string stripPrefixes(const std::string &topic);

    // The rule of `topic` (as the camera wrote it), null when the media server does not support it.
    static const OnvifEventTopicRule *classify(const std::string &topic);

    // Topic expression subscribing to `rule`: the standard ONVIF namespace prefix "tns1:" + path,
    // whatever prefix the camera used in its topic set.
    static std::string concreteTopic(const OnvifEventTopicRule &rule);

    // Position of `event_type` in rules(), for a stable order; rules().size() when unknown.
    static size_t eventTypeOrder(const std::string &event_type);
};

} // namespace mediakit

#endif // S3MEDIAKIT_ONVIFEVENTTOPICS_H
