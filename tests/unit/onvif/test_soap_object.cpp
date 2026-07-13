#include <gtest/gtest.h>

#include "Onvif/SoapUtil.h"

TEST(SoapObjectTest, LoadsAndNavigatesNamespacedResponse) {
    const std::string xml =
        "<s:Envelope xmlns:s='urn:soap'><s:Body><GetResponse>"
        "<Item><Name>first</Name></Item><Item><Name>second</Name></Item>"
        "</GetResponse></s:Body></s:Envelope>";
    SoapObject object;
    object.load(xml.data(), xml.size());
    ASSERT_TRUE(object);
    EXPECT_STREQ("first", object["Envelope/Body/GetResponse/Item/Name"].as_xml().text().as_string());
    EXPECT_STREQ("second", object["Envelope/Body/GetResponse"][1]["Name"].as_xml().text().as_string());
    EXPECT_FALSE(object["Envelope/Missing"]);
}

TEST(SoapObjectTest, SerializesNodeAndRejectsMalformedXml) {
    SoapObject object;
    const std::string xml = "<root><child attr='v'>text</child></root>";
    object.load(xml.data(), xml.size());
    EXPECT_STREQ("v", object["root/child"].as_xml().attribute("attr").as_string());
    EXPECT_THROW(object.load("<bad>", 5), std::invalid_argument);
}

TEST(SoapUtilTest, CreatesDiscoveryAndSoapEnvelopes) {
    const std::string uuid = SoapUtil::createUuidString();
    EXPECT_FALSE(uuid.empty());
    const std::string discovery = SoapUtil::createDiscoveryString(uuid);
    EXPECT_NE(std::string::npos, discovery.find(uuid));
    EXPECT_NE(std::string::npos, discovery.find("Probe"));

    const std::string anonymous = SoapUtil::createSoapRequest("<Body/>");
    EXPECT_NE(std::string::npos, anonymous.find("<Body/>"));
    const std::string authenticated = SoapUtil::createSoapRequest("<Body/>", "user", "pass");
    EXPECT_NE(std::string::npos, authenticated.find("UsernameToken"));
    EXPECT_NE(std::string::npos, authenticated.find("user"));
}
