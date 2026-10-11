#include <cstdio>
#include <cstring>
#include <strings.h>
#include "OnvifEvent.h"
#include "OnvifEventTopics.h"

using namespace std;

namespace mediakit {

static const char *localName(const char *name) {
    auto colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static bool isNamed(const pugi::xml_node &node, const char *local) {
    return node.type() == pugi::node_element && strcasecmp(localName(node.name()), local) == 0;
}

static pugi::xml_node child(const pugi::xml_node &parent, const char *local) {
    for (auto &node : parent.children()) {
        if (isNamed(node, local)) {
            return node;
        }
    }
    return pugi::xml_node();
}

static pugi::xml_attribute attribute(const pugi::xml_node &node, const char *local) {
    for (auto &attr : node.attributes()) {
        if (strcasecmp(localName(attr.name()), local) == 0) {
            return attr;
        }
    }
    return pugi::xml_attribute();
}

static string trim(const string &text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return "";
    }
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

static string childText(const pugi::xml_node &parent, const char *local) {
    return trim(child(parent, local).text().as_string());
}

static string escapeXml(const string &text) {
    string ret;
    ret.reserve(text.size());
    for (auto ch : text) {
        switch (ch) {
            case '&': ret += "&amp;"; break;
            case '<': ret += "&lt;"; break;
            case '>': ret += "&gt;"; break;
            case '"': ret += "&quot;"; break;
            default: ret += ch; break;
        }
    }
    return ret;
}

// ── Topic set ──

static void itemNames(const pugi::xml_node &desc, const char *local, vector<string> &out) {
    auto items = child(desc, local);
    for (auto &node : items.children()) {
        if (isNamed(node, "SimpleItemDescription")) {
            auto name = node.attribute("Name").as_string();
            if (*name) {
                out.emplace_back(name);
            }
        }
    }
}

static void collectTopics(const pugi::xml_node &node, const string &parent_path, size_t max_topics,
                          vector<OnvifTopicDescription> &out, bool &truncated) {
    if (node.type() != pugi::node_element || isNamed(node, "MessageDescription")) {
        return;
    }
    auto path = parent_path.empty() ? string(node.name()) : parent_path + "/" + node.name();
    auto desc = child(node, "MessageDescription");
    auto topic_attr = attribute(node, "topic");
    if (desc || (topic_attr && topic_attr.as_bool())) {
        if (out.size() >= max_topics) {
            truncated = true;
        } else {
            OnvifTopicDescription topic;
            topic.topic = path;
            if (desc) {
                topic.isProperty = desc.attribute("IsProperty").as_bool();
                itemNames(desc, "Source", topic.sourceItems);
                itemNames(desc, "Data", topic.dataItems);
            }
            out.emplace_back(std::move(topic));
        }
    }
    for (auto &sub : node.children()) {
        collectTopics(sub, path, max_topics, out, truncated);
    }
}

void parseOnvifTopicSet(const pugi::xml_node &topic_set, size_t max_topics, vector<OnvifTopicDescription> &out,
                        bool *truncated) {
    bool more = false;
    for (auto &node : topic_set.children()) {
        collectTopics(node, "", max_topics, out, more);
    }
    if (truncated) {
        *truncated = more;
    }
}

vector<OnvifTopicDescription> parseOnvifTopicSet(const vector<string> &fragments, size_t max_topics, bool *truncated) {
    vector<OnvifTopicDescription> ret;
    bool more = false;
    for (auto &fragment : fragments) {
        pugi::xml_document doc;
        if (!doc.load_buffer(fragment.data(), fragment.size())) {
            continue;
        }
        for (auto &node : doc.children()) {
            collectTopics(node, "", max_topics, ret, more);
        }
    }
    if (truncated) {
        *truncated = more;
    }
    return ret;
}

// ── Notifications ──

static void simpleItems(const pugi::xml_node &parent, OnvifSimpleItems &out) {
    for (auto &node : parent.children()) {
        if (isNamed(node, "SimpleItem")) {
            out.emplace_back(node.attribute("Name").as_string(), node.attribute("Value").as_string());
        }
    }
}

string OnvifEventMessage::stateValue() const {
    if (stateItem.empty()) {
        return "";
    }
    for (auto &item : data) {
        if (strcasecmp(item.first.data(), stateItem.data()) == 0) {
            return item.second;
        }
    }
    return "";
}

string OnvifEventMessage::stateKey() const {
    string ret = eventType.empty() ? OnvifEventTopics::stripPrefixes(topic) : eventType;
    for (auto &item : source) {
        ret += '|';
        ret += item.first;
        ret += '=';
        ret += item.second;
    }
    return ret;
}

bool parseOnvifPullMessages(const pugi::xml_node &body, OnvifPullResult &out) {
    auto response = child(body, "PullMessagesResponse");
    if (!response) {
        return false;
    }
    out.currentTime = parseXsDateTime(childText(response, "CurrentTime"));
    out.terminationTime = parseXsDateTime(childText(response, "TerminationTime"));
    for (auto &node : response.children()) {
        if (!isNamed(node, "NotificationMessage")) {
            continue;
        }
        OnvifEventMessage msg;
        msg.topic = childText(node, "Topic");
        if (msg.topic.empty()) {
            continue;
        }
        if (auto rule = OnvifEventTopics::classify(msg.topic)) {
            msg.eventType = rule->eventType;
            msg.stateItem = rule->stateItem;
        }
        auto message = child(child(node, "Message"), "Message");
        if (message) {
            msg.utcTime = message.attribute("UtcTime").as_string();
            msg.cameraUtc = parseXsDateTime(msg.utcTime);
            msg.propertyOp = message.attribute("PropertyOperation").as_string();
            simpleItems(child(message, "Source"), msg.source);
            simpleItems(child(message, "Data"), msg.data);
        }
        out.messages.emplace_back(std::move(msg));
    }
    return true;
}

// The element with the namespace declarations of its ancestors, so it can be pasted into another document.
static string serializeStandalone(const pugi::xml_node &node) {
    pugi::xml_document doc;
    auto copy = doc.append_copy(node);
    for (auto parent = node.parent(); parent; parent = parent.parent()) {
        for (auto &attr : parent.attributes()) {
            if (strncmp(attr.name(), "xmlns", 5) == 0 && !copy.attribute(attr.name())) {
                copy.append_attribute(attr.name()) = attr.value();
            }
        }
    }
    struct Writer : pugi::xml_writer {
        string result;
        void write(const void *data, size_t size) override { result.append(static_cast<const char *>(data), size); }
    } writer;
    doc.save(writer, "", pugi::format_raw | pugi::format_no_declaration);
    return writer.result;
}

bool parseOnvifCreatePullPoint(const pugi::xml_node &body, OnvifSubscriptionRef &out) {
    auto response = child(body, "CreatePullPointSubscriptionResponse");
    auto ref = child(response, "SubscriptionReference");
    out.address = childText(ref, "Address");
    if (out.address.empty()) {
        return false;
    }
    out.referenceParameters.clear();
    for (auto &node : child(ref, "ReferenceParameters").children()) {
        if (node.type() == pugi::node_element) {
            out.referenceParameters += serializeStandalone(node);
        }
    }
    out.currentTime = parseXsDateTime(childText(response, "CurrentTime"));
    out.terminationTime = parseXsDateTime(childText(response, "TerminationTime"));
    return true;
}

bool parseOnvifRenew(const pugi::xml_node &body, time_t &current_time, time_t &termination_time) {
    auto response = child(body, "RenewResponse");
    if (!response) {
        return false;
    }
    current_time = parseXsDateTime(childText(response, "CurrentTime"));
    termination_time = parseXsDateTime(childText(response, "TerminationTime"));
    return true;
}

time_t parseXsDateTime(const string &text_in) {
    auto text = trim(text_in);
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    int consumed = 0;
    if (sscanf(text.data(), "%d-%d-%dT%d:%d:%d%n", &tm_val.tm_year, &tm_val.tm_mon, &tm_val.tm_mday,
               &tm_val.tm_hour, &tm_val.tm_min, &tm_val.tm_sec, &consumed) != 6) {
        return 0;
    }
    if (tm_val.tm_year < 1970 || tm_val.tm_mon < 1 || tm_val.tm_mon > 12 || tm_val.tm_mday < 1 || tm_val.tm_mday > 31) {
        return 0;
    }
    tm_val.tm_year -= 1900;
    tm_val.tm_mon -= 1;
    auto ret = timegm(&tm_val);
    const char *rest = text.data() + consumed;
    if (*rest == '.') {
        ++rest;
        while (*rest >= '0' && *rest <= '9') {
            ++rest;
        }
    }
    if (*rest == '+' || *rest == '-') {
        int hh = 0, mm = 0;
        if (sscanf(rest + 1, "%d:%d", &hh, &mm) < 1) {
            return 0;
        }
        auto offset = hh * 3600 + mm * 60;
        ret += *rest == '+' ? -offset : offset;
    }
    return ret;
}

// ── Requests ──

const char *OnvifEventRequest::kCreatePullPointAction =
    "http://www.onvif.org/ver10/events/wsdl/EventPortType/CreatePullPointSubscriptionRequest";
const char *OnvifEventRequest::kPullMessagesAction =
    "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest";
const char *OnvifEventRequest::kRenewAction = "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/RenewRequest";
const char *OnvifEventRequest::kUnsubscribeAction =
    "http://docs.oasis-open.org/wsn/bw-2/SubscriptionManager/UnsubscribeRequest";

static string duration(int sec) {
    return "PT" + to_string(sec) + "S";
}

string OnvifEventRequest::createPullPoint(const vector<string> &topics, int termination_sec) {
    string ret = R"(<CreatePullPointSubscription xmlns="http://www.onvif.org/ver10/events/wsdl">)";
    if (!topics.empty()) {
        string expr;
        for (auto &topic : topics) {
            if (!expr.empty()) {
                expr += '|';
            }
            expr += topic;
        }
        ret += R"(<Filter><wsnt:TopicExpression xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2")"
               R"( xmlns:tns1="http://www.onvif.org/ver10/topics")"
               R"( Dialect="http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet">)";
        ret += escapeXml(expr);
        ret += "</wsnt:TopicExpression></Filter>";
    }
    ret += "<InitialTerminationTime>" + duration(termination_sec) + "</InitialTerminationTime>";
    ret += "</CreatePullPointSubscription>";
    return ret;
}

string OnvifEventRequest::pullMessages(int timeout_sec, int message_limit) {
    return R"(<PullMessages xmlns="http://www.onvif.org/ver10/events/wsdl"><Timeout>)" + duration(timeout_sec) +
           "</Timeout><MessageLimit>" + to_string(message_limit) + "</MessageLimit></PullMessages>";
}

string OnvifEventRequest::renew(int termination_sec) {
    return R"(<Renew xmlns="http://docs.oasis-open.org/wsn/b-2"><TerminationTime>)" + duration(termination_sec) +
           "</TerminationTime></Renew>";
}

string OnvifEventRequest::unsubscribe() {
    return R"(<Unsubscribe xmlns="http://docs.oasis-open.org/wsn/b-2"/>)";
}

string OnvifEventRequest::addressingHeader(const string &action, const string &to, const string &reference_parameters) {
    // The envelope of SoapUtil::createSoapRequest declares the "a" (WS-Addressing 1.0) and "s" prefixes.
    string ret = R"(<a:Action s:mustUnderstand="1">)" + escapeXml(action) + "</a:Action>";
    if (!to.empty()) {
        ret += R"(<a:To s:mustUnderstand="1">)" + escapeXml(to) + "</a:To>";
    }
    ret += reference_parameters;
    return ret;
}

} // namespace mediakit
