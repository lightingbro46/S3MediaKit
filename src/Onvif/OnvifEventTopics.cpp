#include <strings.h>
#include "OnvifEventTopics.h"

using namespace std;

namespace mediakit {

const vector<OnvifEventTopicRule> &OnvifEventTopics::rules() {
    static const vector<OnvifEventTopicRule> s_rules = {
        { "MOTION",        "RuleEngine/CellMotionDetector/Motion",        "IsMotion" },
        { "MOTION",        "VideoSource/MotionAlarm",                     "State" },
        { "TAMPER",        "RuleEngine/TamperDetector/Tamper",            "IsTamper" },
        { "TAMPER",        "VideoSource/GlobalSceneChange/ImagingService", "State" },
        { "VIDEO_LOSS",    "VideoSource/SignalLoss",                      "State" },
        { "DIGITAL_INPUT", "Device/Trigger/DigitalInput",                 "LogicalState" },
        { "RELAY",         "Device/Trigger/Relay",                        "LogicalState" },
        { "LINE_CROSSING", "RuleEngine/LineDetector/Crossed",             "" },
        { "INTRUSION",     "RuleEngine/FieldDetector/ObjectsInside",      "IsInside" },
    };
    return s_rules;
}

string OnvifEventTopics::stripPrefixes(const string &topic) {
    string ret;
    ret.reserve(topic.size());
    size_t start = 0;
    while (start <= topic.size()) {
        auto end = topic.find('/', start);
        if (end == string::npos) {
            end = topic.size();
        }
        auto segment = topic.substr(start, end - start);
        auto colon = segment.rfind(':');
        if (colon != string::npos) {
            segment = segment.substr(colon + 1);
        }
        // Topic text may carry whitespace around it (pretty printed XML).
        auto first = segment.find_first_not_of(" \t\r\n");
        auto last = segment.find_last_not_of(" \t\r\n");
        segment = first == string::npos ? string() : segment.substr(first, last - first + 1);
        if (!segment.empty()) {
            if (!ret.empty()) {
                ret += '/';
            }
            ret += segment;
        }
        start = end + 1;
    }
    return ret;
}

const OnvifEventTopicRule *OnvifEventTopics::classify(const string &topic) {
    auto path = stripPrefixes(topic);
    for (auto &rule : rules()) {
        if (strcasecmp(path.data(), rule.path) == 0) {
            return &rule;
        }
    }
    return nullptr;
}

string OnvifEventTopics::concreteTopic(const OnvifEventTopicRule &rule) {
    return string("tns1:") + rule.path;
}

size_t OnvifEventTopics::eventTypeOrder(const string &event_type) {
    auto &table = rules();
    for (size_t i = 0; i < table.size(); ++i) {
        if (event_type == table[i].eventType) {
            return i;
        }
    }
    return table.size();
}

} // namespace mediakit
