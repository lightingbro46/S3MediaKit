#include "GenericPushCameraImp.h"
#include "Player/PlayerProxy.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericPushCameraImp::GenericPushCameraImp(const DeviceTuple &tuple, const std::unordered_map<int, StreamTuple> &stream_map, const PushCameraStatisticImp::Ptr &statistic)
    : _statistic(statistic) {
    _poller = DeviceSource::nextDevicePoller();
    CHECK(_poller != nullptr, "Failed to get poller for GenericPushCameraImp");
    _src = std::make_shared<GenericPushCamera>(tuple, stream_map);
    statistic->setDeviceTuple(tuple);
    statistic->setStreamTuples(stream_map);
}

void GenericPushCameraImp::setPushCameraOption(const PushCameraOption &option) {
    CHECK(_poller->isCurrentThread(), "Can only call setPushCameraOption in it's owner poller");
    if (_option == option) {
        return; // No change
    }
    _option = option;
    _enabled = option.enableActive;
    _origin_type = static_cast<uint8_t>(option.originType);
    auto strong_statistic = _statistic.lock();
    if (strong_statistic) {
        strong_statistic->setPushCameraOption(option);
    } else {
        WarnL << "Push camera " << _src->getUrl() << " statistic has been released. Ignore option save";
    }
    if (_enabled.load() && !_registered) {
        _registered = true;
        // set this as listener to handle device events, then trigger device registration event
        _src->setListener(shared_from_this());
        _src->regist();
    }
}

PushCameraStatisticImp::Ptr GenericPushCameraImp::getPushCameraStatisticImp() {
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Push camera " << _src->getUrl() << " statistic has been released. Ignore get statistic request";
    }
    return strong_statistic;
}

void GenericPushCameraImp::onStreamReady(DeviceSource &sender, int type, bool live, const std::string &status, const toolkit::Any &data) {
    if (!isValidStreamType(type)) {
        WarnL << "Invalid stream type in onStreamReady: " << type;
        return;
    }
    auto strong_statistic = _statistic.lock();
    if (!strong_statistic) {
        WarnL << "Push camera " << _src->getUrl() << " statistic has been released. Ignore stream ready event";
        return;
    }
    const TranslationInfo *info = data.is<TranslationInfo>() ? &data.get<TranslationInfo>() : nullptr;
    strong_statistic->addStreamStatistic(type, live, status, info);
}

} // namespace managerkit
