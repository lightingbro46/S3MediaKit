#include "RtspMuxer.h"
#include "Common/config.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

void RtspMuxer::onRtp(RtpPacket::Ptr in, bool is_key) {
    if (_live) {
        auto &ref = _tracks[in->track_index];
        if (ref.rtp_stamp != in->getHeader()->stamp) {
            // Only calculate NTP when the RTP timestamp changes, saving CPU resources
            int64_t stamp_ms_inc;
            // Get the RTP timestamp increment
            ref.stamp.revise(in->ntp_stamp, in->ntp_stamp, stamp_ms_inc, stamp_ms_inc);
            ref.rtp_stamp = in->getHeader()->stamp;
            ref.ntp_stamp = stamp_ms_inc + _ntp_stamp_start;
        }

        // RTP interception entry, set NTP here uniformly
        in->ntp_stamp = ref.ntp_stamp;
    } else {
        // In on-demand scenarios, set the NTP timestamp to the RTP timestamp plus the base NTP timestamp
        in->ntp_stamp = _ntp_stamp_start + (in->getStamp() * uint64_t(1000) / in->sample_rate);
    }
    _rtpRing->write(std::move(in), is_key);
}

RtspMuxer::RtspMuxer(const TitleSdp::Ptr &title) {
    if (!title) {
        _sdp = std::make_shared<TitleSdp>()->getSdp();
    } else {
        _live = title->getDuration() == 0;
        _sdp = title->getSdp();
    }
    _rtpRing = std::make_shared<RtpRing::RingType>();
    _rtpInterceptor = std::make_shared<RtpRing::RingType>();
    _rtpInterceptor->setDelegate(std::make_shared<RingDelegateHelper>([this](RtpPacket::Ptr in, bool is_key) {
        onRtp(std::move(in), is_key);
    }));

    _ntp_stamp_start = getCurrentMillisecond(true);
}

bool RtspMuxer::addTrack(const Track::Ptr &track) {
    if (_track_existed[track->getTrackType()]) {
        // RTSP does not support multiple tracks of the same type
        WarnL << "Already add a track kind of: " << track->getTrackTypeStr() << ", ignore track: " << track->getCodecName();
        return false;
    }
    if (!track->ready()) {
        WarnL << track->getCodecName() << " unready!";
        return false;
    }

    auto &ref = _tracks[track->getIndex()];
    auto &encoder = ref.encoder;
    CHECK(!encoder);

    auto pt = RtpPayload::getPayloadType(*track);
    // Payload type 96 and above is dynamic PT
    Sdp::Ptr sdp = track->getSdp(pt == -1 ? 96 + _index : pt);
    if (!sdp) {
        WarnL << "Unsupported codec: " << track->getCodecName();
        return false;
    }

    encoder = Factory::getRtpEncoderByCodecId(track->getCodecId(), sdp->getPayloadType());
    if (!encoder) {
        return false;
    }

    // Mark that a track of this type already exists
    _track_existed[track->getTrackType()] = true;

    {
        static atomic<uint32_t> s_ssrc(0);
        uint32_t ssrc = s_ssrc++;
        if (!ssrc) {
            // SSRC cannot be 0
            ssrc = s_ssrc++;
        }
        if (track->getTrackType() == TrackVideo) {
            // The video SSRC is even for debugging convenience
            ssrc = 2 * ssrc;
        } else {
            // The audio SSRC is odd
            ssrc = 2 * ssrc + 1;
        }
        GET_CONFIG(uint32_t, audio_mtu, Rtp::kAudioMtuSize);
        GET_CONFIG(uint32_t, video_mtu, Rtp::kVideoMtuSize);
        auto mtu = track->getTrackType() == TrackVideo ? video_mtu : audio_mtu;
        encoder->setRtpInfo(ssrc, mtu, sdp->getSampleRate(), sdp->getPayloadType(), 2 * track->getTrackType(), track->getIndex());
    }

    // Set the RTP output circular buffer
    encoder->setRtpRing(_rtpInterceptor);

    auto str = sdp->getSdp();
    str += "a=control:trackID=";
    str += std::to_string(_index);
    str += "\r\n";

    // Add its SDP
    _sdp.append(str);
    trySyncTrack();

    // The RTP timestamp is PTS, allowing rollback
    if (track->getTrackType() == TrackVideo) {
        ref.stamp.enableRollback(true);
    }
    ++_index;
    return true;
}

void RtspMuxer::trySyncTrack() {
    Stamp *first = nullptr;
    for (auto &pr : _tracks) {
        if (!first) {
            first = &pr.second.stamp;
        } else {
            pr.second.stamp.syncTo(*first);
        }
    }
}

bool RtspMuxer::inputFrame(const Frame::Ptr &frame) {
    auto &encoder = _tracks[frame->getIndex()].encoder;
    return encoder ? encoder->inputFrame(frame) : false;
}

void RtspMuxer::flush() {
    for (auto &pr : _tracks) {
        if (pr.second.encoder) {
            pr.second.encoder->flush();
        }
    }
}

string RtspMuxer::getSdp() {
    return _sdp;
}

RtpRing::RingType::Ptr RtspMuxer::getRtpRing() const {
    return _rtpRing;
}

void RtspMuxer::resetTracks() {
    _sdp.clear();
    _tracks.clear();
    CLEAR_ARR(_track_existed);
}

} /* namespace mediakit */