#if defined(ENABLE_MKV)

#include "MKVMuxer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MKVMuxer::~MKVMuxer() {
    closeMKV();
}

void MKVMuxer::openMKV(const string &file) {
    closeMKV();
    _file_name = file;
    _mkv_file = std::make_shared<MKVFileDisk>();
    _mkv_file->openFile(_file_name.data(), "wb+");
}

MKVFileIO::Writer MKVMuxer::createWriter() {
    GET_CONFIG(bool, recordEnableWebM, Record::kEnableWebM);
    return _mkv_file->createWriter(recordEnableWebM ? (int)MKV_OPTION_WEBM : 0);
}

void MKVMuxer::closeMKV() {
    MKVMuxerInterface::resetTracks();
    _mkv_file = nullptr;
}

void MKVMuxer::resetTracks() {
    MKVMuxerInterface::resetTracks();
    openMKV(_file_name);
}

/////////////////////////////////////////// MKVMuxerInterface /////////////////////////////////////////////

bool MKVMuxerInterface::haveVideo() const {
    return _have_video;
}

uint64_t MKVMuxerInterface::getDuration() const {
    uint64_t ret = 0;
    for (auto &pr : _tracks) {
        if (pr.second.stamp.getRelativeStamp() > (int64_t)ret) {
            ret = pr.second.stamp.getRelativeStamp();
        }
    }
    return ret;
}

void MKVMuxerInterface::resetTracks() {
    _started = false;
    _have_video = false;
    _mkv_writter = nullptr;
    _tracks.clear();
}

void MKVMuxerInterface::flush() {
    for (auto &pr : _tracks) {
        pr.second.merger.flush();
    }
}

bool MKVMuxerInterface::inputFrame(const Frame::Ptr &frame) {
    auto it = _tracks.find(frame->getIndex());
    if (it == _tracks.end()) {
        // This Track does not exist or initialization failed
        return false;
    }

    if (!_started) {
        // This logic ensures that the first frame is a keyframe when there is video
        if (_have_video && !frame->keyFrame()) {
            // Contains video, but not a keyframe, then the previous frames are discarded
            return false;
        }
        // Start writing the file
        _started = true;
    }

    if (frame->getTrackType() == TrackVideo && _mkv_writter) {
        if (frame->keyFrame()) {
            _non_iframe_video_count = 0;
        } else {
            _non_iframe_video_count++;
        }
    }

    // The mkv file timestamp needs to start from 0
    auto &track = it->second;
    switch (frame->getCodecId()) {
        case CodecH264:
        case CodecH265: {
            // The code logic here is to package frames with the same timestamp, such as SPS, PPS, and IDR, as one frame,
            track.merger.inputFrame(frame, [this, &track](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
                int64_t dts_out, pts_out;
                track.stamp.revise(dts, pts, dts_out, pts_out);
                mkv_writer_write(_mkv_writter.get(), track.track_id, buffer->data(), buffer->size(), pts_out, dts_out, have_idr ? MKV_FLAGS_KEYFRAME : 0);
            });
            break;
        }

        default: {
            int64_t dts_out, pts_out;
            track.stamp.revise(frame->dts(), frame->pts(), dts_out, pts_out);
            mkv_writer_write(_mkv_writter.get(), track.track_id, frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize(), pts_out, dts_out, frame->keyFrame() ? MKV_FLAGS_KEYFRAME : 0);
            break;
        }
    }
    return true;
}

void MKVMuxerInterface::stampSync() {
    Stamp *first = nullptr;
    for (auto &pr : _tracks) {
        if (!first) {
            first = &pr.second.stamp;
        } else {
            pr.second.stamp.syncTo(*first);
        }
    }
}

bool MKVMuxerInterface::addTrack(const Track::Ptr &track) {
    if (!_mkv_writter) {
        _mkv_writter = createWriter();
    }
    auto mkv_codec = getMkvIdByCodec(track->getCodecId());
    if (mkv_codec == MKV_CODEC_UNKNOWN) {
        WarnL << "Unsupported codec: " << track->getCodecName();
        return false;
    }

    if (!track->ready()) {
        WarnL << "Track " << track->getCodecName() << " unready";
        return false;
    }

    track->update();

    auto extra = track->getExtraData();
    auto extra_data = extra ? extra->data() : nullptr;
    auto extra_size = extra ? extra->size() : 0;
    if (track->getTrackType() == TrackVideo) {
        auto video_track = dynamic_pointer_cast<VideoTrack>(track);
        CHECK(video_track);
        auto track_id = mkv_writer_add_video(_mkv_writter.get(), static_cast<mkv_codec_t>(mkv_codec), video_track->getVideoWidth(), video_track->getVideoHeight(), extra_data, extra_size);
        if (track_id < 0) {
            WarnL << "mkv_writer_add_video failed: " << video_track->getCodecName();
            return false;
        }
        _tracks[track->getIndex()].track_id = track_id;
        _have_video = true;
        _non_iframe_video_count = 0;
    } else if (track->getTrackType() == TrackAudio) {
        auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
        CHECK(audio_track);
        auto track_id = mkv_writer_add_audio(_mkv_writter.get(), static_cast<mkv_codec_t>(mkv_codec), audio_track->getAudioChannel(), audio_track->getAudioSampleBit() * audio_track->getAudioChannel(), audio_track->getAudioSampleRate(), extra_data, extra_size);
        if (track_id < 0) {
            WarnL << "mkv_writer_add_audio failed: " << audio_track->getCodecName();
            return false;
        }
        _tracks[track->getIndex()].track_id = track_id;
    }

    // Try audio and video synchronization
    stampSync();
    return true;
}

/////////////////////////////////////////// MKVMuxerMemory /////////////////////////////////////////////

MKVMuxerMemory::MKVMuxerMemory() {
    _memory_file = std::make_shared<MKVFileMemory>();
}

MKVFileIO::Writer MKVMuxerMemory::createWriter() {
    return _memory_file->createWriter(MKV_OPTION_LIVE);
}

void MKVMuxerMemory::resetTracks() {
    MKVMuxerInterface::resetTracks();
    _memory_file = std::make_shared<MKVFileMemory>();
}

bool MKVMuxerMemory::inputFrame(const Frame::Ptr &frame) {
    auto data = _memory_file->getAndClearMemory();
    if (!data.empty()) {
        // Output segment data
        onSegmentData(std::move(data), _last_dst, _key_frame);
        _key_frame = false;
    }

    // only audio all frame is key frame
    if (frame->keyFrame() || !haveVideo()) {
        _key_frame = true;
    }
    if (frame->getTrackType() == TrackVideo || !haveVideo()) {
        _last_dst = frame->dts();
    }
    return MKVMuxerInterface::inputFrame(frame);
}

} // namespace mediakit
#endif // defined(ENABLE_MKV)