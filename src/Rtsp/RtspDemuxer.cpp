#include <cctype>
#include <algorithm>
#include "RtpCodec.h"
#include "RtspDemuxer.h"
#include "Util/base64.h"
#include "Extension/Factory.h"

using namespace std;

namespace mediakit {

void RtspDemuxer::loadSdp(const string &sdp) {
    loadSdp(SdpParser(sdp));
}

void RtspDemuxer::loadSdp(const SdpParser &attr) {
    auto tracks = attr.getAvailableTrack();
    for (auto &track : tracks) {
        switch (track->_type) {
            case TrackVideo: {
                makeVideoTrack(track);
            }
                break;
            case TrackAudio: {
                makeAudioTrack(track);
            }
                break;
            default:
                break;
        }
    }
    // rtsp can immediately know how many tracks there are through sdp
    addTrackCompleted();

    auto titleTrack = attr.getTrack(TrackTitle);
    if (titleTrack) {
        _duration = titleTrack->_duration;
    }
}

float RtspDemuxer::getDuration() const {
    return _duration;
}

bool RtspDemuxer::inputRtp(const RtpPacket::Ptr &rtp) {
    switch (rtp->type) {
        case TrackVideo: {
            if (_video_rtp_decoder) {
                return _video_rtp_decoder->inputRtp(rtp, true);
            }
            return false;
        }
        case TrackAudio: {
            if (_audio_rtp_decoder) {
                _audio_rtp_decoder->inputRtp(rtp, false);
                return false;
            }
            return false;
        }
        default: return false;
    }
}

static void setBitRate(const SdpTrack::Ptr &sdp, const Track::Ptr &track) {
    if (!sdp->_b.empty()) {
        int data_rate = 0;
        sscanf(sdp->_b.data(), "AS:%d", &data_rate);
        if (data_rate) {
            track->setBitRate(data_rate * 1024);
        }
    }
}

void RtspDemuxer::makeAudioTrack(const SdpTrack::Ptr &audio) {
    if (_audio_rtp_decoder) {
        return;
    }
    // Generate Track object
    _audio_track = dynamic_pointer_cast<AudioTrack>(Factory::getTrackBySdp(audio));
    if (!_audio_track) {
        return;
    }
    setBitRate(audio, _audio_track);
    // Generate RtpCodec object to decode rtp
    _audio_rtp_decoder = Factory::getRtpDecoderByCodecId(_audio_track->getCodecId());
    if (!_audio_rtp_decoder) {
        // Cannot find the corresponding rtp decoder, the track is invalid
        _audio_track.reset();
        return;
    }
    // Set the rtp decoder proxy, the generated frame is written to this Track
    _audio_rtp_decoder->addDelegate(_audio_track);
    addTrack(_audio_track);
}

void RtspDemuxer::makeVideoTrack(const SdpTrack::Ptr &video) {
    if (_video_rtp_decoder) {
        return;
    }
    // Generate Track object
    _video_track = dynamic_pointer_cast<VideoTrack>(Factory::getTrackBySdp(video));
    if (!_video_track) {
        return;
    }
    setBitRate(video, _video_track);
    // Generate RtpCodec object to decode rtp
    _video_rtp_decoder = Factory::getRtpDecoderByCodecId(_video_track->getCodecId());
    if (!_video_rtp_decoder) {
        // Cannot find the corresponding rtp decoder, the track is invalid
        _video_track.reset();
        return;
    }
    // Set the rtp decoder proxy, the generated frame is written to this Track
    _video_rtp_decoder->addDelegate(_video_track);
    addTrack(_video_track);
}

} /* namespace mediakit */
