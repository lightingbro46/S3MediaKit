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
    int media_port = _option.autoMediaPort ? 0 :  _option.mediaPort;

    if (!_enabled) {
        if (hasStreamTuple(PrimaryStream)) {
            stopMonitor(PrimaryStream);
        }
        if (hasStreamTuple(SecondaryStream)) {
            stopMonitor(SecondaryStream);
        }
        return;
    }

    if (hasStreamTuple(PrimaryStream)) {
        bool enable_record = _option.enableRecord && !_option.doNotRecordPrimaryStream;
        setupMonitor(PrimaryStream, enable_record, rtp_type, media_port);
    }

    if (hasStreamTuple(SecondaryStream)) { 
        bool enable_record = _option.enableRecord && !_option.doNotRecordSecondaryStream;
        setupMonitor(SecondaryStream, enable_record, rtp_type, media_port);
    }
}

void GenericRtspCameraImp::onAllStreamReady() {
    // regist device source after all stream ready
    regist();
}

} // namespace managerkit
