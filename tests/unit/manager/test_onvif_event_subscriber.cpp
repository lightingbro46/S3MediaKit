#include <gtest/gtest.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include "Util/NoticeCenter.h"
#include "Util/mini.h"
#include "Poller/EventPoller.h"
#include "Common/Parser.h"
#include "Common/config.h"
#include "Control/OnvifDeviceControl.h"
#include "Control/OnvifEventSubscriber.h"

using namespace managerkit;
using namespace toolkit;
using namespace mediakit;

namespace {

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

struct Request {
    std::string url;
    std::string action;
    std::string envelope;
    OnvifEventTransport::Callback cb;
};

class FakeTransport : public OnvifEventTransport {
public:
    void send(const std::string &url, const std::string &action, const std::string &envelope, float,
              const std::string &, const std::string &, const HttpAuthSession::Ptr &, const Callback &cb) override {
        std::lock_guard<std::mutex> lck(_mtx);
        _requests.push_back(Request{ url, action, envelope, cb });
    }
    // Waits for the next request; empty action when none came.
    Request next(int timeout_ms = 3000) {
        Request ret;
        waitFor([&]() {
            std::lock_guard<std::mutex> lck(_mtx);
            if (_requests.empty()) return false;
            ret = _requests.front();
            _requests.pop_front();
            return true;
        }, timeout_ms);
        return ret;
    }

private:
    std::mutex _mtx;
    std::deque<Request> _requests;
};

std::string envelope(const std::string &body) {
    return R"(<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>)" + body + "</s:Body></s:Envelope>";
}

void respond(const Request &req, const std::string &body_xml, int status = 200, const std::string &fault = "") {
    mediakit::Parser parser;
    std::string head = "HTTP/1.1 " + std::to_string(status) + " X\r\n\r\n";
    parser.parse(head.data(), head.size());
    SoapObject root;
    if (status == 200) {
        root.load(body_xml.data(), body_xml.size());
        req.cb(root["Envelope/Body"], SoapErr(req.url, req.action, SockException(), parser));
    } else {
        req.cb(SoapObject(), SoapErr(req.url, req.action, SockException(), parser, fault));
    }
}

void respondNetworkError(const Request &req) {
    req.cb(SoapObject(), SoapErr(req.url, req.action, SockException(Err_timeout, "timeout"), mediakit::Parser()));
}

std::string subscription(int lifetime_sec) {
    return envelope(R"(<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>)"
                    R"(<wsa5:Address>http://cam/onvif/sub/1</wsa5:Address>)"
                    R"(<wsa5:ReferenceParameters><x:Id xmlns:x="urn:x">7</x:Id></wsa5:ReferenceParameters>)"
                    R"(</tev:SubscriptionReference><wsnt:CurrentTime>2026-10-11T00:00:00Z</wsnt:CurrentTime>)"
                    R"(<wsnt:TerminationTime>2026-10-11T00:)" +
                    std::string(lifetime_sec >= 600 ? "10:00" : "00:" + std::string(lifetime_sec < 10 ? "0" : "") +
                                                                std::to_string(lifetime_sec)) +
                    R"(Z</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>)");
}

std::string motion(const std::string &value, const std::string &op = "Changed", const std::string &topic =
                       "tns1:RuleEngine/CellMotionDetector/Motion") {
    return R"(<wsnt:NotificationMessage><wsnt:Topic>)" + topic + R"(</wsnt:Topic><wsnt:Message>)"
           R"(<tt:Message UtcTime="2026-10-11T00:00:01Z" PropertyOperation=")" + op + R"(">)"
           R"(<tt:Source><tt:SimpleItem Name="VideoSourceConfigurationToken" Value="VSC"/></tt:Source>)"
           R"(<tt:Data><tt:SimpleItem Name="IsMotion" Value=")" + value + R"("/></tt:Data></tt:Message>)"
           R"(</wsnt:Message></wsnt:NotificationMessage>)";
}

// `termination`: TerminationTime of the subscription the camera reports, mm:ss after 00:00.
std::string pulled(const std::string &messages, const std::string &termination = "10:00") {
    return envelope(R"(<tev:PullMessagesResponse><tev:CurrentTime>2026-10-11T00:00:02Z</tev:CurrentTime>)"
                    R"(<tev:TerminationTime>2026-10-11T00:)" + termination + R"(Z</tev:TerminationTime>)" + messages +
                    "</tev:PullMessagesResponse>");
}

class OnvifEventSubscriberTest : public ::testing::Test {
protected:
    void SetUp() override {
        mINI::Instance()[OnvifEventCfg::kStartJitterMs] = 0;
        mINI::Instance()[OnvifEventCfg::kEventEnable] = 1;
        mINI::Instance()[OnvifEventCfg::kSubscribeFilter] = 1;
        NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
        _poller = EventPollerPool::Instance().getPoller();
        _transport = std::make_shared<FakeTransport>();
        _device_id = std::string("cam-ev-") + ::testing::UnitTest::GetInstance()->current_test_info()->name();
        _sub = OnvifEventSubscriber::create(_poller, _device_id, _transport);
        NoticeCenter::Instance().addListener(this, kBroadcastOnvifEvent, [this](BroadcastOnvifEventArgs) {
            if (device_id != _device_id) return;
            std::lock_guard<std::mutex> lck(_mtx);
            _events.push_back(event.eventType + ":" + event.propertyOp + ":" + event.stateValue());
        });
    }

    void TearDown() override {
        NoticeCenter::Instance().delListener(this, kBroadcastOnvifEvent);
        auto sub = _sub;
        _poller->sync([sub]() { sub->clearEndpoint(); });
        _sub.reset();
    }

    OnvifEventEndpoint endpoint() const {
        OnvifEventEndpoint ep;
        ep.address = "10.0.0.5";
        ep.eventsUrl = "http://10.0.0.5/onvif/events";
        ep.username = "admin";
        ep.password = "pw";
        ep.filterTopics = { "tns1:RuleEngine/CellMotionDetector/Motion" };
        return ep;
    }

    void start() {
        auto sub = _sub;
        auto ep = endpoint();
        _poller->sync([sub, ep]() { sub->setEndpoint(ep); });
    }

    OnvifEventSubscriber::State state() const { return _sub->state(); }

    std::vector<std::string> events() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _events;
    }

    // Subscribes and returns the first pull.
    Request subscribed(int lifetime_sec = 600) {
        auto create = _transport->next();
        EXPECT_EQ(OnvifEventRequest::kCreatePullPointAction, create.action);
        respond(create, subscription(lifetime_sec));
        auto pull = _transport->next();
        EXPECT_EQ(OnvifEventRequest::kPullMessagesAction, pull.action);
        return pull;
    }

    EventPoller::Ptr _poller;
    std::shared_ptr<FakeTransport> _transport;
    std::string _device_id;
    OnvifEventSubscriber::Ptr _sub;
    std::mutex _mtx;
    std::vector<std::string> _events;
};

} // namespace

TEST_F(OnvifEventSubscriberTest, SubscribesWithFilterAndDropsRepeatedStates) {
    start();
    auto create = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kCreatePullPointAction, create.action);
    EXPECT_EQ("http://10.0.0.5/onvif/events", create.url);
    EXPECT_NE(std::string::npos, create.envelope.find("tns1:RuleEngine/CellMotionDetector/Motion</wsnt:TopicExpression>"));
    EXPECT_NE(std::string::npos, create.envelope.find("<Username>admin</Username>"));
    respond(create, subscription(600));

    auto pull = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kPullMessagesAction, pull.action);
    EXPECT_EQ("http://cam/onvif/sub/1", pull.url);
    EXPECT_NE(std::string::npos, pull.envelope.find("<a:To s:mustUnderstand=\"1\">http://cam/onvif/sub/1</a:To>"));
    // The reference parameter, with the namespace declarations it needs.
    EXPECT_NE(std::string::npos, pull.envelope.find("<x:Id xmlns:x=\"urn:x\""));
    EXPECT_NE(std::string::npos, pull.envelope.find(">7</x:Id>"));
    EXPECT_EQ(OnvifEventSubscriber::State::Pulling, state());

    // Initialized false, then true twice (the second dropped), an unsupported topic, then false.
    respond(pull, pulled(motion("false", "Initialized") + motion("true") + motion("true") +
                         motion("true", "Changed", "tnsx:Vendor/Thing") + motion("false")));
    auto next = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kPullMessagesAction, next.action);
    std::vector<std::string> expected = { "MOTION:Initialized:false", "MOTION:Changed:true", "MOTION:Changed:false" };
    EXPECT_EQ(expected, events());
    EXPECT_EQ(3u, _sub->eventCount());
}

TEST_F(OnvifEventSubscriberTest, RefusedFilterSubscribesWithoutIt) {
    start();
    auto create = _transport->next();
    ASSERT_NE(std::string::npos, create.envelope.find("<Filter>"));
    respond(create, "", 400, "wsnt:InvalidFilterFault bad filter");
    auto again = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kCreatePullPointAction, again.action);
    EXPECT_EQ(std::string::npos, again.envelope.find("<Filter>"));
}

TEST_F(OnvifEventSubscriberTest, RejectedCredentialsSuspend) {
    start();
    auto pull = subscribed();
    respond(pull, "", 401);
    ASSERT_TRUE(waitFor([&]() { return state() == OnvifEventSubscriber::State::Suspended; }));
    // No other login attempt soon after.
    EXPECT_TRUE(_transport->next(300).action.empty());

    // The controller connected again: the subscriber starts over.
    start();
    EXPECT_EQ(OnvifEventRequest::kCreatePullPointAction, _transport->next().action);
}

TEST_F(OnvifEventSubscriberTest, LostSubscriptionSubscribesAgainOnce) {
    start();
    auto pull = subscribed();
    respond(pull, "", 500, "ResourceUnknown");
    auto create = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kCreatePullPointAction, create.action);
    respond(create, subscription(600));
    pull = _transport->next();
    respond(pull, "", 500, "ResourceUnknown");
    // Second failure in a row: wait instead of hammering the camera.
    ASSERT_TRUE(waitFor([&]() { return state() == OnvifEventSubscriber::State::Backoff; }));
    EXPECT_TRUE(_transport->next(300).action.empty());
}

TEST_F(OnvifEventSubscriberTest, NetworkErrorBacksOff) {
    start();
    auto pull = subscribed();
    respondNetworkError(pull);
    ASSERT_TRUE(waitFor([&]() { return state() == OnvifEventSubscriber::State::Backoff; }));
}

TEST_F(OnvifEventSubscriberTest, RenewsBeforeTheSubscriptionEnds) {
    start();
    // 20 s left is less than margin + pull timeout + grace (25 s): renew before pulling again.
    auto pull = subscribed(20);
    respond(pull, pulled("", "00:22"));
    auto renew = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kRenewAction, renew.action);
    EXPECT_EQ("http://cam/onvif/sub/1", renew.url);
    respond(renew, envelope(R"(<wsnt:RenewResponse><wsnt:TerminationTime>2026-10-11T00:10:00Z</wsnt:TerminationTime>)"
                            R"(<wsnt:CurrentTime>2026-10-11T00:00:00Z</wsnt:CurrentTime></wsnt:RenewResponse>)"));
    EXPECT_EQ(OnvifEventRequest::kPullMessagesAction, _transport->next().action);
}

TEST_F(OnvifEventSubscriberTest, RenewNotSupportedSubscribesAgain) {
    start();
    auto pull = subscribed(20);
    respond(pull, pulled("", "00:22"));
    auto renew = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kRenewAction, renew.action);
    respond(renew, "", 400, "ActionNotSupported");
    auto unsubscribe = _transport->next();
    EXPECT_EQ(OnvifEventRequest::kUnsubscribeAction, unsubscribe.action);
    EXPECT_EQ(OnvifEventRequest::kCreatePullPointAction, _transport->next().action);
}

TEST_F(OnvifEventSubscriberTest, TurnedOffUnsubscribesAndIgnoresLateAnswers) {
    start();
    auto pull = subscribed();
    auto sub = _sub;
    _poller->sync([sub]() { sub->setCameraEnabled(false); });
    auto unsubscribe = _transport->next();
    ASSERT_EQ(OnvifEventRequest::kUnsubscribeAction, unsubscribe.action);
    EXPECT_EQ(OnvifEventSubscriber::State::Idle, state());
    respond(pull, pulled(motion("true")));
    EXPECT_TRUE(_transport->next(300).action.empty());
    EXPECT_TRUE(events().empty());

    _poller->sync([sub]() { sub->setCameraEnabled(true); });
    EXPECT_EQ(OnvifEventRequest::kCreatePullPointAction, _transport->next().action);
}

TEST_F(OnvifEventSubscriberTest, GlobalSwitchOff) {
    mINI::Instance()[OnvifEventCfg::kEventEnable] = 0;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    start();
    EXPECT_TRUE(_transport->next(300).action.empty());
    EXPECT_EQ(OnvifEventSubscriber::State::Idle, state());

    mINI::Instance()[OnvifEventCfg::kEventEnable] = 1;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
    EXPECT_EQ(OnvifEventRequest::kCreatePullPointAction, _transport->next().action);
}

TEST_F(OnvifEventSubscriberTest, NoSupportedTopicNeverSubscribes) {
    auto ep = endpoint();
    ep.filterTopics.clear();
    auto sub = _sub;
    _poller->sync([sub, ep]() { sub->setEndpoint(ep); });
    EXPECT_TRUE(_transport->next(300).action.empty());
}
