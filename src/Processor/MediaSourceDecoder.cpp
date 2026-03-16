#if defined(ENABLE_FFMPEG)

#include "MediaSourceDecoder.h"
#include <memory>

using namespace std;

namespace mediakit {

///////////////////////////MediaSourceDecoder///////////////////////////

MediaSourceDecoder::~MediaSourceDecoder() {
    try {
        flush();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

bool MediaSourceDecoder::addTrack(const Track::Ptr &track) {
    if (_track_existed[track->getTrackType()]) {
        // Do not support multiple tracks of the same type
        WarnL << "Already add a track kind of: " << track->getTrackTypeStr() << ", ignore track: " << track->getCodecName();
        return false;
    }

    if (!track->ready()) {
        WarnL << track->getCodecName() << " unready!";
        return false;
    }

    auto &ref = _tracks[track->getIndex()];
    auto &decoder = ref.decoder;
    CHECK(!decoder);

    if (track->getTrackType() == TrackVideo) {
        auto video_track = dynamic_pointer_cast<VideoTrack>(track);
        decoder = std::make_shared<FFmpegDecoder>(video_track, 0, std::vector<std::string>{ "h264", "hevc" });
        // Set decode callback
        decoder->setOnDecode([this](const FFmpegFrame::Ptr &frame) {
            onDecode(frame);
        });
        // Note: do not use video_track 's addDelegate method like VideoStack because MultiMediaSourceMuxer has a FrameStamp to change the timestamp
    }
    // todo: support audio decoder if needed

    _track_existed[track->getTrackType()] = true;
    return true;
}

bool MediaSourceDecoder::inputFrame(const Frame::Ptr &frame) {
    auto it = _tracks.find(frame->getIndex());
    if (it == _tracks.end()) {
        // This Track does not exist or initialization failed
        return false;
    }
    if (!_started) {
        // This logic ensures that the first frame is a keyframe when there is video
        if (frame->getTrackType() == TrackVideo && !frame->keyFrame()) {
            // Contains video, but not a keyframe, then the previous frames are discarded
            return false;
        }
        // Start decoding
        _started = true;
    }
    auto &track = it->second;
    auto &decoder = track.decoder;
    if (decoder) {
        return decoder->inputFrame(frame, true, true);
    }
    return false;
}

void MediaSourceDecoder::flush() {
    for (auto &track : _tracks) {
        auto &decoder = track.second.decoder;
        if (decoder) {
            decoder->flush();
        }
    }
}

void MediaSourceDecoder::resetTracks() {
    _tracks.clear();
    CLEAR_ARR(_track_existed);
}

bool MediaSourceDecoder::haveVideo() const {
    return _track_existed[TrackVideo];
}

} // namespace mediakit

#endif // ENABLE_FFMPEG