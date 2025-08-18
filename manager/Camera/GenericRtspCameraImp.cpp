#include "GenericRtspCameraImp.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

GenericRtspCameraImp::GenericRtspCameraImp(const CameraInfo &info, const unordered_map<int, StreamTuple> &stream_map)
    : GenericRtspCamera(info), StreamSink(stream_map) {}

void GenericRtspCameraImp::setCameraOption(const CameraOption &option) {
    _option = option;
    _enabled = _option.enableActive;
    int rtp_type = _option.rtpTransport == _option.kRtpTransportUdp ? 1 /*udp mode*/ : 0 /*tcp mode*/;

    if (!_enabled) {
        if (hasPrimaryStream()) {
            stopMonitor(PrimaryStream);
        }
        if (hasSecondaryStream()) {
            stopMonitor(SecondaryStream);
        }
        return;
    }

    if (hasPrimaryStream()) {
        bool enable_record = _option.enableRecord && !_option.doNotRecordPrimaryStream;
        setupMonitor(PrimaryStream, enable_record, rtp_type);
    }

    if (hasSecondaryStream()) { 
        bool enable_record = _option.enableRecord && !_option.doNotRecordSecondaryStream;
        setupMonitor(SecondaryStream, enable_record, rtp_type);
    }
}

void GenericRtspCameraImp::onAllStreamReady() {
    // regist device source after all stream ready
    regist();
}

} // namespace managerkit
