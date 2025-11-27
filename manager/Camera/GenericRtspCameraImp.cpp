#include "GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const CameraInfo &info, const unordered_map<int, StreamTuple> &stream_map)
    : GenericRtspCamera(info, stream_map), StreamSink(), CameraController(info), RecordStrategy(), CameraStatisticImp(info, stream_map) {
}

void GenericRtspCameraImp::setCameraOptionImp(const CameraOption &option) {
    if (setCameraOption(option)) {
        onSetCameraOption(option);
    }
}

void GenericRtspCameraImp::onAllStreamReady() {
    regist();
}

void GenericRtspCameraImp::onSetCameraOption(const CameraOption &option) {
    _enabled = option.enableActive;
    
    if (!_enabled) {
        stop();
        return;
    }

    saveCameraOption(option);
    setupController(option);
    setupScheduler(option.recordScheduler);
    setupRecordStream(getRecordModeActive());
}

void GenericRtspCameraImp::onRecordModeChange(RecordMode mode) {
    DebugL << "Camera " << _tuple.device_id << " has already change record mode: " << RecordModeHelper::toString(mode);
    setupRecordStream(mode);
}

void GenericRtspCameraImp::setupRecordStream(RecordMode mode) {
    auto option = getCameraOption();
    int rtp_type = option.rtpTransport == option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;
    int media_port = option.autoMediaPort ? 0 :  option.mediaPort;

    if (hasStreamTuple(PrimaryStream)) {
        auto tuple = getStreamTuple(PrimaryStream);
        bool enable_record = option.enableRecord && !option.doNotRecordPrimaryStream && mode != RecordMode::NoRecord;
        setupMonitor(PrimaryStream, tuple, enable_record, rtp_type, media_port);
    }

    if (hasStreamTuple(SecondaryStream)) { 
        auto tuple = getStreamTuple(SecondaryStream);
        bool enable_record = option.enableRecord && !option.doNotRecordSecondaryStream  && mode != RecordMode::NoRecord;
        setupMonitor(SecondaryStream, tuple, enable_record, rtp_type, media_port);
    }

    onAllStreamReady();
}

void GenericRtspCameraImp::stopRecordStream() {
    if (hasStreamTuple(PrimaryStream)) {
        stopMonitor(PrimaryStream);
        onStreamChange(PrimaryStream);
    }
    if (hasStreamTuple(SecondaryStream)) {
        stopMonitor(SecondaryStream);
        onStreamChange(SecondaryStream);
    }
}

void GenericRtspCameraImp::stop() {
    stopScheduler();
    stopRecordStream();
    stopController();
}

void GenericRtspCameraImp::onStreamChange(int stream_type) {
    if (hasStreamTuple(stream_type)) {
        bool live = isStreamLive(stream_type);
        string status = getStreamStatus(stream_type);
        TranslationInfo info = getStreamInfo(stream_type);
        addStreamStatistic(stream_type, live, status, &info);
    }
}

void GenericRtspCameraImp::onControllerReady() {
    bool enable_ptz = enablePTZ();
    addDeviceCapabilities(enable_ptz);
}

} // namespace managerkit
