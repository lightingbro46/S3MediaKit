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
    bool mediaPort = 0;

    // whether to use other media port
    bool autoMediaPort = true;

    enum { 
        kRtpTransportAuto = 0, // System automatical choose rtp transport method to order to get media source by rtsp protocol, default tcp mode
        kRtpTransportTcp = 1, // System choose tcp protocol in order to get media source by rtsp protocol
        kRtpTransportUdp = 2 // System choose udp protocol to order to get media source by rtsp protocol
    };
    // rtp transport method when using rtsp protocol
    int rtpTransport = kRtpTransportAuto;

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

    GenericRtspCamera(const CameraInfo &info) : DeviceSource(CAMERA_SCHEMA, info), _info(std::move(info)) {}

    const CameraInfo getCameraInfo() const { return _info; }

private:
    CameraInfo _info;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERA_H