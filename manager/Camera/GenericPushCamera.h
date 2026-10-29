#ifndef CAMERA_GENERICPUSHCAMERA_H
#define CAMERA_GENERICPUSHCAMERA_H

#include <stdexcept>
#include <string>
#include <unordered_map>
#include "Common/DeviceSource.h"
#include "Common/DeviceOption.h"
#include "StreamSource.h"

namespace managerkit {

struct PushCameraOption : public RecordingOption {
    // mobile_device, flying_device, ...; saved as getOriginTypeString()
    DeviceOriginType originType = DeviceOriginType::unknown;

    // whether to enable camera
    bool enableActive = false;

    bool operator==(const PushCameraOption &other) const {
        return name == other.name &&
               enableRecord == other.enableRecord &&
               recordRootPath == other.recordRootPath &&
               keepArchivedMinForAuto == other.keepArchivedMinForAuto &&
               keepArchivedMinFor == other.keepArchivedMinFor &&
               keepArchivedMaxForAuto == other.keepArchivedMaxForAuto &&
               keepArchivedMaxFor == other.keepArchivedMaxFor &&
               recordSchedules == other.recordSchedules &&
               originType == other.originType &&
               enableActive == other.enableActive;
    }

    bool operator!=(const PushCameraOption &other) const {
        return !(*this == other);
    }
};

class GenericPushCameraImp;

/**
 * Camera pushing its stream to the server (drone, mobile video push): a device tuple and the streams
 * it publishes, no controller.
 */
class GenericPushCamera : public DeviceSource {
public:
    friend class GenericPushCameraImp;
    using Ptr = std::shared_ptr<GenericPushCamera>;

    GenericPushCamera(const DeviceTuple &tuple, const std::unordered_map<int, StreamTuple> &stream_map)
        : DeviceSource(GENERIC_PUSH_CAMERA_SCHEMA, tuple), _stream_map(stream_map) {}

    bool hasStreamTuple(int type) const {
        auto it = _stream_map.find(type);
        return it != _stream_map.end() && !it->second.empty();
    }

    const StreamTuple& getStreamTuple(int type) const {
        auto it = _stream_map.find(type);
        if (it == _stream_map.end()) {
            throw std::runtime_error("No stream at index " + std::to_string(type));
        }
        return it->second;
    }

    const std::unordered_map<int, StreamTuple> &getStreamMap() const { return _stream_map; }

protected:
    std::unordered_map<int, StreamTuple> _stream_map;
};

} // namespace managerkit

#endif // CAMERA_GENERICPUSHCAMERA_H
