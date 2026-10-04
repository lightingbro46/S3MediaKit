#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Backchannel/BackchannelManager.h"
#include "Backchannel/IsapiTwoWayAudioPusher.h"
#include "Control/OnvifDeviceControl.h"
#include "Control/PsiaDeviceControl.h"

using namespace managerkit;
using namespace managerkit::psia_detail;

namespace {

// gSOAP's `namespaces` table is defined in OnvifDeviceControl.cpp (wsdd.nsmap). The server always links
// it through CameraController; this binary has to pull it in itself.
bool (*const kPullsGsoapNamespaces)(time_t, time_t, int) = &OnvifControl::clockOffsetFresh;

const char *kDeviceInfo =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<DeviceInfo version=\"1.0\" xmlns=\"urn:psialliance-org\">"
    "<deviceName>Gate</deviceName><deviceID>88a1c2d3-0001</deviceID><model>DS-2CD2</model>"
    "<serialNumber>SN123</serialNumber><macAddress>44:19:b6:00:00:01</macAddress>"
    "<firmwareVersion>V5.4.5</firmwareVersion></DeviceInfo>";

// Two video channels (one on a custom RTSP port), one disabled channel and one audio-only channel.
const char *kChannels =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<psia:StreamingChannelList xmlns:psia=\"urn:psialliance-org\" version=\"1.0\">"
    "<psia:StreamingChannel><psia:id>101</psia:id><psia:enabled>true</psia:enabled>"
    "<psia:Transport><psia:rtspPortNo>554</psia:rtspPortNo></psia:Transport>"
    "<psia:Video><psia:enabled>true</psia:enabled><psia:videoCodecType>H.264</psia:videoCodecType>"
    "<psia:videoResolutionWidth>1920</psia:videoResolutionWidth><psia:videoResolutionHeight>1080</psia:videoResolutionHeight>"
    "<psia:videoQualityControlType>CBR</psia:videoQualityControlType><psia:constantBitRate>4096</psia:constantBitRate>"
    "<psia:maxFrameRate>2500</psia:maxFrameRate></psia:Video>"
    "<psia:Audio><psia:enabled>true</psia:enabled><psia:audioCompressionType>G.711ulaw</psia:audioCompressionType></psia:Audio>"
    "</psia:StreamingChannel>"
    "<psia:StreamingChannel><psia:id>102</psia:id>"
    "<psia:Transport><psia:rtspPortNo>8554</psia:rtspPortNo></psia:Transport>"
    "<psia:Video><psia:videoCodecType>H.265</psia:videoCodecType>"
    "<psia:videoResolutionWidth>640</psia:videoResolutionWidth><psia:videoResolutionHeight>360</psia:videoResolutionHeight>"
    "<psia:videoQualityControlType>VBR</psia:videoQualityControlType><psia:constantBitRate>256</psia:constantBitRate>"
    "<psia:vbrUpperCap>512</psia:vbrUpperCap><psia:maxFrameRate>1250</psia:maxFrameRate></psia:Video>"
    "</psia:StreamingChannel>"
    "<psia:StreamingChannel><psia:id>103</psia:id>"
    "<psia:Video><psia:enabled>false</psia:enabled><psia:videoCodecType>MJPEG</psia:videoCodecType></psia:Video>"
    "</psia:StreamingChannel>"
    "<psia:StreamingChannel><psia:id>104</psia:id>"
    "<psia:Audio><psia:enabled>true</psia:enabled></psia:Audio></psia:StreamingChannel>"
    "</psia:StreamingChannelList>";

const char *kChannelCaps =
    "<StreamingChannel version=\"1.0\" xmlns=\"urn:psialliance-org\"><id>101</id>"
    "<Video><videoCodecType opt=\"H.264,H.265,MJPEG\">H.264</videoCodecType>"
    "<videoResolutionWidth opt=\"1920,1280,640\">1920</videoResolutionWidth>"
    "<videoResolutionHeight opt=\"1080,720,360\">1080</videoResolutionHeight>"
    "<constantBitRate min=\"32\" max=\"16384\">4096</constantBitRate>"
    "<maxFrameRate opt=\"2500,1500,50\">2500</maxFrameRate></Video></StreamingChannel>";

const char *kChannel =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<StreamingChannel version=\"1.0\" xmlns=\"urn:psialliance-org\"><id>101</id>"
    "<Video><enabled>true</enabled><videoCodecType>H.264</videoCodecType>"
    "<videoResolutionWidth>1920</videoResolutionWidth><videoResolutionHeight>1080</videoResolutionHeight>"
    "<videoQualityControlType>CBR</videoQualityControlType><constantBitRate>4096</constantBitRate>"
    "<maxFrameRate>2500</maxFrameRate><vendorExtension>keep-me</vendorExtension></Video>"
    "<Extensions><smartCodec>off</smartCodec></Extensions></StreamingChannel>";

const char *kPtzCaps =
    "<PTZChanelCap version=\"1.0\" xmlns=\"urn:psialliance-org\">"
    "<AbsolutePanTiltPositionSpace><XRange><Min>0</Min><Max>3600</Max></XRange>"
    "<YRange><Min>-900</Min><Max>900</Max></YRange></AbsolutePanTiltPositionSpace>"
    "<AbsoluteZoomPositionSpace><ZRange><Min>10</Min><Max>40</Max></ZRange></AbsoluteZoomPositionSpace>"
    "<ContinuousPanTiltSpace><XRange><Min>-100</Min><Max>100</Max></XRange>"
    "<YRange><Min>-100</Min><Max>100</Max></YRange></ContinuousPanTiltSpace>"
    "<ContinuousZoomSpace><ZRange><Min>-100</Min><Max>100</Max></ZRange></ContinuousZoomSpace>"
    "</PTZChanelCap>";

} // namespace

TEST(PsiaParseTest, DeviceInfoWithAndWithoutNamespacePrefix) {
    OnvifDeviceInfo info;
    ASSERT_TRUE(parseDeviceInfo(kDeviceInfo, info));
    EXPECT_EQ("Gate", info.manufacturer); // no <manufacturer>: deviceName
    EXPECT_EQ("DS-2CD2", info.model);
    EXPECT_EQ("V5.4.5", info.firmwareVersion);
    EXPECT_EQ("SN123", info.serialNumber);
    EXPECT_EQ("44:19:b6:00:00:01", info.macAddress);
    EXPECT_EQ("88a1c2d3-0001", info.hardwareId);

    OnvifDeviceInfo prefixed;
    ASSERT_TRUE(parseDeviceInfo("<p:DeviceInfo xmlns:p=\"urn:psialliance-org\"><p:manufacturer>ACME</p:manufacturer>"
                                "<p:model>X1</p:model></p:DeviceInfo>", prefixed));
    EXPECT_EQ("ACME", prefixed.manufacturer);
    EXPECT_EQ("X1", prefixed.model);

    OnvifDeviceInfo none;
    EXPECT_FALSE(parseDeviceInfo("<html><body>Not found</body></html>", none));
    EXPECT_FALSE(parseDeviceInfo("", none));
}

TEST(PsiaParseTest, StreamingChannelsBecomeProfilesWithRtspUrls) {
    OnvifMediaProfileMap profiles;
    ASSERT_TRUE(parseStreamingChannels(kChannels, "10.0.0.5", profiles));
    ASSERT_EQ(2u, profiles.size()); // disabled and audio-only channels are skipped

    EXPECT_EQ("101", profiles[0].token);
    EXPECT_EQ("rtsp://10.0.0.5:554/PSIA/Streaming/channels/101", profiles[0].url);
    EXPECT_EQ("H264", profiles[0].vcodec);
    EXPECT_EQ(1920, profiles[0].width);
    EXPECT_EQ(1080, profiles[0].height);
    EXPECT_EQ(4096, profiles[0].bitrate);
    EXPECT_FLOAT_EQ(25.0f, profiles[0].fps);
    EXPECT_TRUE(profiles[0].hasAudio);
    EXPECT_EQ("G711", profiles[0].acodec);

    EXPECT_EQ("rtsp://10.0.0.5:8554/PSIA/Streaming/channels/102", profiles[1].url);
    EXPECT_EQ("H265", profiles[1].vcodec);
    EXPECT_EQ(512, profiles[1].bitrate); // VBR: the upper cap
    EXPECT_FLOAT_EQ(12.5f, profiles[1].fps);

    OnvifMediaProfileMap single;
    ASSERT_TRUE(parseStreamingChannels(kChannel, "h", single));
    ASSERT_EQ(1u, single.size());
    EXPECT_EQ("101", single[0].token);

    EXPECT_FALSE(parseStreamingChannels(kDeviceInfo, "h", single));
}

TEST(PsiaParseTest, ChannelCapabilitiesFillTheEncoderOptions) {
    OnvifMediaProfileMap profiles;
    ASSERT_TRUE(parseStreamingChannels(kChannel, "h", profiles));
    OnvifMediaProfile profile = profiles.front();
    parseChannelCapabilities(kChannelCaps, profile);

    ASSERT_EQ(3u, profile.vEncoderOptionMap.size());
    ASSERT_TRUE(profile.vEncoderOptionMap.count("H264"));
    ASSERT_TRUE(profile.vEncoderOptionMap.count("H265"));
    ASSERT_TRUE(profile.vEncoderOptionMap.count("JPEG"));
    const auto &opt = profile.vEncoderOptionMap["H264"];
    ASSERT_EQ(3u, opt.ResolutionsAvailable.size());
    EXPECT_EQ(std::make_pair(1280, 720), opt.ResolutionsAvailable[1]);
    EXPECT_EQ("25 15 0.5", opt.FrameRatesSupported);
    EXPECT_FALSE(opt.BitRateRange.empty());
    EXPECT_EQ(0u, opt.BitRateRange.find("32 "));
    EXPECT_TRUE(opt.FPSEditable);
    EXPECT_TRUE(opt.bitrateEditable);
    EXPECT_TRUE(opt.resolutionEditable);
    EXPECT_TRUE(profile.videoEncEditable);
    EXPECT_TRUE(profile.videoConfigEditable);
}

TEST(PsiaParseTest, EncoderWriteKeepsTheNodesItDoesNotOwn) {
    VideoEncoderConfig config;
    config.vcodec = "H265";
    config.width = 1280;
    config.height = 720;
    config.bitrate = 2048;
    config.fps = 15;
    config.quality = 0;
    std::string out;
    ASSERT_TRUE(applyVideoEncoderConfig(kChannel, config, out));
    EXPECT_NE(std::string::npos, out.find("<videoCodecType>H.265</videoCodecType>"));
    EXPECT_NE(std::string::npos, out.find("<videoResolutionWidth>1280</videoResolutionWidth>"));
    EXPECT_NE(std::string::npos, out.find("<constantBitRate>2048</constantBitRate>"));
    EXPECT_NE(std::string::npos, out.find("<maxFrameRate>1500</maxFrameRate>"));
    EXPECT_NE(std::string::npos, out.find("<vendorExtension>keep-me</vendorExtension>"));
    EXPECT_NE(std::string::npos, out.find("<smartCodec>off</smartCodec>"));
    EXPECT_NE(std::string::npos, out.find("xmlns=\"urn:psialliance-org\""));

    // Read back, it is the config that was written.
    OnvifMediaProfileMap profiles;
    ASSERT_TRUE(parseStreamingChannels(out, "h", profiles));
    ASSERT_EQ(1u, profiles.size());
    EXPECT_TRUE(profiles[0].isVideoConfigEqual(config));

    EXPECT_FALSE(applyVideoEncoderConfig(kDeviceInfo, config, out));
}

TEST(PsiaParseTest, ResponseStatusErrors) {
    ResponseStatus status;
    ASSERT_TRUE(parseResponseStatus("<ResponseStatus xmlns=\"urn:psialliance-org\"><statusCode>6</statusCode>"
                                    "<statusString>Invalid XML Content</statusString>"
                                    "<subStatusCode>badParameters</subStatusCode></ResponseStatus>", status));
    EXPECT_EQ(6, status.statusCode);
    EXPECT_EQ("Invalid XML Content", status.statusString);
    EXPECT_EQ("badParameters", status.subStatusCode);
    EXPECT_FALSE(isResponseStatusOk(status));
    status.statusCode = 7; // Reboot Required: the change was accepted
    EXPECT_TRUE(isResponseStatusOk(status));
    EXPECT_FALSE(parseResponseStatus("<Other/>", status));
}

TEST(PsiaParseTest, CodecNames) {
    EXPECT_EQ("H264", codecFromPsia("H.264"));
    EXPECT_EQ("H265", codecFromPsia("H.265+"));
    EXPECT_EQ("JPEG", codecFromPsia("MJPEG"));
    EXPECT_EQ("H.264", codecToPsia("H264"));
    EXPECT_EQ("MJPEG", codecToPsia("JPEG"));
}

TEST(PsiaPtzTest, CapabilitiesAndNormalizedPositions) {
    PtzSpace space;
    OnvifPTZProfile profile;
    ASSERT_TRUE(parsePtzCapabilities(kPtzCaps, space, profile));
    EXPECT_TRUE(profile.isAbsMoveEnable);
    EXPECT_TRUE(profile.isConsMoveEnable);
    EXPECT_FALSE(profile.isRelMoveEnable);
    EXPECT_FLOAT_EQ(-1, profile.absMinPan);
    EXPECT_FLOAT_EQ(1, profile.absMaxPan);
    EXPECT_FLOAT_EQ(0, profile.absMinZoom);
    EXPECT_FLOAT_EQ(1, profile.absMaxZoom);
    EXPECT_FLOAT_EQ(100, space.consPanMax);

    // Normalized → raw → normalized round trip, and the ends of each range.
    EXPECT_FLOAT_EQ(0, toRaw(-1, space.absPanMin, space.absPanMax));
    EXPECT_FLOAT_EQ(1800, toRaw(0, space.absPanMin, space.absPanMax));
    EXPECT_FLOAT_EQ(3600, toRaw(1, space.absPanMin, space.absPanMax));
    EXPECT_FLOAT_EQ(3600, toRaw(5, space.absPanMin, space.absPanMax)); // clamped
    EXPECT_FLOAT_EQ(10, toRaw(0, space.absZoomMin, space.absZoomMax, true));
    EXPECT_FLOAT_EQ(40, toRaw(1, space.absZoomMin, space.absZoomMax, true));
    EXPECT_FLOAT_EQ(0.5f, toNormalized(toRaw(0.5f, -900, 900), -900, 900));
    EXPECT_FLOAT_EQ(0, toNormalized(5, 3, 3)); // empty range

    float pan = 9, tilt = 9, zoom = 9;
    ASSERT_TRUE(parsePtzStatus("<PTZStatus><AbsoluteHigh><elevation>450</elevation><azimuth>900</azimuth>"
                               "<absoluteZoom>25</absoluteZoom></AbsoluteHigh></PTZStatus>", space, pan, tilt, zoom));
    EXPECT_FLOAT_EQ(-0.5f, pan);
    EXPECT_FLOAT_EQ(0.5f, tilt);
    EXPECT_FLOAT_EQ(0.5f, zoom);
    EXPECT_FALSE(parsePtzStatus("<PTZStatus/>", space, pan, tilt, zoom));

    OnvifPTZProfile none;
    EXPECT_FALSE(parsePtzCapabilities("<PTZChanelCap/>", space, none));
    EXPECT_FALSE(none.isAbsMoveEnable || none.isConsMoveEnable || none.isRelMoveEnable);
}

TEST(PsiaPtzTest, PresetsOutputsAndChannels) {
    OnvifPTZProfile::PTZPresetMap presets;
    ASSERT_TRUE(parsePresets("<PTZPresetList><PTZPreset><id>1</id><presetName>Gate</presetName><enabled>true</enabled>"
                             "</PTZPreset><PTZPreset><id>2</id><enabled>false</enabled></PTZPreset>"
                             "<PTZPreset><id>3</id></PTZPreset></PTZPresetList>", presets));
    ASSERT_EQ(2u, presets.size());
    EXPECT_EQ("Gate", presets["1"].Name);
    EXPECT_EQ("Preset 3", presets["3"].Name);

    std::vector<OnvifRelayOutputProfile> outputs;
    ASSERT_TRUE(parseOutputs("<IOOutputPortList><IOOutputPort><id>1</id></IOOutputPort>"
                             "<IOOutputPort><id>2</id><name>Siren</name></IOOutputPort></IOOutputPortList>", outputs));
    ASSERT_EQ(2u, outputs.size());
    EXPECT_EQ("Output 1", outputs[0].name);
    EXPECT_EQ("Siren", outputs[1].name);

    EXPECT_EQ("2", parseFirstPtzChannel("<PTZChannelList><PTZChannel><id>1</id><enabled>false</enabled></PTZChannel>"
                                        "<PTZChannel><id>2</id></PTZChannel></PTZChannelList>"));
    EXPECT_EQ("", parseFirstPtzChannel("<PTZChannelList/>"));
}

TEST(PsiaTimeTest, PosixTimeZonesAndLocalTime) {
    EXPECT_EQ(7 * 3600, posixTimeZoneOffset("CST-7:00:00"));
    EXPECT_EQ(-(5 * 3600 + 30 * 60), posixTimeZoneOffset("EST+5:30:00"));
    EXPECT_EQ(0, posixTimeZoneOffset("UTC"));

    time_t utc = 0;
    bool suffix = false;
    ASSERT_TRUE(parseLocalTime("2026-10-03T19:00:00", 7 * 3600, utc, suffix));
    EXPECT_FALSE(suffix);
    EXPECT_EQ(static_cast<time_t>(1791028800), utc); // 2026-10-03T12:00:00Z
    ASSERT_TRUE(parseLocalTime("2026-10-03T19:00:00+07:00", 0, utc, suffix));
    EXPECT_TRUE(suffix);
    EXPECT_EQ(static_cast<time_t>(1791028800), utc);
    ASSERT_TRUE(parseLocalTime("2026-10-03T12:00:00.250Z", 3600, utc, suffix));
    EXPECT_EQ(static_cast<time_t>(1791028800), utc);
    EXPECT_FALSE(parseLocalTime("yesterday", 0, utc, suffix));
}

TEST(PsiaTimeTest, ManualTimeIsWrittenInTheCameraZone) {
    const std::string doc = "<Time xmlns=\"urn:psialliance-org\"><timeMode>NTP</timeMode>"
                            "<localTime>2026-10-03T18:00:00</localTime><timeZone>CST-7:00:00</timeZone></Time>";
    std::string out;
    time_t camera_utc = 0;
    ASSERT_TRUE(applyManualTime(doc, 1791028800, out, camera_utc));
    EXPECT_EQ(static_cast<time_t>(1791028800 - 3600), camera_utc); // camera one hour behind
    EXPECT_NE(std::string::npos, out.find("<timeMode>manual</timeMode>"));
    EXPECT_NE(std::string::npos, out.find("<localTime>2026-10-03T19:00:00</localTime>"));
    EXPECT_NE(std::string::npos, out.find("<timeZone>CST-7:00:00</timeZone>"));
}

TEST(PsiaConnectErrorTest, OnlyAClearNonPsiaAnswerIsNotThisProtocol) {
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, classifyConnectError(404, 0, false));
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, classifyConnectError(405, 0, false));
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, classifyConnectError(200, 0, false)); // HTML login page
    EXPECT_EQ(ConnectError::AUTH_FAILED, classifyConnectError(401, 0, false));
    EXPECT_EQ(ConnectError::AUTH_FAILED, classifyConnectError(403, 0, false));
    EXPECT_EQ(ConnectError::UNREACHABLE, classifyConnectError(0, 28 /* SOAP_TCP_ERROR */, false));
    EXPECT_EQ(ConnectError::UNREACHABLE, classifyConnectError(0, -1 /* SOAP_EOF */, false));
    EXPECT_EQ(ConnectError::OTHER, classifyConnectError(500, 0, false));
    EXPECT_EQ(ConnectError::NONE, classifyConnectError(200, 0, true));
}

// The API error codes (905004/905005/905006) of a failed PSIA/ISAPI request.
TEST(PsiaConnectErrorTest, RequestFailureForTheApi) {
    EXPECT_EQ(DeviceFailure::Unauthorized, classifyFailure(401, 0));
    EXPECT_EQ(DeviceFailure::Unauthorized, classifyFailure(403, 0));
    EXPECT_EQ(DeviceFailure::Unreachable, classifyFailure(0, 28 /* SOAP_TCP_ERROR */));
    EXPECT_EQ(DeviceFailure::Timeout, classifyFailure(0, -1 /* SOAP_EOF */));
    EXPECT_EQ(DeviceFailure::Other, classifyFailure(500, 0));
    EXPECT_EQ(DeviceFailure::None, classifyFailure(200, 0));
}

// Hikvision ISAPI documents: Hikvision namespace, no manufacturer, no rtspPortNo in the channels.
TEST(IsapiParseTest, HikvisionDocuments) {
    const std::string info =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DeviceInfo version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<deviceName>Front door</deviceName><deviceID>48443830-3637</deviceID><model>DS-2CD2143G2-I</model>"
        "<serialNumber>DS-2CD2143G2-I20220101AAWRJ00000001</serialNumber><macAddress>bc:ba:c2:00:00:01</macAddress>"
        "<firmwareVersion>V5.7.3</firmwareVersion><deviceType>IPCamera</deviceType></DeviceInfo>";
    OnvifDeviceInfo out;
    ASSERT_TRUE(parseDeviceInfo(info, out, "Hikvision"));
    EXPECT_EQ("Hikvision", out.manufacturer);
    EXPECT_EQ("DS-2CD2143G2-I", out.model);
    ASSERT_TRUE(parseDeviceInfo(info, out));
    EXPECT_EQ("Front door", out.manufacturer) << "PSIA keeps the deviceName fallback";

    const std::string channels =
        "<StreamingChannelList version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<StreamingChannel><id>101</id><channelName>Camera 01</channelName><enabled>true</enabled>"
        "<Transport><maxPacketSize>1000</maxPacketSize></Transport>"
        "<Video><enabled>true</enabled><videoCodecType>H.265</videoCodecType>"
        "<videoResolutionWidth>2560</videoResolutionWidth><videoResolutionHeight>1440</videoResolutionHeight>"
        "<videoQualityControlType>VBR</videoQualityControlType><vbrUpperCap>4096</vbrUpperCap>"
        "<maxFrameRate>2000</maxFrameRate><SmartCodec><enabled>false</enabled></SmartCodec></Video>"
        "</StreamingChannel></StreamingChannelList>";
    OnvifMediaProfileMap profiles;
    ASSERT_TRUE(parseStreamingChannels(channels, "10.0.0.5", profiles, "/Streaming/Channels/", 10554));
    ASSERT_EQ(1u, profiles.size());
    EXPECT_EQ("rtsp://10.0.0.5:10554/Streaming/Channels/101", profiles[0].url);
    EXPECT_EQ("H265", profiles[0].vcodec);
    EXPECT_EQ(4096, profiles[0].bitrate);
    EXPECT_FLOAT_EQ(20.0f, profiles[0].fps);

    const std::string access =
        "<AdminAccessProtocolList xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<AdminAccessProtocol><id>1</id><enabled>true</enabled><protocol>HTTP</protocol><portNo>80</portNo></AdminAccessProtocol>"
        "<AdminAccessProtocol><id>4</id><enabled>true</enabled><protocol>RTSP</protocol><portNo>10554</portNo></AdminAccessProtocol>"
        "</AdminAccessProtocolList>";
    EXPECT_EQ(10554, parseRtspPort(access));
    EXPECT_EQ(0, parseRtspPort("<AdminAccessProtocolList><AdminAccessProtocol><protocol>RTSP</protocol>"
                               "<enabled>false</enabled><portNo>554</portNo></AdminAccessProtocol></AdminAccessProtocolList>"));
    EXPECT_EQ(0, parseRtspPort("<html/>"));

    EXPECT_EQ("H264", codecFromPsia("Smart264"));
    EXPECT_EQ("H265", codecFromPsia("Smart265"));
    EXPECT_EQ("H264", codecFromPsia("H.264+"));
}

// The read/write/PTZ/relay sequence both dialects of psia_mock.py support; `ctr` is connected.
static void exerciseMockCamera(PsiaControl &ctr) {
    auto profiles = ctr.getMediaProfilesInfo();
    ASSERT_EQ(2u, profiles.size());
    EXPECT_FALSE(profiles[0].vEncoderOptionMap.empty());

    VideoEncoderConfig config;
    ASSERT_TRUE(ctr.getVideoEncoderConfigByToken("102", config));
    config.fps = 10;
    config.bitrate = 768;
    ASSERT_TRUE(ctr.setVideoEncoderConfigByToken("102", config)) << ctr.lastError();
    VideoEncoderConfig back;
    ASSERT_TRUE(ctr.getVideoEncoderConfigByToken("102", back));
    EXPECT_TRUE(back == config);
    EXPECT_FALSE(ctr.setVideoEncoderConfigByToken("999", config));
    EXPECT_NE(std::string::npos, ctr.lastError().find("404")) << ctr.lastError();

    ASSERT_TRUE(ctr.enablePTZ());
    EXPECT_TRUE(ctr.getPTZProfile().isPresetEnable);
    EXPECT_TRUE(ctr.PTZ_ContinuousMove(0.5f, 0, 0, 1));
    EXPECT_TRUE(ctr.PTZ_Stop(true, true));
    EXPECT_TRUE(ctr.PTZ_AbsoluteMove(0, 0.5f, 1));
    float pan = 9, tilt = 9, zoom = 9;
    EXPECT_EQ(PtzMoveStatus::IDLE, ctr.PTZ_GetStatus(pan, tilt, zoom));
    EXPECT_NEAR(0, pan, 0.01);
    EXPECT_NEAR(1, zoom, 0.01);
    EXPECT_TRUE(ctr.PTZ_GotoPreset("1", 0.5f, 0.5f, 0.5f));
    EXPECT_FALSE(ctr.PTZ_GotoPreset("99", 0.5f, 0.5f, 0.5f));
    EXPECT_TRUE(ctr.PTZ_GotoHomePosition(0.5f, 0.5f, 0.5f));
    EXPECT_TRUE(ctr.Imaging_FocusMove(0.5f));
    EXPECT_TRUE(ctr.Imaging_FocusStop());
    EXPECT_TRUE(ctr.PTZ_AuxiliaryCommand("tt:Iris|open"));
    EXPECT_FALSE(ctr.PTZ_AuxiliaryCommand("tt:Wiper|On"));

    ASSERT_TRUE(ctr.enableRelayOutput());
    EXPECT_TRUE(ctr.Relay_SetOutputState("1", true));
    EXPECT_FALSE(ctr.Relay_SetOutputState("9", true));
    ctr.setCameraTimeManual();
}

// Against tests/integration/stress_vms/psia_mock.py; skipped unless PSIA_MOCK_ADDR=ip:port is set.
TEST(PsiaMockTest, ConnectReadWritePtzAndRelay) {
    const char *addr = getenv("PSIA_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "PSIA_MOCK_ADDR not set";
    }
    PsiaControl wrong(addr, "admin", "wrong-password");
    EXPECT_FALSE(wrong.connect());
    EXPECT_EQ(ConnectError::AUTH_FAILED, wrong.lastConnectError());

    PsiaControl ctr(addr, "admin", "admin123");
    ASSERT_TRUE(ctr.connect()) << ctr.lastError();
    EXPECT_TRUE(ctr.isConnected());
    EXPECT_TRUE(ctr.probeConnection());
    EXPECT_EQ("PSIA-MOCK-1", ctr.getDeviceInfo().model);
    EXPECT_EQ(ControlProtocol::PSIA, ctr.protocol());
    exerciseMockCamera(ctr);

    // A PSIA camera has nothing under /ISAPI: AUTO moves on.
    IsapiControl isapi(addr, "admin", "admin123");
    EXPECT_FALSE(isapi.connect());
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, isapi.lastConnectError()) << isapi.lastError();
}

// Against psia_mock.py --dialect isapi --rtsp-port 8554; skipped unless ISAPI_MOCK_ADDR=ip:port is set.
TEST(IsapiMockTest, ConnectReadWritePtzAndRelay) {
    const char *addr = getenv("ISAPI_MOCK_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "ISAPI_MOCK_ADDR not set";
    }
    IsapiControl wrong(addr, "admin", "wrong-password");
    EXPECT_FALSE(wrong.connect());
    EXPECT_EQ(ConnectError::AUTH_FAILED, wrong.lastConnectError());

    IsapiControl ctr(addr, "admin", "admin123");
    ASSERT_TRUE(ctr.connect()) << ctr.lastError();
    EXPECT_TRUE(ctr.probeConnection());
    EXPECT_EQ(ControlProtocol::ISAPI, ctr.protocol());
    EXPECT_EQ("Hikvision", ctr.getDeviceInfo().manufacturer);
    EXPECT_EQ("DS-2DE4425IW-DE", ctr.getDeviceInfo().model);
    auto profiles = ctr.getMediaProfilesInfo();
    ASSERT_EQ(2u, profiles.size());
    // Port from /ISAPI/Security/adminAccesses: the mock's channels do not report one.
    const std::string host(addr, strchr(addr, ':') ? strchr(addr, ':') - addr : strlen(addr));
    EXPECT_EQ("rtsp://" + host + ":8554/Streaming/Channels/101", profiles[0].url);
    exerciseMockCamera(ctr);

    PsiaControl psia(addr, "admin", "admin123");
    EXPECT_FALSE(psia.connect());
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, psia.lastConnectError()) << psia.lastError();
}

TEST(IsapiParseTest, TwoWayAudioChannel) {
    std::string id, codec;
    EXPECT_TRUE(parseTwoWayAudioChannel(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<TwoWayAudioChannelList version=\"2.0\" xmlns=\"http://www.hikvision.com/ver20/XMLSchema\">"
        "<TwoWayAudioChannel><id>1</id><enabled>false</enabled>"
        "<audioCompressionType>G.711alaw</audioCompressionType></TwoWayAudioChannel>"
        "</TwoWayAudioChannelList>", id, codec));
    EXPECT_EQ("1", id);
    EXPECT_EQ("PCMA", codec);

    EXPECT_TRUE(parseTwoWayAudioChannel(
        "<TwoWayAudioChannelList><TwoWayAudioChannel><id>2</id>"
        "<audioCompressionType>G.726</audioCompressionType></TwoWayAudioChannel></TwoWayAudioChannelList>", id, codec));
    EXPECT_EQ("2", id);
    EXPECT_EQ("G.726", codec);

    EXPECT_FALSE(parseTwoWayAudioChannel("<TwoWayAudioChannelList/>", id, codec));
    EXPECT_FALSE(parseTwoWayAudioChannel("not xml", id, codec));
}

namespace {

std::string readFile(const std::string &path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Number / boolean field of the JSON --state file the mocks write.
long long stateNumber(const std::string &json, const std::string &key) {
    std::smatch m;
    if (std::regex_search(json, m, std::regex("\"" + key + "\":\\s*(\\d+)"))) {
        return atoll(m[1].str().c_str());
    }
    return 0;
}

bool stateTrue(const std::string &json, const std::string &key) {
    return std::regex_search(json, std::regex("\"" + key + "\":\\s*true"));
}

// audio_bytes / two_way_open from the --state file of psia_mock.py.
struct MockAudioState {
    long long audio_bytes = 0;
    bool two_way_open = false;
};

MockAudioState readMockAudioState(const std::string &path) {
    const std::string json = readFile(path);
    MockAudioState state;
    state.audio_bytes = stateNumber(json, "audio_bytes");
    state.two_way_open = stateTrue(json, "two_way_open");
    return state;
}

// 8 kHz mono 16-bit WAV of `samples` samples, the same 16-sample pattern as the live tests.
std::string writeTestWav(const std::string &name, uint32_t samples) {
    const std::string path = "/tmp/" + name + ".wav";
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
    out.write("RIFF", 4); u32(36 + samples * 2); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    out.write("data", 4); u32(samples * 2);
    for (uint32_t k = 0; k < samples; ++k) {
        u16((uint16_t)(int16_t)((k % 16) * 1000 - 8000));
    }
    return path;
}

// 1 s of PCM (50 frames of 20 ms at 8 kHz) into a live session, paced like a client.
void pushOneSecondLive(const std::string &device_id) {
    std::vector<int16_t> frame(160);
    for (size_t k = 0; k < frame.size(); ++k) {
        frame[k] = (int16_t)((k % 16) * 1000 - 8000);
    }
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(mediakit::BackchannelManager::Instance().pushAudioFrame(device_id, frame.data(), frame.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

template <typename Pred>
bool waitFor(Pred pred, int timeout_ms = 5000) {
    for (int waited = 0; waited < timeout_ms; waited += 50) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

} // namespace

// BackchannelManager → IsapiTwoWayAudioPusher → psia_mock.py --dialect isapi --state PATH. Skipped
// unless ISAPI_MOCK_ADDR=ip:port and ISAPI_MOCK_STATE=PATH are set.
TEST(IsapiMockTest, TwoWayAudioSendsG711AndCloses) {
    const char *addr = getenv("ISAPI_MOCK_ADDR");
    const char *state_path = getenv("ISAPI_MOCK_STATE");
    if (!addr || !*addr || !state_path || !*state_path) {
        GTEST_SKIP() << "ISAPI_MOCK_ADDR / ISAPI_MOCK_STATE not set";
    }
    using namespace mediakit;

    IsapiControl ctr(addr, "admin", "admin123");
    ASSERT_TRUE(ctr.connect()) << ctr.lastError();
    ASSERT_TRUE(ctr.supportsAudioBackchannel()) << ctr.lastError();
    AudioBackchannelInfo info;
    ASSERT_TRUE(ctr.setupAudioBackchannel(info, false));
    EXPECT_EQ(AudioBackchannelInfo::HTTP_ISAPI, info.transport);
    EXPECT_EQ(std::string("http://") + addr + "/ISAPI/System/TwoWayAudio/channels/1", info.url);
    EXPECT_EQ("PCMU", info.codec);
    info.username = "admin";
    info.password = "admin123";

    ASSERT_FALSE(readMockAudioState(state_path).two_way_open) << "another two-way session is open on the mock";
    const long long bytes_before = readMockAudioState(state_path).audio_bytes;

    const std::string device_id = "isapi_two_way_test";
    auto &manager = BackchannelManager::Instance();
    manager.registerDevice(device_id, info);
    ASSERT_EQ(StartResult::Started, manager.startLiveSession(device_id, 8000, 1));
    ASSERT_TRUE(waitFor([&] { return readMockAudioState(state_path).two_way_open; })) << "open never reached the mock";

    // A second session while the first holds the device: deviceBusy.
    auto second = std::make_shared<IsapiTwoWayAudioPusher>(info);
    std::promise<int> second_failed;
    second->onFailed = [&second_failed](const toolkit::SockException &ex) { second_failed.set_value(ex.getCustomCode()); };
    second->start();
    auto second_result = second_failed.get_future();
    ASSERT_EQ(std::future_status::ready, second_result.wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(IsapiTwoWayAudioPusher::kDeviceBusyCode, second_result.get());
    EXPECT_TRUE(readMockAudioState(state_path).two_way_open) << "the refused session must not close the first";

    // Give the audioData request time to be accepted (the ring reader attaches then), then 1 s of audio.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pushOneSecondLive(device_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    manager.stopSession(device_id);
    manager.unregisterDevice(device_id);

    ASSERT_TRUE(waitFor([&] { return !readMockAudioState(state_path).two_way_open; })) << "close never reached the mock";
    // G.711: one byte per 8 kHz sample.
    EXPECT_TRUE(waitFor([&] { return readMockAudioState(state_path).audio_bytes - bytes_before == 8000; }))
        << "mock got " << readMockAudioState(state_path).audio_bytes - bytes_before << " bytes";
}

// BackchannelManager::playFile → IsapiTwoWayAudioPusher: the file plays to its end and close follows on its own.
TEST(IsapiMockTest, TwoWayAudioPlaysWholeFile) {
    const char *addr = getenv("ISAPI_MOCK_ADDR");
    const char *state_path = getenv("ISAPI_MOCK_STATE");
    if (!addr || !*addr || !state_path || !*state_path) {
        GTEST_SKIP() << "ISAPI_MOCK_ADDR / ISAPI_MOCK_STATE not set";
    }
    using namespace mediakit;
    AudioBackchannelInfo info;
    info.transport = AudioBackchannelInfo::HTTP_ISAPI;
    info.url = std::string("http://") + addr + "/ISAPI/System/TwoWayAudio/channels/1";
    info.username = "admin";
    info.password = "admin123";
    info.codec = "PCMU";
    ASSERT_FALSE(readMockAudioState(state_path).two_way_open) << "another two-way session is open on the mock";
    const long long bytes_before = readMockAudioState(state_path).audio_bytes;

    const std::string device_id = "isapi_two_way_file_test";
    auto &manager = BackchannelManager::Instance();
    manager.registerDevice(device_id, info);
    ASSERT_TRUE(manager.playFile(device_id, writeTestWav(device_id, 8000))) << manager.getErrorMsg();
    ASSERT_TRUE(waitFor([&] { return readMockAudioState(state_path).two_way_open; })) << "open never reached the mock";
    // 1 s of audio, then FileAudioSource finishes and the session closes by itself.
    ASSERT_TRUE(waitFor([&] { return !readMockAudioState(state_path).two_way_open; }, 8000)) << "close never reached the mock";
    manager.unregisterDevice(device_id);
    const long long got = readMockAudioState(state_path).audio_bytes - bytes_before;
    // Frames produced before the device accepted audioData are dropped, as with the RTSP pusher.
    EXPECT_GE(got, 7000) << "mock got " << got << " bytes";
    EXPECT_LE(got, 8000) << "mock got " << got << " bytes";
}

// BackchannelManager → BackchannelPusher (ONVIF RTSP backchannel) → rtsp_backchannel_mock.py --state PATH.
// Skipped unless RTSP_BC_MOCK_ADDR=ip:port and RTSP_BC_MOCK_STATE=PATH are set.
class RtspBackchannelMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char *addr = getenv("RTSP_BC_MOCK_ADDR");
        const char *state = getenv("RTSP_BC_MOCK_STATE");
        if (!addr || !*addr || !state || !*state) {
            GTEST_SKIP() << "RTSP_BC_MOCK_ADDR / RTSP_BC_MOCK_STATE not set";
        }
        url = std::string("rtsp://admin:admin123@") + addr + "/onvif/backchannel";
        state_path = state;
        ASSERT_FALSE(stateTrue(readFile(state_path), "session_open")) << "another session is open on the mock";
        before = readFile(state_path);
    }

    long long delta(const std::string &key) const {
        return stateNumber(readFile(state_path), key) - stateNumber(before, key);
    }
    bool sessionOpen() const { return stateTrue(readFile(state_path), "session_open"); }

    std::string url;
    std::string state_path;
    std::string before;
};

TEST_F(RtspBackchannelMockTest, LiveSendsG711AndTearsDown) {
    using namespace mediakit;
    const std::string device_id = "rtsp_backchannel_live_test";
    auto &manager = BackchannelManager::Instance();
    // The AudioBackchannelInfo form SpeakerController registers since S4.
    AudioBackchannelInfo info;
    info.transport = AudioBackchannelInfo::RTSP_ONVIF;
    info.url = url;
    manager.registerDevice(device_id, info);
    ASSERT_EQ(StartResult::Started, manager.startLiveSession(device_id, 8000, 1));
    ASSERT_TRUE(waitFor([&] { return sessionOpen(); })) << "PLAY never reached the mock";
    // A second live session on the same device while the first holds it: the ownership check refuses it.
    EXPECT_EQ(StartResult::DeviceBusy, manager.startLiveSession(device_id, 8000, 1));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pushOneSecondLive(device_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    manager.stopSession(device_id);
    manager.unregisterDevice(device_id);

    ASSERT_TRUE(waitFor([&] { return !sessionOpen() && delta("teardowns") == 1; })) << "TEARDOWN never reached the mock";
    // One RTP packet of 160 G.711 bytes per 20 ms frame, sequence numbers without gaps.
    EXPECT_EQ(50, delta("rtp_packets"));
    EXPECT_EQ(8000, delta("rtp_bytes"));
    EXPECT_EQ(0, delta("seq_gaps"));
    EXPECT_NE(std::string::npos, readFile(state_path).find("\"payload_types\": [\n  0\n ]"));
}

TEST_F(RtspBackchannelMockTest, PlayFileSendsWholeFile) {
    using namespace mediakit;
    const std::string device_id = "rtsp_backchannel_file_test";
    auto &manager = BackchannelManager::Instance();
    // The plain-URL form used before S4.
    manager.registerDevice(device_id, url);
    ASSERT_TRUE(manager.playFile(device_id, writeTestWav(device_id, 8000))) << manager.getErrorMsg();
    ASSERT_TRUE(waitFor([&] { return sessionOpen(); })) << "PLAY never reached the mock";
    // The file ends after 1 s and the session tears down by itself.
    ASSERT_TRUE(waitFor([&] { return !sessionOpen() && delta("teardowns") == 1; }, 8000)) << "TEARDOWN never reached the mock";
    manager.unregisterDevice(device_id);
    const long long got = delta("rtp_bytes");
    EXPECT_GE(got, 7000) << "mock got " << got << " bytes";
    EXPECT_LE(got, 8000) << "mock got " << got << " bytes";
    EXPECT_EQ(0, got % 160);
    EXPECT_EQ(0, delta("seq_gaps"));
}

// A web server that is not PSIA (here: nothing under /PSIA) is NOT_THIS_PROTOCOL, so AUTO moves on.
TEST(PsiaMockTest, NonPsiaServerIsNotThisProtocol) {
    const char *addr = getenv("PSIA_NOT_PSIA_ADDR");
    if (!addr || !*addr) {
        GTEST_SKIP() << "PSIA_NOT_PSIA_ADDR not set";
    }
    PsiaControl ctr(addr, "admin", "admin123");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(ConnectError::NOT_THIS_PROTOCOL, ctr.lastConnectError()) << ctr.lastError();
}

TEST(PsiaMockTest, ClosedPortIsUnreachable) {
    ASSERT_NE(nullptr, kPullsGsoapNamespaces);
    PsiaControl ctr("127.0.0.1:1", "admin", "admin123");
    EXPECT_FALSE(ctr.connect());
    EXPECT_EQ(ConnectError::UNREACHABLE, ctr.lastConnectError()) << ctr.lastError();
}
