#include "GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const CameraInfo &info, const unordered_map<int, StreamTuple> &stream_map)
    : GenericRtspCamera(info), StreamSink(stream_map), CameraController(info), RecordStrategy(), CameraStatisticImp(info, stream_map) {}

void GenericRtspCameraImp::onSetCameraOption(const CameraOption &option) {
    _enabled = option.enableActive;

    if (!_enabled) {
        stop();
        return;
    }

    setupController();
    setupScheduler(option.recordScheduler);
    setupRecordStream(getRecordModeActive(), option);
}

void GenericRtspCameraImp::onAllStreamReady() {
    // regist device source after all stream ready
    regist();
}

void GenericRtspCameraImp::onRecordModeChange(RecordMode mode) {
    DebugL << "Camera " << _tuple.device_id << " has already change record mode: " << RecordModeHelper::toString(mode);
    auto option = getCameraOption();
    setupRecordStream(mode, option);
}

void GenericRtspCameraImp::setupRecordStream(RecordMode mode, const CameraOption &option) {
    int rtp_type = option.rtpTransport == option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;
    int media_port = option.autoMediaPort ? 0 :  option.mediaPort;

    if (hasStreamTuple(PrimaryStream)) {
        bool enable_record = option.enableRecord && !option.doNotRecordPrimaryStream && mode != RecordMode::NoRecord;
        setupMonitor(PrimaryStream, enable_record, rtp_type, media_port);
    }

    if (hasStreamTuple(SecondaryStream)) { 
        bool enable_record = option.enableRecord && !option.doNotRecordSecondaryStream  && mode != RecordMode::NoRecord;
        setupMonitor(SecondaryStream, enable_record, rtp_type, media_port);
    }
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
    stopController();
    stopScheduler();
    stopRecordStream();
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
