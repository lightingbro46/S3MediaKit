#ifdef ENABLE_MP4

#include <algorithm>
#include "MP4Demuxer.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Extension/Factory.h"
#include "Common/config.h"
#include "Common/Parser.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

MP4Demuxer::~MP4Demuxer() {
    closeMP4();
}

void MP4Demuxer::openMP4(const string &file) {
    closeMP4();

    _mp4_file = std::make_shared<MP4FileDisk>();
    _mp4_file->openFile(file.data(), "rb+");
    _mov_reader = _mp4_file->createReader();
    getAllTracks();
    _duration_ms = mov_reader_getduration(_mov_reader.get());
}

void MP4Demuxer::closeMP4() {
    _mov_reader.reset();
    _mp4_file.reset();
}

int MP4Demuxer::getAllTracks() {
    static mov_reader_trackinfo_t s_on_track = {
            [](void *param, uint32_t track, uint8_t object, int width, int height, const void *extra, size_t bytes) {
                //onvideo
                MP4Demuxer *thiz = (MP4Demuxer *)param;
                thiz->onVideoTrack(track,object,width,height,extra,bytes);
            },
            [](void *param, uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
                //onaudio
                MP4Demuxer *thiz = (MP4Demuxer *)param;
                thiz->onAudioTrack(track,object,channel_count,bit_per_sample,sample_rate,extra,bytes);
            },
            [](void *param, uint32_t track, uint8_t object, const void *extra, size_t bytes) {
                //onsubtitle, do nothing
            }
    };
    return mov_reader_getinfo(_mov_reader.get(),&s_on_track,this);
}

void MP4Demuxer::onVideoTrack(uint32_t track, uint8_t object, int width, int height, const void *extra, size_t bytes) {
    auto video = Factory::getTrackByCodecId(getCodecByMovId(object));
    if (!video) {
        return;
    }
    video->setIndex(track);
    _tracks.emplace(track, video);
    if (extra && bytes) {
        video->setExtraData((uint8_t *)extra, bytes);
    }
}

void MP4Demuxer::onAudioTrack(uint32_t track, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
    auto audio = Factory::getTrackByCodecId(getCodecByMovId(object), sample_rate, channel_count, bit_per_sample / channel_count);
    if (!audio) {
        return;
    }
    audio->setIndex(track);
    _tracks.emplace(track, audio);
    if (extra && bytes) {
        audio->setExtraData((uint8_t *)extra, bytes);
    }
}

int64_t MP4Demuxer::seekTo(int64_t stamp_ms) {
    if(0 != mov_reader_seek(_mov_reader.get(),&stamp_ms)){
        return -1;
    }
    return stamp_ms;
}

struct Context {
    Context(MP4Demuxer *ptr) : thiz(ptr) {}
    MP4Demuxer *thiz;
    int flags = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t track_id = 0;
    BufferRaw::Ptr buffer;
};

Frame::Ptr MP4Demuxer::readFrame(bool &keyFrame, bool &eof) {
    keyFrame = false;
    eof = false;

    static mov_reader_onread2 mov_onalloc = [](void *param, uint32_t track_id, size_t bytes, int64_t pts, int64_t dts, int flags) -> void * {
        Context *ctx = (Context *) param;
        ctx->pts = pts;
        ctx->dts = dts;
        ctx->flags = flags;
        ctx->track_id = track_id;

        ctx->buffer = ctx->thiz->_buffer_pool.obtain2();
        ctx->buffer->setCapacity(bytes + 1);
        ctx->buffer->setSize(bytes);
        return ctx->buffer->data();
    };

    Context ctx(this);
    auto ret = mov_reader_read2(_mov_reader.get(), mov_onalloc, &ctx);
    switch (ret) {
        case 0 : {
            eof = true;
            return nullptr;
        }

        case 1 : {
            keyFrame = ctx.flags & MOV_AV_FLAG_KEYFREAME;
            return makeFrame(ctx.track_id, ctx.buffer, ctx.pts, ctx.dts);
        }

        default : {
            eof = true;
            WarnL << "Failed to read mp4 file data:" << ret;
            return nullptr;
        }
    }
}

Frame::Ptr MP4Demuxer::makeFrame(uint32_t track_id, Buffer::Ptr buf, int64_t pts, int64_t dts) {
    auto it = _tracks.find(track_id);
    if (it == _tracks.end()) {
        return nullptr;
    }
    Frame::Ptr ret;
    auto codec = it->second->getCodecId();
    switch (codec) {
        case CodecH264:
        case CodecH265: {
            auto bytes = buf->size();
            auto data = buf->data();
            auto offset = 0u;
            while (offset < bytes) {
                uint32_t frame_len;
                memcpy(&frame_len, data + offset, 4);
                frame_len = ntohl(frame_len);
                if (frame_len + offset + 4 > bytes) {
                    return nullptr;
                }
                memcpy(data + offset, "\x00\x00\x00\x01", 4);
                offset += (frame_len + 4);
            }
            ret = Factory::getFrameFromBuffer(codec, std::move(buf), dts, pts);
            break;
        }

        default: {
            ret = Factory::getFrameFromBuffer(codec, std::move(buf), dts, pts);
            break;
        }
    }
    if (ret) {
        ret->setIndex(track_id);
        it->second->inputFrame(ret);
    }
    return ret;
}

vector<Track::Ptr> MP4Demuxer::getTracks(bool ready) const {
    vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (ready && !pr.second->ready()) {
            continue;
        }
        ret.push_back(pr.second);
    }
    return ret;
}

uint64_t MP4Demuxer::getDurationMS() const {
    return _duration_ms;
}

/////////////////////////////////////////////////////////////////////////////////

void MultiMP4Demuxer::openMP4(const string &files_string) {
    if (files_string.find("/vod/") != string::npos) {
        _use_timeline = true;
        openMP4WithTimeline(files_string);
        return;
    }

    std::vector<std::string> files;
    if (File::is_dir(files_string)) {
        File::scanDir(files_string, [&](const string &path, bool is_dir) {
            if (!is_dir && end_with(path, ".mp4")) {
                files.emplace_back(path);
            }
            return true;
        }, true);
        std::sort(files.begin(), files.end());
    } else {
        files = split(files_string, ";");
    }

    uint64_t duration_ms = 0;
    for (auto &file : files) {
        auto demuxer = std::make_shared<MP4Demuxer>();
        demuxer->openMP4(file);
        _demuxers.emplace(duration_ms, demuxer);
        duration_ms += demuxer->getDurationMS();
    }
    CHECK(!_demuxers.empty());
    _it = _demuxers.begin();
    for (auto &track : _it->second->getTracks(false)) {
        auto clone_track(track->clone());
        clone_track->setIndex(clone_track->getTrackType());
        _tracks.emplace(clone_track->getIndex(), clone_track);
        DebugL << "track index: " << track->getIndex() << " -> " << clone_track->getIndex();
    }
}

uint64_t MultiMP4Demuxer::getDurationMS() const {
    if (_use_timeline) {
        return _stats.total_dur * 1000;
    }
    return _demuxers.empty() ? 0 : _demuxers.rbegin()->first + _demuxers.rbegin()->second->getDurationMS();
}

void MultiMP4Demuxer::closeMP4() {
    _demuxers.clear();
    _it = _demuxers.end();
    _tracks.clear();
}

int64_t MultiMP4Demuxer::seekTo(int64_t stamp_ms) {
    if (_use_timeline) {
        return seekToWithTimeline(stamp_ms);
    }
    if (stamp_ms >= (int64_t)getDurationMS()) {
        return -1;
    }
    _it = std::prev(_demuxers.upper_bound(stamp_ms));
    return _it->first + _it->second->seekTo(stamp_ms - _it->first);
}

Frame::Ptr MultiMP4Demuxer::readFrame(bool &keyFrame, bool &eof) {
    if (_use_timeline) {
        return readFrameWithTimeline(keyFrame, eof);
    }
    for (;;) {
        auto ret = _it->second->readFrame(keyFrame, eof);
        if (ret) {
            ret->setIndex(ret->getTrackType());
            auto it = _tracks.find(ret->getIndex());
            if (it != _tracks.end()) {
                auto ret2 = std::make_shared<FrameStamp>(ret);
                ret2->setStamp(_it->first + ret->dts(), _it->first + ret->pts());
                ret = std::move(ret2);
                it->second->inputFrame(ret);
            }
        }
        if (eof && _it != _demuxers.end()) {
            // Switch to the next file
            if (++_it == _demuxers.end()) {
                // It's the last file
                eof = true;
                return nullptr;
            }
            // The next file starts from scratch
            _it->second->seekTo(0);
            continue;
        }
        return ret;
    }
}

std::vector<Track::Ptr> MultiMP4Demuxer::getTracks(bool trackReady) const {
    std::vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (!trackReady || pr.second->ready()) {
            ret.emplace_back(pr.second);
        }
    }
    return ret;
}

static uint64_t findSegmentFiles(map<uint64_t, string> &files, const MediaTuple &tuple, const uint64_t &stamp, const uint64_t &max_duration = 300) {
    uint64_t duration = 0;
    map<string, map<uint64_t, string>> multi_files;
    Broadcast::Seek2Invoker invoker = [&](const uint64_t &duration_, const map<string, map<uint64_t, string>> &ret) {
        duration = duration_;
        multi_files = ret;
    };
    auto flag = NOTICE_EMIT(BroadcastMediaSeeked2Args, Broadcast::kBroadcastMediaSeeked2, tuple, stamp, max_duration, invoker);
    if (!flag) {
        // No one is listening to this event
    }
    // Flatten: prefer stream that matches tuple.stream; otherwise take the first
    if (!multi_files.empty()) {
        auto it = tuple.stream.empty() ? multi_files.begin() : multi_files.find(tuple.stream);
        if (it == multi_files.end()) it = multi_files.begin();
        files = it->second;
    }
    return duration;
}

static uint64_t findSegmentDuration(const MediaTuple &tuple, const uint64_t &stamp, const uint64_t &max_duration = 3600) {
    uint64_t duration = 0;
    Broadcast::Seek2Invoker invoker = [&](const uint64_t &ret, const map<string, map<uint64_t, string>> &) {
        duration = ret;
    };
    auto flag = NOTICE_EMIT(BroadcastMediaSeeked2Args, Broadcast::kBroadcastMediaSeeked2, tuple, stamp, max_duration, invoker);
    if (!flag) {
        // No one is listening to this event
    }
    return duration;
}

void MultiMP4Demuxer::openMP4WithTimeline(const std::string &file_path) {
    auto prefix_path = findSubString(file_path.data(), nullptr, "/vod");
    auto suffix_path = findSubString(file_path.data(), "vod/", nullptr);

    auto tmp = split(prefix_path, "/");
    auto app = tmp[tmp.size() - 2];
    auto stream = tmp[tmp.size() - 1];

    CHECK(!app.empty() && !stream.empty());
    MediaTuple tuple = { DEFAULT_VHOST, app, stream, "" };
    uint64_t start_time = stoll(suffix_path.data());

    auto total_duration = findSegmentDuration(tuple, start_time);
    int64_t offset = 0;

    if (total_duration > 0) {
        _stats.tuple = tuple;
        _stats.start_time = start_time;
        _stats.total_dur = total_duration;
        offset = findNextSegment(true);
    }

    CHECK(!_demuxers.empty());
    _it = _demuxers.begin();
    for (auto &track : _it->second->getTracks(false)) {
        auto clone_track(track->clone());
        clone_track->setIndex(clone_track->getTrackType());
        _tracks.emplace(clone_track->getIndex(), clone_track);
        DebugL << "track index: " << track->getIndex() << " -> " << clone_track->getIndex();
    }

    if (offset >= 0) {
        _it->second->seekTo(offset * 1000);
    }
}

int64_t MultiMP4Demuxer::findNextSegment(bool first_segment, uint64_t max_duration) {
    // clear map
    if (_demuxers.size()) {
        _demuxers.clear();
        _it = _demuxers.end();
    }
    
    uint64_t start_segment = first_segment ? _stats.start_time : _stats.next_time;
    uint64_t next_time = 0;

    if (start_segment <= 0) {
        return -1;
    }

    map<uint64_t, string> files;
    auto duration = findSegmentFiles(files, _stats.tuple, start_segment, max_duration * 2);
    if (duration <= 0) {
        return -1;
    }
    uint64_t offset = 0;
    uint64_t duration_ms = 0;
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (it == files.begin()) {
            offset = (start_segment >= it->first) ? (start_segment - it->first) : 0;
            if (first_segment) {
                _stats.first_time = it->first;
            } else {    
                duration_ms = (it->first - _stats.first_time) * 1000;
            }
        } else if (it->first - start_segment >= max_duration) {
            break;
        }
        auto demuxer = std::make_shared<MP4Demuxer>();
        demuxer->openMP4(it->second);
        _demuxers.emplace(duration_ms, demuxer);
        duration_ms += demuxer->getDurationMS();
        next_time = it->first + static_cast<uint64_t>(demuxer->getDurationMS() / 1000);
    }
    
    _stats.next_time = next_time;
    return offset;
}

int64_t MultiMP4Demuxer::seekToWithTimeline(int64_t stamp_ms) {
    if (stamp_ms >= (int64_t)getDurationMS()) {
        return -1;
    }
    _stats.next_time = _stats.start_time + stamp_ms / 1000;
    auto offset = findNextSegment();
    if (offset < 0) {
        return -1;
    }
    _it = _demuxers.begin();
    auto diff_time_ms = (_stats.start_time - _stats.first_time) * 1000;
    auto offset_ms = offset * 1000;
    return _it->first - diff_time_ms +_it->second->seekTo(offset_ms);
}

Frame::Ptr MultiMP4Demuxer::readFrameWithTimeline(bool &keyFrame, bool &eof) {
    for (;;) {
        auto ret = _it->second->readFrame(keyFrame, eof);
        if (ret) {
            ret->setIndex(ret->getTrackType());
            auto it = _tracks.find(ret->getIndex());
            if (it != _tracks.end()) {
                auto ret2 = std::make_shared<FrameStamp>(ret);
                ret2->setStamp(_it->first + ret->dts(), _it->first + ret->pts());
                ret = std::move(ret2);
                it->second->inputFrame(ret);
            } 
        }
        if (eof && _it != _demuxers.end()) {
            // Switch to the next file
            if (++_it == _demuxers.end()) {
                // Find next segment
                auto offset = findNextSegment();
                if (offset < 0) {
                    // It's the last file
                    eof = true;
                    return nullptr;
                }
                _it = _demuxers.begin();
            }
            // The next file starts from scratch
            _it->second->seekTo(0);
            continue;
        }
        return ret;
    }
}

}//namespace mediakit
#endif// ENABLE_MP4
