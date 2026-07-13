#include <gtest/gtest.h>

#include "Common/DeviceSource.h"
#include "Common/config.h"

using namespace managerkit;

namespace {

class TestDeviceSource : public DeviceSource {
public:
    TestDeviceSource(const std::string &schema, const DeviceTuple &tuple)
        : DeviceSource(schema, tuple) {}
    void registerDevice() { regist(); }
};

class CapturingDeviceListener : public DeviceSourceEvent {
public:
    DeviceOriginType origin = DeviceOriginType::api_service;
    std::string origin_url = "api://device";
    size_t registered = 0;
    size_t unregistered = 0;
    size_t record_changes = 0;
    size_t quality_changes = 0;
    size_t stream_changes = 0;
    size_t controller_changes = 0;
    toolkit::EventPoller::Ptr poller = toolkit::EventPollerPool::Instance().getPoller();

    DeviceOriginType getOriginType(DeviceSource &) const override { return origin; }
    std::string getOriginUrl(DeviceSource &) const override { return origin_url; }
    void onRegist(DeviceSource &, bool value) override {
        value ? ++registered : ++unregistered;
    }
    void onRecordModeChange(DeviceSource &, int, bool) override { ++record_changes; }
    void onImageQualityChange(DeviceSource &, int, int) override { ++quality_changes; }
    void onStreamReady(DeviceSource &, int, bool, const std::string &, const toolkit::Any &) override {
        ++stream_changes;
    }
    void onControllerReady(DeviceSource &, bool, const std::string &, const toolkit::Any &) override {
        ++controller_changes;
    }
    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &) override { return poller; }
};

} // namespace

TEST(DeviceSourceTest, ExposesDefaultsAndDelegatesOriginInformation) {
    auto source = std::make_shared<TestDeviceSource>("unit-device", DeviceTuple{"tenant", "camera", "Front"});
    EXPECT_EQ("unit-device", source->getSchema());
    EXPECT_EQ(DEFAULT_VHOST, source->getDeviceTuple().vhost);
    EXPECT_EQ("camera", source->getDeviceTuple().device_id);
    EXPECT_EQ(std::string("unit-device://") + DEFAULT_VHOST + "/camera", source->getUrl());
    EXPECT_EQ(DeviceOriginType::unknown, source->getOriginType());
    EXPECT_EQ(source->getUrl(), source->getOriginUrl());
    EXPECT_THROW(source->getOwnerPoller(), std::runtime_error);

    auto listener = std::make_shared<CapturingDeviceListener>();
    source->setListener(listener);
    EXPECT_EQ(listener, source->getListener().lock());
    EXPECT_EQ(DeviceOriginType::api_service, source->getOriginType());
    EXPECT_EQ("api://device", source->getOriginUrl());
    EXPECT_EQ(listener->poller, source->getOwnerPoller());
    listener->origin_url.clear();
    EXPECT_EQ(source->getUrl(), source->getOriginUrl());
}

TEST(DeviceSourceTest, RegistersFindsTraversesAndRejectsDuplicates) {
    auto listener = std::make_shared<CapturingDeviceListener>();
    auto source = std::make_shared<TestDeviceSource>(GENERIC_RTSP_CAMERA_SCHEMA,
                                                      DeviceTuple{"tenant", "registry-camera", "Camera"});
    source->setListener(listener);
    source->registerDevice();
    EXPECT_EQ(1U, listener->registered);
    EXPECT_EQ(source, DeviceSource::find(GENERIC_RTSP_CAMERA_SCHEMA, DEFAULT_VHOST, "registry-camera"));
    EXPECT_EQ(source, DeviceSource::find(DEFAULT_VHOST, "registry-camera"));
    EXPECT_FALSE(DeviceSource::find(GENERIC_RTSP_CAMERA_SCHEMA, DEFAULT_VHOST, ""));

    size_t exact = 0;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &item) {
        EXPECT_EQ(source, item);
        ++exact;
    }, GENERIC_RTSP_CAMERA_SCHEMA, DEFAULT_VHOST, "registry-camera");
    EXPECT_EQ(1U, exact);

    size_t wildcard = 0;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &) { ++wildcard; });
    EXPECT_GE(wildcard, 1U);

    auto duplicate = std::make_shared<TestDeviceSource>(GENERIC_RTSP_CAMERA_SCHEMA,
                                                         DeviceTuple{"tenant", "registry-camera", "Duplicate"});
    EXPECT_THROW(duplicate->registerDevice(), std::invalid_argument);
    source->registerDevice();
    source.reset();
    EXPECT_FALSE(DeviceSource::find(GENERIC_RTSP_CAMERA_SCHEMA, DEFAULT_VHOST, "registry-camera"));
    EXPECT_EQ(1U, listener->unregistered);
}

TEST(DeviceSourceTest, GrantsExclusiveOwnershipUntilReleased) {
    auto source = std::make_shared<TestDeviceSource>("unit", DeviceTuple{"v", "owned", "Owned"});
    auto token = source->getOwnership();
    ASSERT_TRUE(token);
    EXPECT_FALSE(source->getOwnership());
    token.reset();
    EXPECT_TRUE(source->getOwnership());
    EXPECT_GE(source->getCreateStamp(), 1U);
    EXPECT_EQ(0U, source->getAliveSecond());
}

TEST(DeviceSourceEventInterceptorTest, DelegatesEveryEventAndFallsBack) {
    auto source = std::make_shared<TestDeviceSource>("unit", DeviceTuple{"v", "events", "Events"});
    DeviceSourceEventInterceptor interceptor;
    EXPECT_FALSE(interceptor.getDelegate());
    interceptor.onRegist(*source, true);
    interceptor.onRecordModeChange(*source, 1, true);
    interceptor.onImageQualityChange(*source, 15, 2);
    interceptor.onStreamReady(*source, 0, true, "ok", toolkit::Any{});
    interceptor.onControllerReady(*source, true, "ok", toolkit::Any{});
    EXPECT_THROW(interceptor.getOwnerPoller(*source), DeviceSourceEvent::NotImplemented);

    auto listener = std::make_shared<CapturingDeviceListener>();
    interceptor.setDelegate(listener);
    EXPECT_EQ(listener, interceptor.getDelegate());
    interceptor.onRegist(*source, true);
    interceptor.onRegist(*source, false);
    interceptor.onRecordModeChange(*source, 1, true);
    interceptor.onImageQualityChange(*source, 12, 2);
    interceptor.onStreamReady(*source, 1, true, "ready", toolkit::Any{});
    interceptor.onControllerReady(*source, true, "ready", toolkit::Any{});
    EXPECT_EQ(listener->poller, interceptor.getOwnerPoller(*source));
    EXPECT_EQ(1U, listener->registered);
    EXPECT_EQ(1U, listener->unregistered);
    EXPECT_EQ(1U, listener->record_changes);
    EXPECT_EQ(1U, listener->quality_changes);
    EXPECT_EQ(1U, listener->stream_changes);
    EXPECT_EQ(1U, listener->controller_changes);
    listener.reset();
    EXPECT_FALSE(interceptor.getDelegate());
}

TEST(DeviceSourceHelpersTest, ConvertsOriginAndComparesIdentity) {
    EXPECT_EQ("unknown", getOriginTypeString(DeviceOriginType::unknown));
    EXPECT_EQ("api_service", getOriginTypeString(DeviceOriginType::api_service));
    EXPECT_EQ("mobile_device", getOriginTypeString(DeviceOriginType::mobile_device));
    EXPECT_EQ("unknown", getOriginTypeString(static_cast<DeviceOriginType>(99)));
    EXPECT_TRUE(equalDeviceTuple(DeviceTuple{"v", "id", "one"}, DeviceTuple{"v", "id", "two"}));
    EXPECT_FALSE(equalDeviceTuple(DeviceTuple{"v", "one", ""}, DeviceTuple{"v", "two", ""}));
    EXPECT_EQ("schema://__defaultVhost__/device_id", DeviceSource::NullDeviceSource().getUrl());
}
