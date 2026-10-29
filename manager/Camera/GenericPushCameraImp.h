#ifndef CAMERA_GENERICPUSHCAMERAIMP_H
#define CAMERA_GENERICPUSHCAMERAIMP_H

#include <atomic>
#include "GenericPushCamera.h"
#include "PushCameraStatistic.h"

namespace managerkit {

// Runtime of a push camera, as GenericRtspCameraImp is for an RTSP camera. The push ingest is not
// implemented yet: it will report its streams through onStreamReady() of the registered source.
class GenericPushCameraImp : public DeviceSourceEventInterceptor, public std::enable_shared_from_this<GenericPushCameraImp> {
public:
    using Ptr = std::shared_ptr<GenericPushCameraImp>;

    // Writes the tuple and streams to the statistic (blocking).
    GenericPushCameraImp(const DeviceTuple &tuple, const std::unordered_map<int, StreamTuple> &stream_map, const PushCameraStatisticImp::Ptr &statistic);

    bool isEnabled() const { return _enabled.load(); }

    GenericPushCamera::Ptr getCameraSource() const { return _src; }

    // Owner poller only.
    void setPushCameraOption(const PushCameraOption &option);

    // Owner poller only.
    const PushCameraOption &getPushCameraOption() const { return _option; }

    PushCameraStatisticImp::Ptr getPushCameraStatisticImp();

public:
    //////////////DeviceSourceEvent related interface implementation////////////////
    DeviceOriginType getOriginType(DeviceSource &sender) const override { return static_cast<DeviceOriginType>(_origin_type.load()); }

    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) override { return _poller; }

    void onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) override;

private:
    toolkit::EventPoller::Ptr _poller;
    std::atomic<bool> _enabled { false };
    // getOriginType() is called from any thread, _option only on the owner poller.
    std::atomic<uint8_t> _origin_type { static_cast<uint8_t>(DeviceOriginType::unknown) };
    bool _registered = false;
    GenericPushCamera::Ptr _src;
    PushCameraOption _option;
    std::weak_ptr<PushCameraStatisticImp> _statistic;
};

} // namespace managerkit

#endif // CAMERA_GENERICPUSHCAMERAIMP_H
