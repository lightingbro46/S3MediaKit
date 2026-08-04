#ifndef CAMERA_GENERICRTSPCAMERA_H
#define CAMERA_GENERICRTSPCAMERA_H

#include "Common/DeviceSource.h"
#include "Common/DeviceOption.h"
#include "StreamSource.h"

#define GENERIC_RTSP_CAMERA "GENERIC-RTSP"

namespace managerkit {

class CameraOption : public DeviceOption {
public:
    CameraOption() {
        // todo: load default value from database
    }

    // camera name 
    std::string name;

    // camera model
    std::string model;

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

    // custom MP4 recording root. Empty means use Protocol::kMP4SavePath.
    std::string recordRootPath;

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

    // SD card synchronization config
    bool sdCardSyncEnabled = false;
    bool sdCardSyncAutoSyncEnabled = false;
    int sdCardSyncMinSegmentGapSec = 0;
    int sdCardSyncRetryCount = 0;

    // whether to enforce privacy mask, default false
    bool enforcePrivacyMaskOnView = false;

    // comma separated role ids, such as "1,2,3", which will be excluded from privacy mask
    std::string privacyMaskExcludedRoleIds;

    // privacy mask regions, format: json array, each element is a json object with fields: id, name, polygonMode, maskType, points, for example:
    // [{
    // "id": "region-1",
    // "name": "Cashier",
    // "polygonMode": true,
    // "maskType": "BLUR",
    // "points":[{"x": 0.15, "y": 0.2 }, {"x": 0.75, "y": 0.2 }, {"x": 0.75, "y": 0.8 }, {"x": 0.15, "y": 0.8 }]
    // }]
    std::string privacyMaskRegions;

    // whether to enforce watermark, default false
    bool enforceWatermarkOnView = false;

    // watermark template id, default empty
    std::string watermarkTemplateId;

    // comma separated role codes, such as "1,2,3", excluded from watermark.
    std::string watermarkExcludedRoleIds;

    // serialized watermark template/components used by the view overlay policy.
    // {
    //     "id": "019fa180-b1e2-7000-a18a-504454a8da83",
    //     "name": "Watermark nội bộ 1",
    //     "description": "Template watermark dùng cho camera nội bộ",
    //     "displayMode": "REPEATED",
    //     "repeatEnabled": true,
    //     "canvas":{
    //     "width": 1280,
    //     "height": 720
    //     },
    //     "components":[
    //     {
    //     "type": "TEXT",
    //     "id": "txt_1",
    //     "name": "Text",
    //     "x": 610,
    //     "y": 360,
    //     "scale": 1.0,
    //     "rotation": 0.0,
    //     "opacity": 0.5,
    //     "repeatEnabled": true,
    //     "gapX": 320,
    //     "gapY": 190,
    //     "text": "Nội bộ",
    //     "fontFamily": "Arial",
    //     "fontSize": 34,
    //     "fontWeight": 700,
    //     "color": "#ffffff",
    //     "zindex": 0
    //     },
    //     {
    //     "type": "IMAGE",
    //     "id": "img_1",
    //     "name": "logo.png",
    //     "x": 520,
    //     "y": 300,
    //     "scale": 1.0,
    //     "rotation": 0.0,
    //     "opacity": 0.5,
    //     "repeatEnabled": false,
    //     "gapX": 320,
    //     "gapY": 190,
    //     "imageId": "019fa180-7cdb-7000-874c-b510ca3511c0",
    //     "width": 120,
    //     "height": 120,
    //     "preserveAspectRatio": "xMidYMid meet",
    //     "zindex": 0
    //     }
    //     ],
    //     "overlayAssetId": "019fa180-7cdb-7000-874c-b510ca3511c0"
    // }
    std::string watermarkTemplate;

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
               recordRootPath == other.recordRootPath &&
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
               motionDetectOnStream == other.motionDetectOnStream &&
               sdCardSyncEnabled == other.sdCardSyncEnabled &&
               sdCardSyncAutoSyncEnabled == other.sdCardSyncAutoSyncEnabled &&
               sdCardSyncMinSegmentGapSec == other.sdCardSyncMinSegmentGapSec &&
               sdCardSyncRetryCount == other.sdCardSyncRetryCount &&
               enforceWatermarkOnView == other.enforceWatermarkOnView &&
               watermarkTemplateId == other.watermarkTemplateId &&
               watermarkExcludedRoleIds == other.watermarkExcludedRoleIds &&
               watermarkTemplate == other.watermarkTemplate &&
               enforcePrivacyMaskOnView == other.enforcePrivacyMaskOnView &&
               privacyMaskExcludedRoleIds == other.privacyMaskExcludedRoleIds &&
               privacyMaskRegions == other.privacyMaskRegions;
    }

    bool operator!=(const CameraOption& other) const {
        return !(*this == other);
    }
};

struct SdCardSyncConfig {
    bool sdCardSyncEnabled = false;
    bool sdCardSyncAutoSyncEnabled = false;
    int sdCardSyncMinSegmentGapSec = 0;
    int sdCardSyncRetryCount = 0;

    static SdCardSyncConfig from(const CameraOption &o) {
        SdCardSyncConfig cfg;
        cfg.sdCardSyncEnabled             = o.sdCardSyncEnabled;
        cfg.sdCardSyncAutoSyncEnabled     = o.sdCardSyncAutoSyncEnabled;
        cfg.sdCardSyncMinSegmentGapSec    = o.sdCardSyncMinSegmentGapSec;
        cfg.sdCardSyncRetryCount          = o.sdCardSyncRetryCount;
        return cfg;
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