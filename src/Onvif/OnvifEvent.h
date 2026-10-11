#ifndef S3MEDIAKIT_ONVIFEVENT_H
#define S3MEDIAKIT_ONVIFEVENT_H

#include <ctime>
#include <string>
#include <utility>
#include <vector>
#include "pugixml.hpp"

namespace mediakit {

// One topic of a camera topic set (GetEventProperties).
struct OnvifTopicDescription {
    std::string topic;                    // "tns1:RuleEngine/CellMotionDetector/Motion", as the camera wrote it
    bool isProperty = false;              // MessageDescription@IsProperty
    std::vector<std::string> sourceItems; // SimpleItemDescription names of Source
    std::vector<std::string> dataItems;   // SimpleItemDescription names of Data
};

/**
 * Topics of a wstop:TopicSet. `fragments` are its child elements as XML text (gSOAP keeps xsd:any
 * that way, without the namespace declarations of the envelope: prefixes are kept as plain names).
 * A node is a topic when it has wstop:topic="true" or a MessageDescription. At most `max_topics`
 * are returned; `truncated` (optional) tells whether more were found.
 */
std::vector<OnvifTopicDescription> parseOnvifTopicSet(const std::vector<std::string> &fragments, size_t max_topics,
                                                      bool *truncated = nullptr);

// Same, from the wstop:TopicSet element itself.
void parseOnvifTopicSet(const pugi::xml_node &topic_set, size_t max_topics, std::vector<OnvifTopicDescription> &out,
                        bool *truncated = nullptr);

using OnvifSimpleItems = std::vector<std::pair<std::string, std::string>>;

// One wsnt:NotificationMessage.
struct OnvifEventMessage {
    std::string topic;      // as sent
    std::string eventType;  // OnvifEventTopics::classify; "" when the media server does not support it
    std::string stateItem;  // Data item holding the state, from the topic rule
    std::string propertyOp; // "Initialized", "Changed", "Deleted", "" for a non-property event
    std::string utcTime;    // Message@UtcTime as sent
    time_t cameraUtc = 0;   // utcTime parsed, 0 when missing or invalid
    OnvifSimpleItems source;
    OnvifSimpleItems data;

    // Value of the state item, "" for a non-state event.
    std::string stateValue() const;
    // Event type + source items: identifies one state (motion of one video source...).
    std::string stateKey() const;
};

struct OnvifPullResult {
    time_t currentTime = 0;     // camera clock
    time_t terminationTime = 0; // camera clock
    std::vector<OnvifEventMessage> messages;
};

// `body` is the SOAP Body. False when it holds no PullMessagesResponse.
bool parseOnvifPullMessages(const pugi::xml_node &body, OnvifPullResult &out);

struct OnvifSubscriptionRef {
    std::string address;             // SubscriptionReference/Address
    std::string referenceParameters; // its ReferenceParameters children as XML, to repeat in each header
    time_t currentTime = 0;
    time_t terminationTime = 0;
};

// `body` is the SOAP Body. False when it holds no subscription address.
bool parseOnvifCreatePullPoint(const pugi::xml_node &body, OnvifSubscriptionRef &out);

// `body` is the SOAP Body of a RenewResponse. False when it is not one.
bool parseOnvifRenew(const pugi::xml_node &body, time_t &current_time, time_t &termination_time);

// xs:dateTime ("2026-10-11T03:12:05Z", fractions, "+07:00" offsets; no zone = UTC) to UTC; 0 when invalid.
time_t parseXsDateTime(const std::string &text);

// Bodies, actions and WS-Addressing header of the event requests.
class OnvifEventRequest {
public:
    static const char *kCreatePullPointAction;
    static const char *kPullMessagesAction;
    static const char *kRenewAction;
    static const char *kUnsubscribeAction;

    // `topics` empty: no filter.
    static std::string createPullPoint(const std::vector<std::string> &topics, int termination_sec);
    static std::string pullMessages(int timeout_sec, int message_limit);
    static std::string renew(int termination_sec);
    static std::string unsubscribe();
    // wsa:Action, wsa:To and the reference parameters of the subscription.
    static std::string addressingHeader(const std::string &action, const std::string &to,
                                        const std::string &reference_parameters);
};

} // namespace mediakit

#endif // S3MEDIAKIT_ONVIFEVENT_H
