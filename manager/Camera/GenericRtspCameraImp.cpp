#include "GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const CameraInfo &info, const unordered_map<int, StreamTuple> &stream_map)
    : GenericRtspCamera(info), StreamSink(stream_map), CameraController(info), RecordStrategy() {}

void GenericRtspCameraImp::setCameraOption(const CameraOption &option) {
    _option = option;
    _enabled = _option.enableActive;

    if (!_enabled) {
        stopController();
        stopScheduler();
        stopRecordStream();
        return;
    }

    setupController();
    setupScheduler(_option.recordScheduler);
    setupRecordStream(getRecordModeActive());
}

void GenericRtspCameraImp::onAllStreamReady() {
    // regist device source after all stream ready
    regist();
}

void GenericRtspCameraImp::onRecordModeChange(RecordMode mode) {
    DebugL << "Camera " << _tuple.device_id << " has already change record mode: " << RecordModeHelper::toString(mode);
    setupRecordStream(mode);
}

void GenericRtspCameraImp::setupRecordStream(RecordMode mode) {
    int rtp_type = _option.rtpTransport == _option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;
    int media_port = _option.autoMediaPort ? 0 :  _option.mediaPort;

    if (hasStreamTuple(PrimaryStream)) {
        bool enable_record = _option.enableRecord && !_option.doNotRecordPrimaryStream && mode != RecordMode::NoRecord;
        setupMonitor(PrimaryStream, enable_record, rtp_type, media_port);
    }

    if (hasStreamTuple(SecondaryStream)) { 
        bool enable_record = _option.enableRecord && !_option.doNotRecordSecondaryStream  && mode != RecordMode::NoRecord;
        setupMonitor(SecondaryStream, enable_record, rtp_type, media_port);
    }
}

void GenericRtspCameraImp::stopRecordStream() {
    if (hasStreamTuple(PrimaryStream)) {
        stopMonitor(PrimaryStream);
    }
    if (hasStreamTuple(SecondaryStream)) {
        stopMonitor(SecondaryStream);
    }
}

} // namespace managerkit
