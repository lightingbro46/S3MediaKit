#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <vector>
#include "Control/VendorStage.h"
#include "ext-plugin/DeviceFactory.h"
#include "ext-plugin/IOnvifSetup.h"
#include "ext-plugin/VendorHttp.h"
#include "ext-plugin/Hanwha/HanwhaCamera.h"
#include "ext-plugin/Hikvision/HikvisionCamera.h"
#include "Network/Socket.h"
#include "Thread/semaphore.h"

using namespace managerkit;

namespace {

VendorStage::ProbeResult probeResult(DeviceBrand brand, bool rejected, bool answered) {
    VendorStage::ProbeResult result;
    result.brand = brand;
    result.rejected = rejected;
    result.answered = answered;
    return result;
}

} // namespace

TEST(VendorStageTest, RegistryDescribesTheBoschSpeakerPlugin) {
    VendorPluginInfo info;
    ASSERT_TRUE(DeviceFactory::findPlugin(DeviceType::Speaker, DeviceBrand::BOSCH, info));
    EXPECT_EQ("Bosch", info.name);
    EXPECT_TRUE(info.ownControlAccount);
    EXPECT_EQ("ONVIF", info.requiredProtocol);
    EXPECT_TRUE(info.sharedDsp);
    EXPECT_FALSE(DeviceFactory::findPlugin(DeviceType::Speaker, DeviceBrand::TOA, info));
    EXPECT_FALSE(DeviceFactory::findPlugin(DeviceType::Camera, DeviceBrand::BOSCH, info));
}

TEST(VendorStageTest, RegistryDescribesTheCameraPlugins) {
    const auto &plugins = DeviceFactory::plugins(DeviceType::Camera);
    ASSERT_EQ(2u, plugins.size());
    EXPECT_EQ(DeviceBrand::HIKVISION, plugins[0].brand);
    EXPECT_EQ("Hikvision", plugins[0].name);
    EXPECT_EQ(DeviceBrand::HANWHA, plugins[1].brand);
    EXPECT_EQ("Hanwha Vision", plugins[1].name);
    // Hikvision ONVIF users are apart from the web users; Hanwha ONVIF takes the web account.
    EXPECT_TRUE(plugins[0].ownControlAccount);
    EXPECT_FALSE(plugins[1].ownControlAccount);
    for (const auto &plugin : plugins) {
        // They prepare ONVIF beside the protocols, never constrain them.
        EXPECT_TRUE(plugin.enablesOnvif);
        EXPECT_EQ("", plugin.requiredProtocol);
        EXPECT_FALSE(plugin.sharedDsp);
    }
    VendorPluginInfo info;
    EXPECT_FALSE(DeviceFactory::findPlugin(DeviceType::Speaker, DeviceBrand::HIKVISION, info));
}

TEST(VendorStageTest, ManufacturerNamesMapToBrands) {
    EXPECT_EQ(DeviceBrand::HIKVISION, getDeviceBrand("Hikvision"));
    EXPECT_EQ(DeviceBrand::HIKVISION, getDeviceBrand("HIKVISION DIGITAL TECHNOLOGY CO., LTD"));
    EXPECT_EQ(DeviceBrand::HANWHA, getDeviceBrand("Hanwha Vision"));
    EXPECT_EQ(DeviceBrand::HANWHA, getDeviceBrand("Hanwha Techwin"));
    EXPECT_EQ(DeviceBrand::HANWHA, getDeviceBrand("Wisenet"));
    EXPECT_EQ(DeviceBrand::HANWHA, getDeviceBrand("Samsung Techwin"));
    EXPECT_EQ(DeviceBrand::BOSCH, getDeviceBrand("Bosch Security Systems"));
    EXPECT_EQ(DeviceBrand::UNKNOWN, getDeviceBrand("Dahua"));
}

TEST(VendorStageTest, ConfiguredManufacturerSelectsThePlugin) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "BOSCH Security Systems");
    EXPECT_FALSE(stage.detecting());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    ASSERT_TRUE(stage.hasPlugin());
    EXPECT_EQ("ONVIF", stage.plugin().requiredProtocol);

    // A brand without plugin for this device type only uses the control protocols.
    stage.configure(DeviceType::Speaker, "Hikvision");
    EXPECT_FALSE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_EQ(DeviceBrand::HIKVISION, stage.brand());
}

TEST(VendorStageTest, BlankManufacturerDetectsOnlyWhereThereArePlugins) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "  ");
    EXPECT_TRUE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());

    stage.configure(DeviceType::Camera, "");
    EXPECT_TRUE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
    stage.configure(DeviceType::Camera, "Hanwha Vision");
    EXPECT_FALSE(stage.detecting());
    EXPECT_TRUE(stage.hasPlugin());
}

TEST(VendorStageTest, VendorApiLoginDetectsTheBrand) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    stage.startCycle();
    EXPECT_TRUE(stage.onVendorProbe(probeResult(DeviceBrand::BOSCH, false, true)));
    EXPECT_FALSE(stage.detecting());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    EXPECT_TRUE(stage.hasPlugin());
    EXPECT_EQ("Bosch", stage.name());
}

TEST(VendorStageTest, RejectedOrUnansweredLoginDetectsNothing) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    stage.startCycle();
    EXPECT_FALSE(stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, true, true)));
    EXPECT_TRUE(stage.detecting());
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_TRUE(stage.name().empty());
}

TEST(VendorStageTest, ProtocolDeviceInfoNamesTheManufacturer) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    EXPECT_FALSE(stage.identify(" ")) << "no name: nothing to write back";
    EXPECT_FALSE(stage.detecting()) << "but no point in asking again";

    stage.configure(DeviceType::Speaker, "");
    EXPECT_TRUE(stage.identify("Hikvision"));
    EXPECT_FALSE(stage.hasPlugin());
    EXPECT_EQ("Hikvision", stage.name());

    stage.configure(DeviceType::Speaker, "");
    EXPECT_TRUE(stage.identify("BOSCH Security Systems"));
    EXPECT_TRUE(stage.hasPlugin());
    EXPECT_EQ(DeviceBrand::BOSCH, stage.brand());
    EXPECT_EQ("BOSCH Security Systems", stage.name());
}

TEST(VendorStageTest, CycleBacksOffOnlyWhileNothingAnswers) {
    VendorStage stage;
    stage.configure(DeviceType::Speaker, "");
    const int expected[] = {120, 240, 480, 600, 600};
    for (int want : expected) {
        stage.startCycle();
        stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
        stage.onCycleFailed(true);
        EXPECT_EQ(want, stage.cycleIntervalSec(600));
    }
    EXPECT_EQ(60, stage.cycleIntervalSec(0)) << "0 turns the back-off off";

    // The vendor API answered (HTTP 404), the protocols did not: the device is there.
    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, true));
    stage.onCycleFailed(true);
    EXPECT_EQ(60, stage.cycleIntervalSec(600));

    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
    stage.onCycleFailed(true);
    EXPECT_EQ(120, stage.cycleIntervalSec(600));
    stage.resetBackoff();
    EXPECT_EQ(60, stage.cycleIntervalSec(600));

    // A protocol rejected the password: answered, no back-off.
    stage.startCycle();
    stage.onVendorProbe(probeResult(DeviceBrand::UNKNOWN, false, false));
    stage.onCycleFailed(false);
    EXPECT_EQ(60, stage.cycleIntervalSec(600));
}

// ── Camera plugins: HTTP Digest and the device information pages ─────────────────────────────

TEST(VendorHttpTest, DigestAnswersTheRfc2617Example) {
    const std::string challenge = "Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
                                  "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"";
    const std::string auth = vendorHttpAuthorization(challenge, "GET", "/dir/index.html", "Mufasa", "Circle Of Life", "0a4f113b");
    EXPECT_NE(std::string::npos, auth.find("response=\"6629fae49393a05397450978507c4ef1\"")) << auth;
    EXPECT_NE(std::string::npos, auth.find("opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"")) << auth;
    EXPECT_NE(std::string::npos, auth.find("nc=00000001")) << auth;

    EXPECT_EQ("Basic YWRtaW46cGFzcw==", vendorHttpAuthorization("Basic realm=\"x\"", "GET", "/", "admin", "pass", ""));
    // SHA-256 only, or another scheme: nothing to answer with.
    EXPECT_EQ("", vendorHttpAuthorization("Digest realm=\"x\", nonce=\"n\", algorithm=SHA-256", "GET", "/", "a", "b", "c"));
    EXPECT_EQ("", vendorHttpAuthorization("Bearer realm=\"x\"", "GET", "/", "a", "b", "c"));
}

TEST(VendorHttpTest, FailuresAreClassifiedLikeTheBoschPlugin) {
    EXPECT_EQ(DeviceFailure::None, vendorHttpFailure(0, 200));
    EXPECT_EQ(DeviceFailure::Unauthorized, vendorHttpFailure(0, 401));
    EXPECT_EQ(DeviceFailure::Other, vendorHttpFailure(0, 404));
    EXPECT_EQ(DeviceFailure::Timeout, vendorHttpFailure(toolkit::Err_timeout, 0));
    EXPECT_EQ(DeviceFailure::Unreachable, vendorHttpFailure(toolkit::Err_refused, 0));
}

TEST(HikvisionCameraTest, ParsesIsapiDeviceInfo) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DeviceInfo version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<deviceName>Gate 1</deviceName><deviceID>48443930-3637-3534-3837-c42f90f1c0de</deviceID>"
        "<model>DS-2CD2143G2-I</model><serialNumber>DS-2CD2143G2-I20230101AAWRJ12345678</serialNumber>"
        "<macAddress>c4:2f:90:f1:c0:de</macAddress><firmwareVersion>V5.7.10</firmwareVersion>"
        "</DeviceInfo>";
    IDeviceInfo info;
    ASSERT_TRUE(HikvisionCamera::parseIsapiDeviceInfo(xml, info));
    EXPECT_EQ("Hikvision", info.manufacturer) << "deviceName is the user's name for the camera";
    EXPECT_EQ("DS-2CD2143G2-I", info.model);
    EXPECT_EQ("V5.7.10", info.firmwareVersion);
    EXPECT_EQ("DS-2CD2143G2-I20230101AAWRJ12345678", info.serialNumber);
    EXPECT_EQ("c4:2f:90:f1:c0:de", info.macAddress);

    EXPECT_FALSE(HikvisionCamera::parseIsapiDeviceInfo("<html><body>Not Found</body></html>", info));
    EXPECT_FALSE(HikvisionCamera::parseIsapiDeviceInfo("", info));
}

TEST(HanwhaCameraTest, ParsesSunapiDeviceInfoAsTextOrJson) {
    IDeviceInfo info;
    ASSERT_TRUE(HanwhaCamera::parseSunapiDeviceInfo(
        "Model=XNV-8080R\r\nSerialNumber=ZC7S70GR300017A\r\nFirmwareVersion=2.21.02_20230712\r\n"
        "BuildDate=2023-07-12\r\nConnectedMACAddress=00:16:6C:D1:00:01\r\nDeviceType=NWC\r\n", info));
    EXPECT_EQ("Hanwha Vision", info.manufacturer);
    EXPECT_EQ("XNV-8080R", info.model);
    EXPECT_EQ("ZC7S70GR300017A", info.serialNumber);
    EXPECT_EQ("2.21.02_20230712", info.firmwareVersion);
    EXPECT_EQ("00:16:6C:D1:00:01", info.macAddress);

    IDeviceInfo json_info;
    ASSERT_TRUE(HanwhaCamera::parseSunapiDeviceInfo(
        "{\"Model\":\"PNM-9085RQZ\",\"SerialNumber\":\"ZD1170001\",\"FirmwareVersion\":\"1.41.00\","
        "\"MACAddress\":\"00:16:6C:D1:00:02\"}", json_info));
    EXPECT_EQ("PNM-9085RQZ", json_info.model);
    EXPECT_EQ("00:16:6C:D1:00:02", json_info.macAddress);

    IDeviceInfo none;
    EXPECT_FALSE(HanwhaCamera::parseSunapiDeviceInfo("<html><body>Not Found</body></html>", none));
    EXPECT_FALSE(HanwhaCamera::parseSunapiDeviceInfo("{\"Error\":{\"Code\":600}}", none));
}

TEST(HikvisionCameraTest, TurnsOnvifOnInTheIntegrateDocument) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Integrate version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<CGI><enable>true</enable><certificateType>digest</certificateType></CGI>"
        "<ONVIF><enable>false</enable><certificateType>digest/WSSE</certificateType></ONVIF>"
        "<ISAPI><enable>true</enable></ISAPI></Integrate>";
    bool enabled = true;
    ASSERT_TRUE(HikvisionCamera::parseIntegrateOnvif(xml, enabled));
    EXPECT_FALSE(enabled);
    std::string on;
    ASSERT_TRUE(HikvisionCamera::enableIntegrateOnvif(xml, on));
    ASSERT_TRUE(HikvisionCamera::parseIntegrateOnvif(on, enabled));
    EXPECT_TRUE(enabled);
    // Every other node as the camera sent it.
    EXPECT_NE(std::string::npos, on.find("<certificateType>digest/WSSE</certificateType>"));
    EXPECT_NE(std::string::npos, on.find("<CGI><enable>true</enable>"));

    EXPECT_FALSE(HikvisionCamera::parseIntegrateOnvif("<Integrate><CGI><enable>true</enable></CGI></Integrate>", enabled));
    EXPECT_FALSE(HikvisionCamera::parseIntegrateOnvif("<html/>", enabled));
}

TEST(HikvisionCameraTest, ReadsAndWritesOnvifUsers) {
    std::vector<HikvisionCamera::OnvifUser> users;
    ASSERT_TRUE(HikvisionCamera::parseOnvifUsers(
        "<UserList version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<User><id>1</id><userName>admin</userName><userType>administrator</userType></User>"
        "<User><id>3</id><userName>onvif_vms</userName><userType>operator</userType></User></UserList>", users));
    ASSERT_EQ(2u, users.size());
    EXPECT_EQ("3", users[1].id);
    EXPECT_EQ("onvif_vms", users[1].name);
    ASSERT_TRUE(HikvisionCamera::parseOnvifUsers("<UserList version=\"2.0\"/>", users));
    EXPECT_TRUE(users.empty());
    EXPECT_FALSE(HikvisionCamera::parseOnvifUsers("<ResponseStatus/>", users));

    const std::string user = HikvisionCamera::onvifUserXml("4", "onvif_vms", "p<&>\"w");
    EXPECT_NE(std::string::npos, user.find("<id>4</id>"));
    EXPECT_NE(std::string::npos, user.find("<userName>onvif_vms</userName>"));
    EXPECT_NE(std::string::npos, user.find("<password>p&lt;&amp;&gt;\"w</password>")) << user;
    EXPECT_NE(std::string::npos, user.find("<userType>administrator</userType>"));
}

TEST(HanwhaCameraTest, ReadsSunapiFields) {
    std::map<std::string, std::string> fields;
    ASSERT_TRUE(HanwhaCamera::parseSunapiFields("Enable=False\r\n", fields));
    EXPECT_EQ("False", fields["Enable"]);
    fields.clear();
    ASSERT_TRUE(HanwhaCamera::parseSunapiFields("{\"Enable\":true}", fields));
    EXPECT_EQ("True", fields["Enable"]);
    fields.clear();
    EXPECT_FALSE(HanwhaCamera::parseSunapiFields("<html/>", fields));
}

namespace {

// VendorStage::probe of a camera at `addr` (ip:port) with the account; waits for its answer.
VendorStage::ProbeResult probeCamera(const std::string &addr, const std::string &password) {
    IDeviceConfig cfg;
    cfg.id = "vendor-camera-test";
    cfg.ip = addr.substr(0, addr.find(':'));
    cfg.port = atoi(addr.substr(addr.find(':') + 1).c_str());
    cfg.username = "admin";
    cfg.password = password;
    toolkit::semaphore sem;
    VendorStage::ProbeResult out;
    VendorStage::probe(DeviceType::Camera, cfg, [&](const VendorStage::ProbeResult &result) {
        out = result;
        sem.post();
    });
    sem.wait();
    return out;
}

} // namespace

// Against tests/integration/stress_vms/psia_mock.py --dialect isapi (Digest); skipped unless
// ISAPI_MOCK_ADDR=ip:port is set.
TEST(VendorCameraMockTest, HikvisionIsIdentifiedOverIsapiWithDigest) {
    const char *addr = getenv("ISAPI_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "ISAPI_MOCK_ADDR not set";
    }
    auto result = probeCamera(addr, "admin123");
    EXPECT_EQ(DeviceBrand::HIKVISION, result.brand);
    EXPECT_EQ("Hikvision", result.info.manufacturer);
    EXPECT_EQ("DS-2DE4425IW-DE", result.info.model);

    auto rejected = probeCamera(addr, "wrong-password");
    EXPECT_EQ(DeviceBrand::UNKNOWN, rejected.brand);
    EXPECT_TRUE(rejected.rejected);
}

// Against tests/integration/stress_vms/sunapi_mock.py (Digest); skipped unless SUNAPI_MOCK_ADDR=ip:port is set.
TEST(VendorCameraMockTest, HanwhaIsIdentifiedOverSunapiWithDigest) {
    const char *addr = getenv("SUNAPI_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "SUNAPI_MOCK_ADDR not set";
    }
    auto result = probeCamera(addr, "admin123");
    EXPECT_EQ(DeviceBrand::HANWHA, result.brand);
    EXPECT_EQ("Hanwha Vision", result.info.manufacturer);
    EXPECT_EQ("XNV-8080R", result.info.model);

    auto rejected = probeCamera(addr, "wrong-password");
    EXPECT_EQ(DeviceBrand::UNKNOWN, rejected.brand);
    EXPECT_TRUE(rejected.rejected);
}

// Nothing listens: no brand, and not counted as an answer.
TEST(VendorCameraMockTest, UnreachableCameraIsNotAnAnswer) {
    auto result = probeCamera("127.0.0.1:1", "admin123");
    EXPECT_EQ(DeviceBrand::UNKNOWN, result.brand);
    EXPECT_FALSE(result.answered);
    EXPECT_FALSE(result.rejected);
}

namespace {

struct Prepared {
    bool ok = false;
    std::string msg;
    DeviceFailure failure = DeviceFailure::None;
};

// IOnvifSetup::prepareOnvif of the `brand` plugin of a camera at `addr`; waits for its answer.
Prepared prepareCamera(DeviceBrand brand, const std::string &addr, const std::string &password,
                       const std::string &onvif_username) {
    IDeviceConfig cfg;
    cfg.id = "vendor-camera-test";
    cfg.type = DeviceType::Camera;
    cfg.brand = brand;
    cfg.ip = addr.substr(0, addr.find(':'));
    cfg.port = atoi(addr.substr(addr.find(':') + 1).c_str());
    cfg.username = "admin";
    cfg.password = password;
    auto device = DeviceFactory::create(cfg);
    auto setup = dynamic_cast<IOnvifSetup *>(device.get());
    Prepared out;
    if (!setup) {
        out.msg = "no IOnvifSetup";
        return out;
    }
    toolkit::semaphore sem;
    setup->prepareOnvif(onvif_username, password, [&](bool ok, const std::string &msg) {
        out.ok = ok;
        out.msg = msg;
        out.failure = device->lastFailure();
        sem.post();
    });
    sem.wait();
    return out;
}

} // namespace

// Against psia_mock.py --dialect isapi --onvif-off: ONVIF off and no ONVIF user at first.
TEST(VendorCameraMockTest, HikvisionTurnsOnvifOnAndCreatesThenUpdatesItsUser) {
    const char *addr = getenv("ISAPI_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "ISAPI_MOCK_ADDR not set";
    }
    auto created = prepareCamera(DeviceBrand::HIKVISION, addr, "admin123", "onvif_vms");
    EXPECT_TRUE(created.ok) << created.msg;
    auto updated = prepareCamera(DeviceBrand::HIKVISION, addr, "admin123", "onvif_vms");
    EXPECT_TRUE(updated.ok) << updated.msg;
    EXPECT_EQ("ONVIF user updated", updated.msg);

    auto rejected = prepareCamera(DeviceBrand::HIKVISION, addr, "wrong-password", "onvif_vms");
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(DeviceFailure::Unauthorized, rejected.failure);
}

// Against sunapi_mock.py --onvif-off.
TEST(VendorCameraMockTest, HanwhaTurnsOnvifOn) {
    const char *addr = getenv("SUNAPI_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "SUNAPI_MOCK_ADDR not set";
    }
    auto prepared = prepareCamera(DeviceBrand::HANWHA, addr, "admin123", "");
    EXPECT_TRUE(prepared.ok) << prepared.msg;
    auto again = prepareCamera(DeviceBrand::HANWHA, addr, "admin123", "");
    EXPECT_TRUE(again.ok) << again.msg;
    EXPECT_EQ("ONVIF on", again.msg);
    EXPECT_FALSE(prepareCamera(DeviceBrand::HANWHA, addr, "admin123", "onvif_vms").ok) << "no ONVIF users";
}

// Nothing listens: the network, not the account.
TEST(VendorCameraMockTest, UnreachableCameraIsNotARejectedAccount) {
    auto prepared = prepareCamera(DeviceBrand::HIKVISION, "127.0.0.1:1", "admin123", "onvif_vms");
    EXPECT_FALSE(prepared.ok);
    EXPECT_EQ(DeviceFailure::Unreachable, prepared.failure);
}
