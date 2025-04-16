#include "RtspMediaSourceImp.h"
#include "RtspDemuxer.h"
#include "Common/config.h"
namespace mediakit {
void RtspMediaSource::setSdp(const std::string &sdp) {
    SdpParser sdp_parser(sdp);
    _tracks[TrackVideo] = sdp_parser.getTrack(TrackVideo);
    _tracks[TrackAudio] = sdp_parser.getTrack(TrackAudio);
    _have_video = (bool)_tracks[TrackVideo];
    _sdp = sdp_parser.toString();
    if (_ring) {
        regist();
    }
}

uint32_t RtspMediaSource::getTimeStamp(TrackType trackType) {
    assert(trackType >= TrackInvalid && trackType < TrackMax);
    if (trackType != TrackInvalid) {
        // Get the timestamp of a track
        auto &track = _tracks[trackType];
        if (track) {
            return track->_time_stamp;
        }
    }

    // Get the minimum timestamp of all tracks
    uint32_t ret = UINT32_MAX;
    for (auto &track : _tracks) {
        if (track && track->_time_stamp < ret) {
            ret = track->_time_stamp;
        }
    }
    return ret;
}

/**
 * Update timestamp
*/
void RtspMediaSource::setTimeStamp(uint32_t stamp) {
    for (auto &track : _tracks) {
        if (track) {
            track->_time_stamp = stamp;
        }
    }
}

void RtspMediaSource::onWrite(RtpPacket::Ptr rtp, bool keyPos) {
    _speed[rtp->type] += rtp->size();
    assert(rtp->type >= 0 && rtp->type < TrackMax);
    auto &track = _tracks[rtp->type];
    auto stamp = rtp->getStampMS();
    bool is_video = rtp->type == TrackVideo;
    // Audio is always updated, video is updated when the key packages are
    if (track && ((keyPos && _have_video && is_video) || (!is_video))) {
        track->_seq = rtp->getSeq();
        track->_time_stamp = rtp->getStamp() * uint64_t(1000) / rtp->sample_rate;
        track->_ssrc = rtp->getSSRC();
    }
    if (!_ring) {
        std::weak_ptr<RtspMediaSource> weakSelf = std::static_pointer_cast<RtspMediaSource>(shared_from_this());
        auto lam = [weakSelf](int size) {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->onReaderChanged(size);
        };
        // GOP defaults to buffering 512 groups of RTP packets, each group of RTP packets has the same timestamp (if merge writing is enabled, then each group is the RTP packet within the merge writing time),
        // Every time a key frame's first RTP packet is encountered, the GOP cache will be cleared (because there is a new key frame, which can also achieve instant playback)
        _ring = std::make_shared<RingType>(_ring_size, std::move(lam));
        if (!_sdp.empty()) {
            regist();
        }
    }
   
    PacketCache<RtpPacket>::inputPacket(stamp, is_video, std::move(rtp), keyPos);
}

RtspMediaSourceImp::RtspMediaSourceImp(const MediaTuple& tuple, int ringSize): RtspMediaSource(tuple, ringSize)
{
    _demuxer = std::make_shared<RtspDemuxer>();
    _demuxer->setTrackListener(this);
}

void RtspMediaSourceImp::setSdp(const std::string &strSdp)
{
    if (!getSdp().empty()) {
        return;
    }
    _demuxer->loadSdp(strSdp);
    RtspMediaSource::setSdp(strSdp);
}

void RtspMediaSourceImp::onWrite(RtpPacket::Ptr rtp, bool key_pos)
{
    if (_all_track_ready && !_muxer->isEnabled()) {
        // After getting all Tracks and not enabling protocol conversion, there is no need to demultiplex rtp
        // After closing rtp demultiplexing, it is impossible to know whether it is a key frame, which will lead to the inability to achieve instant playback or the screen will be garbled when playing
        key_pos = rtp->type == TrackVideo;
    } else {
        // Need to demultiplex rtp
        key_pos = _demuxer->inputRtp(rtp);
    }
    GET_CONFIG(bool, directProxy, Rtsp::kDirectProxy);
    if (directProxy) {
        // Only the direct proxy mode directly uses the original rtp
        RtspMediaSource::onWrite(std::move(rtp), key_pos);
    }
}

void RtspMediaSourceImp::setProtocolOption(const ProtocolOption &option)
{
    GET_CONFIG(bool, direct_proxy, Rtsp::kDirectProxy);
    // When direct proxy mode is enabled, rtsp is directly proxied and not duplicated; however, some rtsp push stream ends, because there are already sps pps in the sdp, rtp no longer includes sps pps,
    // This leads to the inability of rtc to play, so it is recommended to turn off direct proxy mode when rtsp pushes the stream and rtc plays
    _option = option;
    _option.enable_rtsp = !direct_proxy;
    _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, _demuxer->getDuration(), _option);
    _muxer->setMediaListener(getListener());
    _muxer->setTrackListener(std::static_pointer_cast<RtspMediaSourceImp>(shared_from_this()));
    // Let the _muxer object intercept some events (such as recording related events)
    MediaSource::setListener(_muxer);

    for (auto &track : _demuxer->getTracks(false)) {
        _muxer->addTrack(track);
        track->addDelegate(_muxer);
    }
}

RtspMediaSource::Ptr RtspMediaSourceImp::clone(const std::string &stream) {
    auto tuple = _tuple;
    tuple.stream = stream;
    auto src_imp = std::make_shared<RtspMediaSourceImp>(tuple);
    src_imp->setSdp(getSdp());
    src_imp->setProtocolOption(getProtocolOption());
    return src_imp;
}

}

