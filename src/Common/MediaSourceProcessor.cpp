#include "MediaSourceProcessor.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MediaSourceProcessor::MediaSourceProcessor(const MediaTuple &tuple, const ProtocolOption &option) : _tuple(tuple), _option(option) {}

MediaSourceProcessor::~MediaSourceProcessor() {
    try {
        flush();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MediaSourceProcessor::flush() {
    if (_decoder) {
        _decoder->flush();
    }
}

bool MediaSourceProcessor::addTrack(const Track::Ptr & track) {
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MediaSourceProcessor::resetTracks() {
    _tracks.clear();
    _have_video = false;
}

void MediaSourceProcessor::addTrackCompleted() {
    if (_have_video) {
        VideoTrack::Ptr video_track;
        for (auto &track : _tracks) {
            if (track->getTrackType() == TrackVideo) {
                video_track = std::dynamic_pointer_cast<VideoTrack>(track);
                break;
            }
        }
        std::weak_ptr<MediaSourceProcessor> weak_self = shared_from_this();
        _decoder = std::make_shared<FFmpegDecoder>(video_track, 0, std::vector<std::string>{ "h264", "hevc" });
        // Set decode callback
        _decoder->setOnDecode([weak_self](const FFmpegFrame::Ptr &frame) {
            auto self = weak_self.lock();
            if (!self) { return; }
            
            self->onDecode(frame);
        });
        // Note: do not use video_track 's addDelegate method like VideoStack because MultiMediaSourceMuxer has a FrameStamp to change the timestamp
        
        if (_option.enable_motion) {
            _motion_proc = std::make_shared<MotionProcessor>(_tuple);
        }
    }
}

bool MediaSourceProcessor::inputFrame(const Frame::Ptr &frame) {
    if (_decoder && frame->getTrackType() == TrackVideo) {
        // Decode video frame
        return _decoder->inputFrame(frame, true, false);
    }
    return true;
}

void MediaSourceProcessor::onDecode(const FFmpegFrame::Ptr &frame) {
    // TraceL << "Decoded frame dts: " << frame->get()->pkt_dts << ", pts: " << frame->get()->pts << ", size: " << frame->get()->pkt_size;
    // Dispatch decoded frame to all tracks
    if (_motion_proc) {
        _motion_proc->inputFrame(frame);
    }
}

} // namespace mediakit