#ifndef CAMERA_GENERICRTSPCAMERA_H
#define CAMERA_GENERICRTSPCAMERA_H

#include "Common/DeviceSource.h"
#include "StreamSource.h"

#define GENERIC_RTSP_CAMERA "GENERIC-RTSP"

namespace managerkit {

class CameraOption {
public:
    CameraOption() {
        // todo: load default value from database
    }

    // camera name 
    std::string name;

    // camera manufacturer
    std::string manufacturer;

    // camera model
    std::string model;

    // camera IP address or domain name
    std::string ip;

    // camera http port
    int port = 0;

    // camera credential username
    std::string username;

    // camera credential password
    std::string password;

    // whether to disable primary stream
    bool disablePrimaryStream = false;

    // whether to disable secondary stream
    bool disableSecondaryStream = false;

    // whether to disable recording primary stream
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

    // use other media port instead of default port of stream url
    int mediaPort = 0;

    // whether to use other media port
    bool autoMediaPort = true;

    enum { 
        kRtpTransportAuto = 0, // System automatical choose rtp transport method to order to get media source by rtsp protocol, default tcp mode
        kRtpTransportTcp = 1, // System choose tcp protocol in order to get media source by rtsp protocol
        kRtpTransportUdp = 2, // System choose udp protocol to order to get media source by rtsp protocol
        kRtpTransportMultiCast = 3 // System choose multicast protocol to order to get media source by rtsp protocol
    };
    // rtp transport method when using rtsp protocol
    int rtpTransport = kRtpTransportAuto;

    // whether to enable failover mode
    bool enableFailover = false;

    // media server id, which camera belong to
    std::string preferedMediaServer;

    // pre-record seconds for motion detection
    int motionPreRecordSec = 0;

    // post-record seconds for motion detection
    int motionPostRecordSec = 0;

    // recording schedules, include 168 elements for one week
    std::string recordSchedules;

    // whether to disable audio
    bool disableAudio = false;

    // web port for camera web access, use if useDefaultWebPort is false
    int webPort = 0;

    // whether to use default web port 80
    bool autoWebPort = true;

    // keep config of profile and stream changed from camera web page
    bool keepConfigProfileAndStream = false;

    // whether to enable ptz control, default true, permit ptz operation only when this option is true
    bool enablePTZControl = true;

    // whether to reserve pan axis when ptz operation
    bool reversePanAxis = false;

    // whether to reserve tilt axis when ptz operation
    bool reverseTiltAxis = false;

    enum { 
        kPTZModeAuto = 0, // System automatical choose ptz control mode, default use absoluted mode or relative mode or continous based on camera capability
        kPTZAbsolutedMode = 1, // System choose choose ptz control with absoluted mode
        kPTZRelativeMode = 2, // System choose choose ptz control with relative mode
        kPTZContinousMode = 3 // System choose choose ptz control with continous mode
    };
    // ptz mode for ptz control
    int ptzMode = kPTZModeAuto;

    // ptz speed for ptz control, range [0,1]
    float ptzSpeed = 0.5;

    // onvif main profile token, such as "AUTO" or "Profile token 1"
    std::string onvifMainProfile; 
    
    // onvif sub profile token, such as "AUTO" or "Profile token 2"
    std::string onvifSubProfile; 

    // Enable motion detection, only for video streams
    bool enableMotion = false;

    // The level value of each grid in roi, the value range is 0-5, 0 means no motion detection, 5 means the most sensitive, the default value is 3
    std::string roiValue;

    // which stream to enable motion detection, default secondary stream
    int motionDetectOnStream = StreamType::SecondaryStream; 

    // whether to trigger stream status change event when stream source is ready or failed to pull stream, default false
    // use for some scenarios that need to trigger recording or other action when stream is ready such as video push stream from mobile device
    bool emitStreamStatusChangeEvent = true;

    // Note: Add more options if needed and implement operator== to compare whether two options are equal

    bool operator==(const CameraOption& other) const{
        return name == other.name &&
               manufacturer == other.manufacturer &&
               model == other.model &&
               ip == other.ip &&
               port == other.port &&
               username == other.username &&
               password == other.password &&
               disablePrimaryStream == other.disablePrimaryStream &&
               disableSecondaryStream == other.disableSecondaryStream &&
               doNotRecordPrimaryStream == other.doNotRecordPrimaryStream &&
               doNotRecordSecondaryStream == other.doNotRecordSecondaryStream &&
               enableRecord == other.enableRecord &&
               keepArchivedMinForAuto == other.keepArchivedMinForAuto &&
               keepArchivedMinFor == other.keepArchivedMinFor &&
               keepArchivedMaxForAuto == other.keepArchivedMaxForAuto &&
               keepArchivedMaxFor == other.keepArchivedMaxFor &&
               enableActive == other.enableActive &&
               mediaPort == other.mediaPort &&
               autoMediaPort == other.autoMediaPort &&
               rtpTransport == other.rtpTransport &&
               enableFailover == other.enableFailover &&
               preferedMediaServer == other.preferedMediaServer &&
               enablePTZControl == other.enablePTZControl &&
               motionPreRecordSec == other.motionPreRecordSec &&
               motionPostRecordSec == other.motionPostRecordSec &&
               recordSchedules == other.recordSchedules &&
               disableAudio == other.disableAudio &&
               webPort == other.webPort &&
               autoWebPort == other.autoWebPort &&
               keepConfigProfileAndStream == other.keepConfigProfileAndStream &&
               ptzMode == other.ptzMode &&
               ptzSpeed == other.ptzSpeed &&
               reversePanAxis == other.reversePanAxis &&
               reverseTiltAxis == other.reverseTiltAxis &&
               onvifMainProfile == other.onvifMainProfile &&
               onvifSubProfile == other.onvifSubProfile && 
               enableMotion == other.enableMotion &&
               roiValue == other.roiValue &&
               motionDetectOnStream == other.motionDetectOnStream;
    }

    bool operator!=(const CameraOption& other) const {
        return !(*this == other);
    }
};

class GenericRtspCameraImp;

/**
 * Data abstraction of generic camera source
 * Camera has two key elements, info and streams
 * As long as these two elements are generated, it is very simple to implement rtsp pull stream and onvif controller
 */
class GenericRtspCamera : public DeviceSource {
public:
    friend class GenericRtspCameraImp;
    using Ptr = std::shared_ptr<GenericRtspCamera>;

    GenericRtspCamera(const DeviceTuple &tuple, const std::unordered_map<int, StreamTuple> &stream_map)
        : DeviceSource(GENERIC_RTSP_CAMERA_SCHEMA, tuple), _stream_map(stream_map) {}

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
    std::unordered_map<int, StreamTuple> _stream_map;
};

} // namespace managerkit

#endif // CAMERA_GENERICRTSPCAMERA_H