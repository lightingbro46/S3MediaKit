#if defined(ENABLE_MP4)

#include "MP4Muxer.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MP4Muxer::~MP4Muxer() {
    closeMP4();
}

void MP4Muxer::openMP4(const string &file) {
    closeMP4();
    _file_name = file;
    _mp4_file = std::make_shared<MP4FileDisk>();
    _mp4_file->openFile(_file_name.data(), "wb+");
}

MP4FileIO::Writer MP4Muxer::createWriter() {
    GET_CONFIG(bool, mp4FastStart, Record::kFastStart);
    GET_CONFIG(bool, recordEnableFmp4, Record::kEnableFmp4);
    return _mp4_file->createWriter(mp4FastStart ? MOV_FLAG_FASTSTART : 0, recordEnableFmp4);
}

void MP4Muxer::closeMP4() {
    MP4MuxerInterface::resetTracks();
    _mp4_file = nullptr;
}

void MP4Muxer::resetTracks() {
    MP4MuxerInterface::resetTracks();
    openMP4(_file_name);
}

/////////////////////////////////////////// MP4MuxerInterface /////////////////////////////////////////////

void MP4MuxerInterface::saveSegment() {
    mp4_writer_save_segment(_mov_writter.get());
}

void MP4MuxerInterface::initSegment() {
    mp4_writer_init_segment(_mov_writter.get());
}

bool MP4MuxerInterface::haveVideo() const {
    return _have_video;
}

uint64_t MP4MuxerInterface::getDuration() const {
    uint64_t ret = 0;
    for (auto &pr : _tracks) {
        if (pr.second.stamp.getRelativeStamp() > (int64_t)ret) {
            ret = pr.second.stamp.getRelativeStamp();
        }
    }
    return ret;
}

uint64_t MP4MuxerInterface::getNextDts() const {
    int64_t ret = 0;
    for (auto &pr : _tracks) {
        if (pr.second.last_dts != INT64_MIN) {
            ret = std::max(ret, pr.second.last_dts + pr.second.last_interval);
        }
    }
    return ret;
}

void MP4MuxerInterface::onSampleWritten(MP4Track &track, int64_t dts_out) {
    if (track.start_dts == INT64_MIN) {
        track.start_dts = dts_out;
    }
    if (track.last_dts != INT64_MIN && dts_out > track.last_dts) {
        track.last_interval = dts_out - track.last_dts;
    }
    track.last_dts = dts_out;
}

void MP4MuxerInterface::resetTracks() {
    _started = false;
    _have_video = false;
    _pending_config.clear();
    _mov_writter = nullptr;
    _tracks.clear();
}

void MP4MuxerInterface::seedStampOffsets(int64_t offset_ms) {
    for (auto &pr : _tracks) {
        pr.second.stamp.setRelativeStamp(offset_ms);
    }
}

void MP4MuxerInterface::flush() {
    for (auto &pr : _tracks) {
        pr.second.merger.flush();
    }
}

bool MP4MuxerInterface::inputFrame(const Frame::Ptr &frame) {
    auto it = _tracks.find(frame->getIndex());
    if (it == _tracks.end()) {
        // This Track does not exist or initialization failed
        return false;
    }

    if (!_started) {
        // This logic ensures that the first frame is a keyframe when there is video
        if (_have_video && !frame->keyFrame()) {
            // Contains video, but not a keyframe, then the previous frames are discarded.
            // Parameter sets right before the keyframe are kept: after a track reset
            // they carry a new resolution in-band, which players reading fMP4
            // progressively need because they ignore a second init segment.
            if (frame->getTrackType() == TrackVideo) {
                if (frame->configFrame()) {
                    _pending_config.emplace_back(Frame::getCacheAbleFrame(frame));
                } else {
                    _pending_config.clear();
                }
            }
            return false;
        }
        // Start writing the file
        _started = true;
        auto pending_config = std::move(_pending_config);
        _pending_config.clear();
        for (auto &config : pending_config) {
            if (config->dts() == frame->dts()) {
                inputFrame(config);
            }
        }
    }

    // fmp4 encapsulation exceeds a certain I-frame interval, force refresh segment to prevent memory increase
    if (frame->getTrackType() == TrackVideo && _mov_writter->fmp4) {
        if (frame->keyFrame()) {
            _non_iframe_video_count = 0;
        } else {
            _non_iframe_video_count++;
        }

        if (_non_iframe_video_count > 200) {
            saveSegment();
            _non_iframe_video_count = 0;
        }
    }

    // The mp4 file timestamp needs to start from 0
    auto &track = it->second;
    switch (frame->getCodecId()) {
        case CodecH264:
        case CodecH265: {
            // The code logic here is to package frames with the same timestamp, such as SPS, PPS, and IDR, as one frame,
            track.merger.inputFrame(frame, [this, &track](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
                int64_t dts_out, pts_out;
                track.stamp.revise(dts, pts, dts_out, pts_out);
                onSampleWritten(track, dts_out);
                mp4_writer_write(_mov_writter.get(), track.track_id, buffer->data(), buffer->size(), pts_out, dts_out, have_idr ? MOV_AV_FLAG_KEYFREAME : 0);
            });
            break;
        }

        default: {
            int64_t dts_out, pts_out;
            track.stamp.revise(frame->dts(), frame->pts(), dts_out, pts_out);
            onSampleWritten(track, dts_out);
            mp4_writer_write(_mov_writter.get(), track.track_id, frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize(), pts_out, dts_out, frame->keyFrame() ? MOV_AV_FLAG_KEYFREAME : 0);
            break;
        }
    }
    return true;
}

namespace {

uint32_t readU32(const char *ptr) {
    auto p = reinterpret_cast<const uint8_t *>(ptr);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void writeU32(char *ptr, uint32_t value) {
    for (int i = 3; i >= 0; --i, value >>= 8) {
        ptr[i] = static_cast<char>(value & 0xFF);
    }
}

uint64_t readU64(const char *ptr) {
    return (uint64_t(readU32(ptr)) << 32) | readU32(ptr + 4);
}

void writeU64(char *ptr, uint64_t value) {
    writeU32(ptr, static_cast<uint32_t>(value >> 32));
    writeU32(ptr + 4, static_cast<uint32_t>(value));
}

// Calls cb(type, box_begin, box_size, header_size) for each box in [begin, end).
template <typename CB>
void forEachBox(char *begin, char *end, CB &&cb) {
    while (end - begin >= 8) {
        uint64_t size = readU32(begin);
        size_t header = 8;
        if (size == 1) {
            if (end - begin < 16) {
                return;
            }
            size = readU64(begin + 8);
            header = 16;
        } else if (size == 0) {
            size = end - begin;
        }
        if (size < header || size > uint64_t(end - begin)) {
            return;
        }
        cb(std::string(begin + 4, 4), begin, static_cast<size_t>(size), header);
        begin += size;
    }
}

} // namespace

void MP4MuxerInterface::reviseDecodeTime(std::string &segment) const {
    bool need_revise = false;
    for (auto &pr : _tracks) {
        if (pr.second.start_dts != INT64_MIN && pr.second.start_dts != 0) {
            need_revise = true;
        }
    }
    if (!need_revise) {
        return;
    }

    auto data = &segment[0];
    forEachBox(data, data + segment.size(), [&](const std::string &type, char *moof, size_t size, size_t header) {
        if (type != "moof") {
            return;
        }
        forEachBox(moof + header, moof + size, [&](const std::string &type, char *traf, size_t size, size_t header) {
            if (type != "traf") {
                return;
            }
            int64_t start_dts = 0;
            forEachBox(traf + header, traf + size, [&](const std::string &type, char *box, size_t size, size_t header) {
                if (type == "tfhd" && size >= header + 8) {
                    // libmov numbers tkhd track_ID from 1 in the order the tracks were added
                    auto track_id = static_cast<int>(readU32(box + header + 4)) - 1;
                    for (auto &pr : _tracks) {
                        if (pr.second.track_id == track_id && pr.second.start_dts != INT64_MIN) {
                            start_dts = pr.second.start_dts;
                        }
                    }
                    return;
                }
                if (type != "tfdt" || start_dts <= 0 || size < header + 8) {
                    return;
                }
                auto version = static_cast<uint8_t>(box[header]);
                auto value = box + header + 4;
                if (version == 1 && size >= header + 12) {
                    writeU64(value, readU64(value) + start_dts);
                } else if (version == 0) {
                    uint64_t decode_time = uint64_t(readU32(value)) + start_dts;
                    if (decode_time > UINT32_MAX) {
                        WarnL << "tfdt overflows 32 bits after revise: " << decode_time;
                        return;
                    }
                    writeU32(value, static_cast<uint32_t>(decode_time));
                }
            });
        });
    });
}

void MP4MuxerInterface::stampSync() {
    Stamp *first = nullptr;
    for (auto &pr : _tracks) {
        if (!first) {
            first = &pr.second.stamp;
        } else {
            pr.second.stamp.syncTo(*first);
        }
    }
}

bool MP4MuxerInterface::addTrack(const Track::Ptr &track) {
    if (!_mov_writter) {
        _mov_writter = createWriter();
    }
    auto mp4_object = getMovIdByCodec(track->getCodecId());
    if (mp4_object == MOV_OBJECT_NONE) {
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
        auto track_id = mp4_writer_add_video(_mov_writter.get(), mp4_object, video_track->getVideoWidth(), video_track->getVideoHeight(), extra_data, extra_size);
        if (track_id < 0) {
            WarnL << "mp4_writer_add_video failed: " << video_track->getCodecName();
            return false;
        }
        _tracks[track->getIndex()].track_id = track_id;
        _have_video = true;
        _non_iframe_video_count = 0;
    } else if (track->getTrackType() == TrackAudio) {
        auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
        CHECK(audio_track);
        auto track_id = mp4_writer_add_audio(_mov_writter.get(), mp4_object, audio_track->getAudioChannel(), audio_track->getAudioSampleBit() * audio_track->getAudioChannel(), audio_track->getAudioSampleRate(), extra_data, extra_size);
        if (track_id < 0) {
            WarnL << "mp4_writer_add_audio failed: " << audio_track->getCodecName();
            return false;
        }
        _tracks[track->getIndex()].track_id = track_id;
    }

    // Try audio and video synchronization
    stampSync();
    return true;
}

/////////////////////////////////////////// MP4MuxerMemory /////////////////////////////////////////////

MP4MuxerMemory::MP4MuxerMemory() {
    _memory_file = std::make_shared<MP4FileMemory>();
}

MP4FileIO::Writer MP4MuxerMemory::createWriter() {
    return _memory_file->createWriter(MOV_FLAG_SEGMENT, true);
}

const string &MP4MuxerMemory::getInitSegment() {
    if (_init_segment.empty()) {
        initSegment();
        saveSegment();
        _init_segment = _memory_file->getAndClearMemory();
    }
    return _init_segment;
}

void MP4MuxerMemory::resetTracks() {
    MP4MuxerInterface::resetTracks();
    _memory_file = std::make_shared<MP4FileMemory>();
    _init_segment.clear();
}

bool MP4MuxerMemory::inputFrame(const Frame::Ptr &frame) {
    if (_init_segment.empty()) {
        // Init segment has not been generated yet
        return false;
    }

    // Flush segment
    saveSegment();

    auto data = _memory_file->getAndClearMemory();
    if (!data.empty()) {
        reviseDecodeTime(data);
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
    return MP4MuxerInterface::inputFrame(frame);
}

void MP4MuxerMemory::flush() {
    MP4MuxerInterface::flush();

    if (_init_segment.empty() || !_memory_file) {
        return;
    }
    saveSegment();

    auto data = _memory_file->getAndClearMemory();
    if (!data.empty()) {
        reviseDecodeTime(data);
        onSegmentData(std::move(data), _last_dst, _key_frame);
        _key_frame = false;
    }
}

} // namespace mediakit
#endif // defined(ENABLE_MP4)
