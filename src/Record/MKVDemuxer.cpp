#ifdef ENABLE_MKV

#include <algorithm>
#include "MKVDemuxer.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit { 

MKVDemuxer::~MKVDemuxer() {
    closeMKV();
}

void MKVDemuxer::openMKV(const string &file) {
    closeMKV();

    _mkv_file = std::make_shared<MKVFileDisk>();
    _mkv_file->openFile(file.data(), "rb+");
    _mkv_reader = _mkv_file->createReader();
    getAllTracks();
    _duration_ms = mkv_reader_getduration(_mkv_reader.get());
}

void MKVDemuxer::closeMKV() {
    _mkv_reader.reset();
    _mkv_file.reset();
}

int MKVDemuxer::getAllTracks() {
    static mkv_reader_trackinfo_t s_on_track = {
            [](void *param, uint32_t track, mkv_codec_t codec, int width, int height, const void *extra, size_t bytes) {
                //onvideo
                MKVDemuxer *thiz = (MKVDemuxer *)param;
                thiz->onVideoTrack(track,codec,width,height,extra,bytes);
            },
            [](void *param, uint32_t track, mkv_codec_t codec, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
                //onaudio
                MKVDemuxer *thiz = (MKVDemuxer *)param;
                thiz->onAudioTrack(track,codec,channel_count,bit_per_sample,sample_rate,extra,bytes);
            },
            [](void *param, uint32_t track, mkv_codec_t codec, const void *extra, size_t bytes) {
                //onsubtitle, do nothing
            }
    };
    return mkv_reader_getinfo(_mkv_reader.get(),&s_on_track,this);
}

void MKVDemuxer::onVideoTrack(uint32_t track, mkv_codec_t codec, int width, int height, const void *extra, size_t bytes) {
    auto video = Factory::getTrackByCodecId(getCodecByMkvId(codec));
    if (!video) {
        return;
    }
    video->setIndex(track);
    _tracks.emplace(track, video);
    if (extra && bytes) {
        video->setExtraData((uint8_t *)extra, bytes);
    }
}

void MKVDemuxer::onAudioTrack(uint32_t track, mkv_codec_t codec,int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes) {
    auto audio = Factory::getTrackByCodecId(getCodecByMkvId(codec), sample_rate, channel_count, bit_per_sample / channel_count);
    if (!audio) {
        return;
    }
    audio->setIndex(track);
    _tracks.emplace(track, audio);
    if (extra && bytes) {
        audio->setExtraData((uint8_t *)extra, bytes);
    }
}

int64_t MKVDemuxer::seekTo(int64_t stamp_ms) {
    if(0 != mkv_reader_seek(_mkv_reader.get(),&stamp_ms)){
        return -1;
    }
    return stamp_ms;
}

struct Context {
    Context(MKVDemuxer *ptr) : thiz(ptr) {}
    MKVDemuxer *thiz;
    int flags = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    uint32_t track_id = 0;
    BufferRaw::Ptr buffer;
};

Frame::Ptr MKVDemuxer::readFrame(bool &keyFrame, bool &eof) {
    keyFrame = false;
    eof = false;

    static mkv_reader_onread2 mkv_onalloc = [](void *param, uint32_t track_id, size_t bytes, int64_t pts, int64_t dts, int flags) -> void * {
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
    auto ret = mkv_reader_read2(_mkv_reader.get(), mkv_onalloc, &ctx);
    switch (ret) {
        case 0 : {
            eof = true;
            return nullptr;
        }

        case 1 : {
            keyFrame = ctx.flags & MKV_FLAGS_KEYFRAME;
            return makeFrame(ctx.track_id, ctx.buffer, ctx.pts, ctx.dts);
        }

        default : {
            eof = true;
            WarnL << "Failed to read mkv file data:" << ret;
            return nullptr;
        }
    }
}

Frame::Ptr MKVDemuxer::makeFrame(uint32_t track_id, const Buffer::Ptr &buf, int64_t pts, int64_t dts) {
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

vector<Track::Ptr> MKVDemuxer::getTracks(bool ready) const {
    vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (ready && !pr.second->ready()) {
            continue;
        }
        ret.push_back(pr.second);
    }
    return ret;
}

uint64_t MKVDemuxer::getDurationMS() const {
    return _duration_ms;
}

/////////////////////////////////////////////////////////////////////////////////

void MultiMKVDemuxer::openMKV(const string &files_string) {
    std::vector<std::string> files;
    if (File::is_dir(files_string)) {
        File::scanDir(files_string, [&](const string &path, bool is_dir) {
            if (!is_dir) {
                files.emplace_back(path);
            }
            return true;
        });
        std::sort(files.begin(), files.end());
    } else {
        files = split(files_string, ";");
    }

    uint64_t duration_ms = 0;
    for (auto &file : files) {
        auto demuxer = std::make_shared<MKVDemuxer>();
        demuxer->openMKV(file);
        _demuxers.emplace(duration_ms, demuxer);
        duration_ms += demuxer->getDurationMS();
    }
    CHECK(!_demuxers.empty());
    _it = _demuxers.begin();
    for (auto &track : _it->second->getTracks(false)) {
        _tracks.emplace(track->getIndex(), track->clone());
    }
}

uint64_t MultiMKVDemuxer::getDurationMS() const {
    return _demuxers.empty() ? 0 : _demuxers.rbegin()->first + _demuxers.rbegin()->second->getDurationMS();
}

void MultiMKVDemuxer::closeMKV() {
    _demuxers.clear();
    _it = _demuxers.end();
    _tracks.clear();
}

int64_t MultiMKVDemuxer::seekTo(int64_t stamp_ms) {
    if (stamp_ms >= (int64_t)getDurationMS()) {
        return -1;
    }
    _it = std::prev(_demuxers.upper_bound(stamp_ms));
    return _it->first + _it->second->seekTo(stamp_ms - _it->first);
}

Frame::Ptr MultiMKVDemuxer::readFrame(bool &keyFrame, bool &eof) {
    for (;;) {
        auto ret = _it->second->readFrame(keyFrame, eof);
        if (ret) {
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

std::vector<Track::Ptr> MultiMKVDemuxer::getTracks(bool trackReady) const {
    std::vector<Track::Ptr> ret;
    for (auto &pr : _tracks) {
        if (!trackReady || pr.second->ready()) {
            ret.emplace_back(pr.second);
        }
    }
    return ret;
}

} // namespace mediakit

#endif // ENABLE_MKV