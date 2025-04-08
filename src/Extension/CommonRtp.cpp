#include "CommonRtp.h"

using namespace mediakit;

CommonRtpDecoder::CommonRtpDecoder(CodecId codec, size_t max_frame_size ){
    _codec = codec;
    _max_frame_size = max_frame_size;
    obtainFrame();
}

void CommonRtpDecoder::obtainFrame() {
    _frame = FrameImp::create();
    _frame->_codec_id = _codec;
}

bool CommonRtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool){
    auto payload_size = rtp->getPayloadSize();
    if (payload_size <= 0) {
        // No actual load
        return false;
    }
    auto payload = rtp->getPayload();
    auto stamp = rtp->getStamp();
    auto seq = rtp->getSeq();

    if (_last_stamp != stamp || _frame->_buffer.size() > _max_frame_size) {
        // If the timestamp changes or the cache exceeds MAX_FRAME_SIZE, clear the previous frame data
        if (!_frame->_buffer.empty()) {
            // If there is a valid frame, output it
            RtpCodec::inputFrame(_frame);
        }

        // New frame data
        obtainFrame();
        _frame->_dts = rtp->getStampMS();
        _last_stamp = stamp;
        _drop_flag = false;
    } else if (_last_seq != 0 && (uint16_t)(_last_seq + 1) != seq) {
        // If the timestamp does not change, but the seq is not continuous, it means that the RTP packet has been lost in the middle, so the entire frame should be discarded
        WarnL << "rtp packet loss:" << _last_seq << " -> " << seq;
        _drop_flag = true;
        _frame->_buffer.clear();
    }

    if (!_drop_flag) {
        _frame->_buffer.append((char *)payload, payload_size);
    }

    _last_seq = seq;

    if (_drop_flag && rtp->getHeader()->mark) {
        _drop_flag = false;
    }
    return false;
}

////////////////////////////////////////////////////////////////

bool CommonRtpEncoder::inputFrame(const Frame::Ptr &frame){
    auto stamp = frame->pts();
    auto ptr = frame->data() + frame->prefixSize();
    auto len = frame->size() - frame->prefixSize();
    auto remain_size = len;
    auto max_size = getRtpInfo().getMaxSize();
    bool is_key = frame->keyFrame();
    bool mark = false;
    while (remain_size > 0) {
        size_t rtp_size;
        if (remain_size > max_size) {
            rtp_size = max_size;
        } else {
            rtp_size = remain_size;
            mark = true;
        }
        RtpCodec::inputRtp(getRtpInfo().makeRtp(frame->getTrackType(), ptr, rtp_size, mark, stamp), is_key);
        ptr += rtp_size;
        remain_size -= rtp_size;
        is_key = false;
    }
    return len > 0;
}