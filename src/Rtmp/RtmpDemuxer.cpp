#include "RtmpCodec.h"
#include "RtmpDemuxer.h"
#include "Extension/Factory.h"

using namespace std;

namespace mediakit {

size_t RtmpDemuxer::trackCount(const AMFValue &metadata) {
    size_t ret = 0;
    metadata.object_for_each([&](const string &key, const AMFValue &val) {
        if (key == "videocodecid") {
            // Find video
            ++ret;
            return;
        }
        if (key == "audiocodecid") {
            // Find audio
            ++ret;
            return;
        }
    });
    return ret;
}

bool RtmpDemuxer::loadMetaData(const AMFValue &val) {
    bool ret = false;
    try {
        int audiosamplerate = 0;
        int audiochannels = 0;
        int audiosamplesize = 0;
        int videodatarate = 0;
        int audiodatarate = 0;
        const AMFValue *audiocodecid = nullptr;
        const AMFValue *videocodecid = nullptr;
        val.object_for_each([&](const string &key, const AMFValue &val) {
            if (key == "duration") {
                _duration = (float)val.as_number();
                return;
            }
            if (key == "audiosamplerate") {
                audiosamplerate = val.as_integer();
                return;
            }
            if (key == "audiosamplesize") {
                audiosamplesize = val.as_integer();
                return;
            }
            if (key == "stereo") {
                audiochannels = val.as_boolean() ? 2 : 1;
                return;
            }
            if (key == "videocodecid") {
                // Find video
                videocodecid = &val;
                return;
            }
            if (key == "audiocodecid") {
                // Find audio
                audiocodecid = &val;
                return;
            }
            if (key == "audiodatarate") {
                audiodatarate = val.as_integer();
                return;
            }
            if (key == "videodatarate") {
                videodatarate = val.as_integer();
                return;
            }
        });
        if (videocodecid) {
            // Has video
            ret = true;
            makeVideoTrack(*videocodecid, videodatarate * 1024);
        }
        if (audiocodecid) {
            // Has audio
            ret = true;
            makeAudioTrack(*audiocodecid, audiosamplerate, audiochannels, audiosamplesize, audiodatarate * 1024);
        }
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }

    if (ret) {
        // If there is a track-related description in the metadata, we determine the number of tracks based on the metadata
        addTrackCompleted();
    }
    return ret;
}

float RtmpDemuxer::getDuration() const {
    return _duration;
}

void RtmpDemuxer::inputRtmp(const RtmpPacket::Ptr &pkt) {
    switch (pkt->type_id) {
        case MSG_VIDEO: {
            if (!_try_get_video_track) {
                _try_get_video_track = true;
                auto codec_id = parseVideoRtmpPacket((uint8_t *)pkt->data(), pkt->size());
                makeVideoTrack(Factory::getTrackByCodecId(codec_id), 0);
            }
            if (_video_rtmp_decoder) {
                _video_rtmp_decoder->inputRtmp(pkt);
            }
            break;
        }

        case MSG_AUDIO: {
            if (!_try_get_audio_track) {
                _try_get_audio_track = true;
                auto codec = AMFValue(pkt->getRtmpCodecId());
                makeAudioTrack(codec, pkt->getAudioSampleRate(), pkt->getAudioChannel(), pkt->getAudioSampleBit(), 0);
            }
            if (_audio_rtmp_decoder) {
                _audio_rtmp_decoder->inputRtmp(pkt);
            }
            break;
        }
        default: break;
    }
}

void RtmpDemuxer::makeVideoTrack(const AMFValue &videoCodec, int bit_rate) {
    makeVideoTrack(Factory::getVideoTrackByAmf(videoCodec), bit_rate);
}

void RtmpDemuxer::makeVideoTrack(const Track::Ptr &track, int bit_rate) {
    if (_video_rtmp_decoder) {
        return;
    }
    // Generate Track object
    _video_track = dynamic_pointer_cast<VideoTrack>(track);
    if (!_video_track) {
        return;
    }
    // Generate rtmpCodec object to decode rtmp
    _video_rtmp_decoder = Factory::getRtmpDecoderByTrack(_video_track);
    if (!_video_rtmp_decoder) {
        // Cannot find the corresponding rtmp decoder, the track is invalid
        _video_track.reset();
        return;
    }
    _video_track->setBitRate(bit_rate);
    addTrack(_video_track);
    _try_get_video_track = true;
}

void RtmpDemuxer::makeAudioTrack(const AMFValue &audioCodec, int sample_rate, int channels, int sample_bit, int bit_rate) {
    if (_audio_rtmp_decoder) {
        return;
    }
    // Generate Track object
    _audio_track = dynamic_pointer_cast<AudioTrack>(Factory::getAudioTrackByAmf(audioCodec, sample_rate, channels, sample_bit));
    if (!_audio_track) {
        return;
    }
    // Generate rtmpCodec object to decode rtmp
    _audio_rtmp_decoder = Factory::getRtmpDecoderByTrack(_audio_track);
    if (!_audio_rtmp_decoder) {
        // Cannot find the corresponding rtmp decoder, the track is invalid
        _audio_track.reset();
        return;
    }
    _audio_track->setBitRate(bit_rate);
    addTrack(_audio_track);
    _try_get_audio_track = true;
}

} /* namespace mediakit */