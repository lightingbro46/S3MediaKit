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

inline bool equalCameraInfo(const CameraInfo &a, const CameraInfo &b) {
    #define EQUAL_INFO_PROPERTY(name) if (a.name != b.name) return false;
    EQUAL_INFO_PROPERTY(ip)
    EQUAL_INFO_PROPERTY(port)
    EQUAL_INFO_PROPERTY(username)
    EQUAL_INFO_PROPERTY(password)
    EQUAL_INFO_PROPERTY(manufacturer)
    EQUAL_INFO_PROPERTY(model)

    return true;
}

class CameraOption {
public:
    CameraOption() {
        // todo: load default value from database
    }

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

inline bool equalCameraOption(const CameraOption &a, const CameraOption& b) {
    #define EQUAL_OPTION_PROPERTY(name) if (a.name != b.name) return false;
    EQUAL_OPTION_PROPERTY(doNotRecordPrimaryStream)
    EQUAL_OPTION_PROPERTY(doNotRecordSecondaryStream)
    EQUAL_OPTION_PROPERTY(enableRecord)
    EQUAL_OPTION_PROPERTY(keepArchivedMinForAuto)
    EQUAL_OPTION_PROPERTY(keepArchivedMinFor)
    EQUAL_OPTION_PROPERTY(keepArchivedMaxForAuto)
    EQUAL_OPTION_PROPERTY(keepArchivedMaxFor)
    EQUAL_OPTION_PROPERTY(enableActive)
    EQUAL_OPTION_PROPERTY(mediaPort)
    EQUAL_OPTION_PROPERTY(autoMediaPort)
    EQUAL_OPTION_PROPERTY(rtpTransport)
    EQUAL_OPTION_PROPERTY(recordScheduler)
    EQUAL_OPTION_PROPERTY(enableFailover)
    EQUAL_OPTION_PROPERTY(preferedMediaServer)
    EQUAL_OPTION_PROPERTY(enablePTZControl)

    return true;
}

/**
 * Data abstraction of generic camera source
 * Camera has two key elements, info and streams
 * As long as these two elements are generated, it is very simple to implement rtsp pull stream and onvif controller
 */
class GenericRtspCamera : public DeviceSource {
public:
    using Ptr = std::shared_ptr<GenericRtspCamera>;

    GenericRtspCamera(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map)
        : DeviceSource(CAMERA_SCHEMA, info), _info(info), _stream_map(stream_map) {}

    const CameraInfo& getCameraInfo() const { return _info; }

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

protected:
    CameraInfo _info;
    std::unordered_map<int, StreamTuple> _stream_map;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERA_H