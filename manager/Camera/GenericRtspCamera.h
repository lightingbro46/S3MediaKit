#ifndef CAMERA_GENERICRTSPCAMERA_H
#define CAMERA_GENERICRTSPCAMERA_H

#include "Common/DeviceSource.h"
#include "StreamSource.h"

namespace managerkit {

struct CameraInfo : public DeviceTuple {
    std::string manufacturer;
    std::string model;
    std::string ip;
    int port = 0;
    std::string username;
    std::string password;
};

bool equalCameraInfo(const CameraInfo &a, const CameraInfo &b);

class CameraOption {
public:
    CameraOption();

    // whether to disable primary secondary stream
    bool doNotRecordPrimaryStream = false;

    // whether to disable recording secondary stream
    bool doNotRecordSecondaryStream = false;

    // whether to enable recording
    bool enableRecord = false;

    // whether to enable automatic choose minium value of block retention
    bool keepArchivedMinForAuto = true;

    // keep minimum time block from being deleted
    uint64_t keepArchivedMinFor = 0;

    // whether to enable automatic choose maximum value of block retention
    bool keepArchivedMaxForAuto = true;

    // keep time block from exceeding threshold
    uint64_t keepArchivedMaxFor = 0;

    // whether to enable camera
    bool enableActive = false;

    // use other media port instead of defaunt port of stream url
    int mediaPort = 0;

    // whether to use other media port
    bool autoMediaPort = true;

    enum { 
        kRtpTransportAuto = 0, // System automatical choose rtp transport method to order to get media source by rtsp protocol, default tcp mode
        kRtpTransportTcp = 1, // System choose tcp protocol in order to get media source by rtsp protocol
        kRtpTransportUdp = 2 // System choose udp protocol to order to get media source by rtsp protocol
    };
    // rtp transport method when using rtsp protocol
    int rtpTransport = kRtpTransportAuto;

    // recording scheduler, include 168 characters
    std::string recordScheduler;

    // whether to enable failover mode
    bool enableFailover;

    // media server id, which camera belong to
    std::string preferedMediaServer;

    // media server id, which camera belong to
    bool enablePTZControl;

    // Add more options if need
};

bool equalCameraOption(const CameraOption &a, const CameraOption &b);

/**
 * Data abstraction of generic camera source
 * Camera has two key elements, info and streams
 * As long as these two elements are generated, it is very simple to implement rtsp pull stream and onvif controller
 */
class GenericRtspCamera : public DeviceSource {
public:
    using Ptr = std::shared_ptr<GenericRtspCamera>;

    GenericRtspCamera(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map) : DeviceSource(CAMERA_SCHEMA, info), _info(std::move(info)), _stream_map(std::move(stream_map)) {}

    const CameraInfo getCameraInfo() const { return _info; }

    bool hasStreamTuple(int type) const {
        return _stream_map.find(type) != _stream_map.end() && !_stream_map.at(type).empty();
    }
 
    StreamTuple getStreamTuple(int type) const {
        auto it = _stream_map.find(type);
        if (it == _stream_map.end()) {
            throw std::runtime_error("No stream at index " + std::to_string(type));
        }
        return it->second;
    }

    const CameraOption &getCameraOption() const { return _option; }

    bool setCameraOption(const CameraOption &option) {
        if (equalCameraOption(_option, option)) {
            return false;
        }
        _option = option; 
        return true;
    }

private:
    CameraInfo _info;
    CameraOption _option;
    std::unordered_map<int, StreamTuple> _stream_map;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERA_H