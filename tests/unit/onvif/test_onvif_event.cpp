#include <gtest/gtest.h>
#include "Onvif/OnvifEvent.h"
#include "Onvif/OnvifEventTopics.h"

using namespace mediakit;

namespace {

pugi::xml_node soapBody(pugi::xml_document &doc, const std::string &xml) {
    EXPECT_TRUE(doc.load_string(xml.c_str()));
    return doc.first_child().child("SOAP-ENV:Body");
}

} // namespace

TEST(OnvifEventTopicsTest, ClassifiesStandardTopicsWhateverThePrefix) {
    auto rule = OnvifEventTopics::classify("tns1:RuleEngine/CellMotionDetector/Motion");
    ASSERT_NE(nullptr, rule);
    EXPECT_STREQ("MOTION", rule->eventType);
    EXPECT_STREQ("IsMotion", rule->stateItem);

    rule = OnvifEventTopics::classify(" ns2:VideoSource/ns2:MotionAlarm \n");
    ASSERT_NE(nullptr, rule);
    EXPECT_STREQ("MOTION", rule->eventType);

    rule = OnvifEventTopics::classify("tt:ruleengine/tamperdetector/tamper");
    ASSERT_NE(nullptr, rule);
    EXPECT_STREQ("TAMPER", rule->eventType);

    EXPECT_EQ(nullptr, OnvifEventTopics::classify("tnshik:RuleEngine/FaceDetection"));
    EXPECT_EQ(nullptr, OnvifEventTopics::classify("tns1:RuleEngine/CellMotionDetector"));
    EXPECT_EQ(nullptr, OnvifEventTopics::classify(""));
}

TEST(OnvifEventTopicsTest, ConcreteTopicUsesTheOnvifPrefix) {
    auto rule = OnvifEventTopics::classify("abc:Device/Trigger/DigitalInput");
    ASSERT_NE(nullptr, rule);
    EXPECT_EQ("tns1:Device/Trigger/DigitalInput", OnvifEventTopics::concreteTopic(*rule));
    EXPECT_LT(OnvifEventTopics::eventTypeOrder("MOTION"), OnvifEventTopics::eventTypeOrder("TAMPER"));
    EXPECT_EQ(OnvifEventTopics::rules().size(), OnvifEventTopics::eventTypeOrder("NOPE"));
}

// gSOAP keeps each child of wstop:TopicSet as a literal fragment, without the envelope xmlns.
TEST(OnvifEventTest, ParsesTopicSetFragments) {
    std::vector<std::string> fragments = {
        R"(<tns1:RuleEngine wstop:topic="false">
             <CellMotionDetector wstop:topic="false">
               <Motion wstop:topic="true">
                 <tt:MessageDescription IsProperty="true">
                   <tt:Source>
                     <tt:SimpleItemDescription Name="VideoSourceConfigurationToken" Type="tt:ReferenceToken"/>
                     <tt:SimpleItemDescription Name="Rule" Type="xs:string"/>
                   </tt:Source>
                   <tt:Data><tt:SimpleItemDescription Name="IsMotion" Type="xs:boolean"/></tt:Data>
                 </tt:MessageDescription>
               </Motion>
             </CellMotionDetector>
             <TamperDetector><Tamper wstop:topic="true"><tt:MessageDescription IsProperty="true"/></Tamper></TamperDetector>
           </tns1:RuleEngine>)",
        // Topic without wstop:topic but with a MessageDescription (seen on old firmware).
        R"(<tns1:VideoSource><MotionAlarm><tt:MessageDescription IsProperty="true">
             <tt:Data><tt:SimpleItemDescription Name="State" Type="xs:boolean"/></tt:Data>
           </tt:MessageDescription></MotionAlarm></tns1:VideoSource>)",
        R"(<tnshik:RuleEngine><FaceDetection wstop:topic="true"/></tnshik:RuleEngine>)",
        "<broken",
    };
    bool truncated = true;
    auto topics = parseOnvifTopicSet(fragments, 100, &truncated);
    EXPECT_FALSE(truncated);
    ASSERT_EQ(4u, topics.size());
    EXPECT_EQ("tns1:RuleEngine/CellMotionDetector/Motion", topics[0].topic);
    EXPECT_TRUE(topics[0].isProperty);
    ASSERT_EQ(2u, topics[0].sourceItems.size());
    EXPECT_EQ("Rule", topics[0].sourceItems[1]);
    ASSERT_EQ(1u, topics[0].dataItems.size());
    EXPECT_EQ("IsMotion", topics[0].dataItems[0]);
    EXPECT_EQ("tns1:RuleEngine/TamperDetector/Tamper", topics[1].topic);
    EXPECT_EQ("tns1:VideoSource/MotionAlarm", topics[2].topic);
    EXPECT_EQ("tnshik:RuleEngine/FaceDetection", topics[3].topic);
    EXPECT_FALSE(topics[3].isProperty);

    topics = parseOnvifTopicSet(fragments, 2, &truncated);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(2u, topics.size());
}

TEST(OnvifEventTest, ParsesPullMessagesResponse) {
    pugi::xml_document doc;
    auto body = soapBody(doc, R"(<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope"><SOAP-ENV:Body>
      <tev:PullMessagesResponse>
        <tev:CurrentTime>2026-10-11T03:12:05Z</tev:CurrentTime>
        <tev:TerminationTime>2026-10-11T03:13:05Z</tev:TerminationTime>
        <wsnt:NotificationMessage>
          <wsnt:Topic Dialect="http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet">tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>
          <wsnt:Message><tt:Message UtcTime="2026-10-11T03:12:04.250Z" PropertyOperation="Changed">
            <tt:Source><tt:SimpleItem Name="VideoSourceConfigurationToken" Value="VSC_1"/><tt:SimpleItem Name="Rule" Value="R1"/></tt:Source>
            <tt:Data><tt:SimpleItem Name="IsMotion" Value="true"/></tt:Data>
          </tt:Message></wsnt:Message>
        </wsnt:NotificationMessage>
        <wsnt:NotificationMessage>
          <wsnt:Topic>tnsaxis:Storage/Disruption</wsnt:Topic>
          <wsnt:Message><tt:Message UtcTime="2026-10-11T10:12:04+07:00"/></wsnt:Message>
        </wsnt:NotificationMessage>
      </tev:PullMessagesResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>)");
    OnvifPullResult result;
    ASSERT_TRUE(parseOnvifPullMessages(body, result));
    EXPECT_EQ(parseXsDateTime("2026-10-11T03:12:05Z"), result.currentTime);
    EXPECT_EQ(60, result.terminationTime - result.currentTime);
    ASSERT_EQ(2u, result.messages.size());
    auto &motion = result.messages[0];
    EXPECT_EQ("MOTION", motion.eventType);
    EXPECT_EQ("Changed", motion.propertyOp);
    EXPECT_EQ("true", motion.stateValue());
    EXPECT_EQ(result.currentTime - 1, motion.cameraUtc);
    EXPECT_EQ("MOTION|VideoSourceConfigurationToken=VSC_1|Rule=R1", motion.stateKey());
    EXPECT_EQ("", result.messages[1].eventType);
    EXPECT_EQ(result.currentTime - 1, result.messages[1].cameraUtc);

    pugi::xml_document other;
    EXPECT_FALSE(parseOnvifPullMessages(soapBody(other, R"(<SOAP-ENV:Envelope><SOAP-ENV:Body><x/></SOAP-ENV:Body></SOAP-ENV:Envelope>)"), result));
}

TEST(OnvifEventTest, ParsesSubscriptionWithReferenceParameters) {
    pugi::xml_document doc;
    auto body = soapBody(doc, R"(<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" xmlns:dom0="urn:axis"><SOAP-ENV:Body>
      <tev:CreatePullPointSubscriptionResponse>
        <tev:SubscriptionReference>
          <wsa5:Address>http://10.0.0.5/onvif/services?a=1&amp;b=2</wsa5:Address>
          <wsa5:ReferenceParameters><dom0:SubscriptionId>17</dom0:SubscriptionId></wsa5:ReferenceParameters>
        </tev:SubscriptionReference>
        <wsnt:CurrentTime>2026-10-11T03:12:05Z</wsnt:CurrentTime>
        <wsnt:TerminationTime>2026-10-11T03:13:05Z</wsnt:TerminationTime>
      </tev:CreatePullPointSubscriptionResponse></SOAP-ENV:Body></SOAP-ENV:Envelope>)");
    OnvifSubscriptionRef ref;
    ASSERT_TRUE(parseOnvifCreatePullPoint(body, ref));
    EXPECT_EQ("http://10.0.0.5/onvif/services?a=1&b=2", ref.address);
    EXPECT_NE(std::string::npos, ref.referenceParameters.find("<dom0:SubscriptionId"));
    EXPECT_NE(std::string::npos, ref.referenceParameters.find(R"(xmlns:dom0="urn:axis")"));
    EXPECT_EQ(60, ref.terminationTime - ref.currentTime);

    auto header = OnvifEventRequest::addressingHeader(OnvifEventRequest::kPullMessagesAction, ref.address,
                                                      ref.referenceParameters);
    EXPECT_NE(std::string::npos, header.find("services?a=1&amp;b=2</a:To>"));
}

TEST(OnvifEventTest, BuildsFilteredSubscription) {
    auto body = OnvifEventRequest::createPullPoint({ "tns1:RuleEngine/CellMotionDetector/Motion", "tns1:VideoSource/MotionAlarm" }, 60);
    EXPECT_NE(std::string::npos, body.find("tns1:RuleEngine/CellMotionDetector/Motion|tns1:VideoSource/MotionAlarm</wsnt:TopicExpression>"));
    EXPECT_NE(std::string::npos, body.find("<InitialTerminationTime>PT60S</InitialTerminationTime>"));
    EXPECT_EQ(std::string::npos, OnvifEventRequest::createPullPoint({}, 30).find("Filter"));
    EXPECT_NE(std::string::npos, OnvifEventRequest::pullMessages(10, 64).find("<Timeout>PT10S</Timeout><MessageLimit>64</MessageLimit>"));
}

TEST(OnvifEventTest, ParsesXsDateTime) {
    EXPECT_EQ(0, parseXsDateTime(""));
    EXPECT_EQ(0, parseXsDateTime("yesterday"));
    EXPECT_EQ(1760152325, parseXsDateTime("2025-10-11T03:12:05Z"));
    EXPECT_EQ(1760152325, parseXsDateTime("2025-10-11T03:12:05"));
    EXPECT_EQ(1760152325, parseXsDateTime("2025-10-11T03:12:05.999Z"));
    EXPECT_EQ(1760152325, parseXsDateTime("2025-10-11T10:12:05+07:00"));
    EXPECT_EQ(1760152325, parseXsDateTime("2025-10-10T22:12:05-05:00"));
}
