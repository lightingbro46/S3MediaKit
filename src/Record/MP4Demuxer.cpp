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

void MultiMP4Demuxer::openMP4(const string &files_string, const string &params) {
    if (files_string.find("/vod/") != string::npos) {
        _use_timeline = true;
        openMP4WithTimeline(files_string, params);
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
    _demuxer_stop_offsets.clear();
    _demuxer_start_cuts.clear();
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

static int64_t findSegmentBatch(map<string, map<uint64_t, string>> &files, const MediaTuple& tuple, uint64_t stamp, uint64_t max_duration) {
    // Query for the next segment starting from start_time, and add it to _demuxers if found
    // Returns the start time of the next segment, or -1 if no more segment is found
    uint64_t duration = 0;
    Broadcast::Seek2Invoker invoker = [&](const uint64_t &duration_, const map<string, map<uint64_t,string>> &ret) {
        duration = duration_;
        files = ret;
    };
    auto flag = NOTICE_EMIT(BroadcastMediaSeeked2Args, Broadcast::kBroadcastMediaSeeked2, tuple, stamp, max_duration, invoker);
    if (!flag) {
        // No one is listening to this event
        WarnL << "No listener for BroadcastMediaSeeked2 event, cannot find segment batch for tuple=" << tuple.shortUrl() << " stamp=" << stamp;
    }
    return duration;
}

void MultiMP4Demuxer::openMP4WithTimeline(const std::string &file_path, const std::string &params) {
    auto prefix_path = findSubString(file_path.data(), nullptr, "/vod");
    auto suffix_path = findSubString(file_path.data(), "vod/", nullptr);

    auto tmp = split(prefix_path, "/");
    string app = tmp[tmp.size() - 2];
    string stream = tmp[tmp.size() - 1];
    uint64_t start_time = stoll(suffix_path.data());

    GET_CONFIG(string, app_name, Record::kAppName);
    if (app == app_name) {
        // URL was record/{app}/vod/{stamp} — stream segment lives under app
        app = stream;
        stream = "";
    }
    CHECK(!app.empty());
    _stats.app = app;
    _stats.start_time = start_time;

    if (!params.empty()) {
        auto kv = Parser::parseArgs(params);
        if (kv.find("quality") != kv.end()) {
            auto quality = kv["quality"];
            if (quality == "hi" || quality == "lo") {
                _stats.quality = quality;
            }
        }
        if (kv.find("prefered") != kv.end()) {
            auto prefered = kv["prefered"];
            if (prefered == "hi" || prefered == "lo") {
                _stats.prefered = prefered;
            }
        }
    }

    // Step 1: discover hi/lo stream IDs for this camera
    if (stream.empty()) {
        // If stream was not explicit in the URL, query for hi/lo stream IDs (multi-stream mode)
        Broadcast::StreamQualityInvoker invoker = [&](const map<int, string> &m) {
            auto it_hi = m.find(0); // PrimaryStream
            if (it_hi != m.end()) _stats.hi_stream = it_hi->second;
            auto it_lo = m.find(1); // SecondaryStream
            if (it_lo != m.end()) _stats.lo_stream = it_lo->second;
        };
        string vhost = DEFAULT_VHOST;
        NOTICE_EMIT(BroadcastGetStreamQualityArgs, Broadcast::kBroadcastGetStreamQuality, vhost, app, invoker);
    } else {
        // If stream was explicit in the URL, pin to it (single-stream mode)
        _stats.hi_stream = stream;
        _stats.lo_stream = "";
        _stats.quality = "hi";
    }

    // Step 2: find the first batch of segments starting from start_time
    // Use stream-level query for duration (stream="" is app-level query, covers all streams; duration is
    // the merged TimeRange span so it is NOT double-counted across hi/lo streams
    {
        MediaTuple tuple = { DEFAULT_VHOST, app, stream, "" };
        map<string, map<uint64_t, string>> files;
        auto total_duration = findSegmentBatch(files, tuple, start_time, 3600);
        DebugL << "Found initial segment batch for tuple=" << tuple.shortUrl() << " start=" << start_time << " total_duration=" << total_duration;
        if (total_duration > 0) {
            _stats.tuple = tuple;
            _stats.total_dur = total_duration;
            prefetchNextSegmentBatch(tuple, start_time, 300);
        }
        if (_next_batch.ready) {
            // If prefetch is already ready (e.g. no listener or very fast response), we can open the first batch immediately
           openNextSegmentBatch();
        }
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

// ---------------------------------------------------------------------------
// Opens each unique file path once, reads ms-accurate duration and caches the
// opened MP4Demuxer in entry.demuxer so openSegmentDemuxers can reuse it
// without reopening.
// ---------------------------------------------------------------------------
static uint64_t loadAllSegment(vector<SegmentEntry> &raw_segs) {
    uint64_t duration_ms = 0;
    for (auto &entry : raw_segs) {
        if (entry.file_path.empty()) {
            continue;
        }
        auto demuxer = std::make_shared<MP4Demuxer>();
        demuxer->openMP4(entry.file_path);
        entry.dur_ms = demuxer->getDurationMS();
        entry.demuxer = demuxer;
        entry.start_cut = 0;
        entry.stop_offset = entry.dur_ms;
        // Note: duration_ms is the sum of all segments' duration, except start_cut/stop_cut
        // the actual played duration may be shorter after applying start_cut/stop_cut, but that is handled in readFrameWithTimeline
        duration_ms += entry.dur_ms;
    }
    return duration_ms;
}

std::vector<SegmentEntry> buildSortedSegmentList(map<string, map<uint64_t, string>> &files, const string &hi_stream, const string &lo_stream, const string &quality, const string &prefered, const uint64_t &start_time) {
    // ------------------------------------------------------------------
    // 1. Extract raw hi / lo lists (stamp-sorted, dur=0 yet)
    // files layout: stream_id → { unix_ts_sec → file_path }
    // ------------------------------------------------------------------
    auto extractStream = [&](const string &stream_id, const string &qual) {
        vector<SegmentEntry> out;
        auto sit = files.find(stream_id);
        if (sit == files.end()) return out;
        for (auto &tv : sit->second) {   // tv.first = unix_ts, tv.second = path
            SegmentEntry e;
            e.stamp     = tv.first;
            e.quality   = qual;
            e.file_path = tv.second;
            out.push_back(move(e));
        }
        // inner map is std::map so already sorted by ts
        return out;
    };

    // ------------------------------------------------------------------
    // 2. Single-quality filter (no gap-fill needed)
    // ------------------------------------------------------------------
    if (quality == "hi" || quality == "lo") {
        string stream_id  = (quality == "hi") ? hi_stream : lo_stream;
        vector<SegmentEntry> result = extractStream(stream_id, quality);
        loadAllSegment(result);
        return result;
    }

    // ------------------------------------------------------------------
    // 3. quality == "auto" : gap-fill merge
    // ------------------------------------------------------------------
    vector<SegmentEntry> hi_list = extractStream(hi_stream, "hi");
    vector<SegmentEntry> lo_list = extractStream(lo_stream, "lo");

    // Load ms-accurate durations for both lists
    loadAllSegment(hi_list);
    loadAllSegment(lo_list);

    // Helper: end time of a segment in unix-ms (stamp is seconds → *1000 + dur_ms)
    auto getSegEndMs = [](const SegmentEntry &e) -> uint64_t {
        return e.stamp * 1000 + e.dur_ms;
    };
    // ------------------------------------------------------------------
    // 3a. Find hi files and create trimmed
    //     SegmentEntries with start_cut / stop_cut set.
    //     start_cut is IDR-probed; stop_cut is a plain ms boundary.
    // ------------------------------------------------------------------
    vector<SegmentEntry> hi_fill;
    for (auto &entry : hi_list) {
        uint64_t seg_start_ms = entry.stamp * 1000;
        uint64_t seg_end_ms = getSegEndMs(entry);
        if (seg_end_ms <= start_time * 1000 + 1000) { // allow 1s gap tolerance to avoid over-fragmentation; also skip if no overlap at all
            // No overlap
            continue;
        }
        SegmentEntry filled = entry; // copy
        // Trim to start_time boundary
        uint64_t raw_start_cut_ms = (start_time * 1000 > seg_start_ms) ? (start_time * 1000 - seg_start_ms) : 0;
        if (raw_start_cut_ms > 0 && filled.demuxer) {
            auto idr_cut = entry.demuxer->seekTo(raw_start_cut_ms);
            if (idr_cut >= 0) {
                filled.start_cut = idr_cut;
            } else {
                // IDR probe failed, fallback to raw ms cut
                filled.start_cut = raw_start_cut_ms;
            }
        } else {
            filled.start_cut = 0;
        }
        // stop_cut is not needed for hi files since they are the preferred quality and will not be cut short by any following file; 
        // also simplifies gap-fill logic since we only need to consider lo files' stop_cut when filling gaps in hi timeline
        filled.stop_offset = filled.dur_ms;
        // Safety: seekTo may snap start_cut FORWARD past the stop boundary
        // (e.g. next IDR is after gap.end_ms).  If so, this fill window has
        // no decodable content — discard rather than causing unsigned underflow
        // in (stop_cut - start_cut) downstream.
        if (filled.start_cut >=  filled.stop_offset - 1000) { // allow 1s tolerance to avoid over-fragmentation; also skip if no usable content after cut
            // No usable content after cut, skip this file
            continue;
        }
        hi_fill.push_back(move(filled));
    }

    // ------------------------------------------------------------------
    // 3b. Build gaps from the hi timeline
    //     A gap is [gap_start_ms, gap_end_ms) in unix-ms where no hi file
    //     provides coverage.
    // ------------------------------------------------------------------
    struct Gap { uint64_t start_ms; uint64_t end_ms; };
    vector<Gap> gaps;
    if (hi_fill.empty()) {
        // Entire range is a gap; fill with lo
        if (!lo_list.empty()) {
            uint64_t gstart = lo_list.front().stamp > start_time ? (lo_list.front().stamp * 1000) : (start_time * 1000);
            uint64_t gend   = getSegEndMs(lo_list.back());
            gaps.push_back({ gstart, gend });
        }
    } else {
        // Gaps before the first hi file
        if (!lo_list.empty() && lo_list.front().stamp * 1000 < hi_fill.front().stamp * 1000) {
            gaps.push_back({ lo_list.front().stamp > start_time ? (lo_list.front().stamp * 1000) : (start_time * 1000), hi_fill.front().stamp * 1000 });
        }
        // Gaps between consecutive hi files
        for (size_t i = 0; i + 1 < hi_fill.size(); ++i) {
            uint64_t end_current = getSegEndMs(hi_fill[i]);
            uint64_t start_next = hi_fill[i + 1].stamp * 1000;
            if (end_current < start_next - 1000) { // allow 1s gap tolerance to avoid over-fragmentation
                gaps.push_back({ end_current, start_next });
            }
        }
        // Gaps after the last hi file (only if lo extends beyond)
        if (!lo_list.empty()) {
            uint64_t end_last_hi = getSegEndMs(hi_fill.back());
            uint64_t end_last_lo = getSegEndMs(lo_list.back());
            if (end_last_lo > end_last_hi + 1000) { // allow 1s gap tolerance
                gaps.push_back({ end_last_hi, end_last_lo });
            }
        }
    }

    // ------------------------------------------------------------------
    // 3c. For each gap, find lo files that overlap it and create trimmed
    //     SegmentEntries with start_cut / stop_cut set.
    //     start_cut is IDR-probed; stop_cut is a plain ms boundary.
    // ------------------------------------------------------------------
    vector<SegmentEntry> lo_fill;
    for (auto &gap : gaps) {
        for (auto &entry : lo_list) {
            uint64_t seg_start_ms = entry.stamp * 1000;
            uint64_t seg_end_ms = getSegEndMs(entry);
            if (seg_end_ms <= gap.start_ms + 1000 || seg_start_ms >= gap.end_ms - 1000) { // allow 1s gap tolerance to avoid over-fragmentation; also skip if no overlap at all
                // No overlap
                continue;
            }
            SegmentEntry filled = entry; // copy
            // Trim to gap boundaries
            // start_cut: offset into the lo file (ms).  IDR-probe to find the
            // nearest IDR that the decoder can use immediately.
            uint64_t raw_start_cut_ms = (gap.start_ms > seg_start_ms) ? (gap.start_ms - seg_start_ms) : 0; // todo: allow 500ms pre-roll for better IDR probe accuracy?
            if (raw_start_cut_ms > 0 && filled.demuxer) {
                auto idr_cut = entry.demuxer->seekTo(raw_start_cut_ms);
                if (idr_cut >= 0) {
                    filled.start_cut = idr_cut;
                } else {
                    // IDR probe failed, fallback to raw ms cut
                    filled.start_cut = raw_start_cut_ms;
                }
            } else {
                filled.start_cut = 0;
            }
            DebugL << "Gap fill: IDR probe for file " << entry.file_path << " raw_start_cut=" << raw_start_cut_ms << "ms -> idr_cut=" << filled.start_cut << "ms";
            // stop_cut: end of the useful portion (ms into the lo file).
            // No backward IDR probe needed — the file that follows starts with
            // its own IDR (its start_cut is IDR-aligned or it starts at 0).
            uint64_t raw_stop_cut_ms = (gap.end_ms < seg_end_ms) ? (gap.end_ms - seg_start_ms) : filled.dur_ms;
            filled.stop_offset = raw_stop_cut_ms;
            DebugL << "Gap fill: stop cut for file " << entry.file_path << " raw_stop_cut=" << raw_stop_cut_ms << "ms";
            // Safety: seekTo may snap start_cut FORWARD past the stop boundary
            // (e.g. next IDR is after gap.end_ms).  If so, this fill window has
            // no decodable content — discard rather than causing unsigned underflow
            // in (stop_cut - start_cut) downstream.
            if (filled.start_cut >=  filled.stop_offset - 1000) { // allow 1s tolerance to avoid over-fragmentation; also skip if no usable content after cut
                // No usable content after cut, skip this file
                WarnL << "Gap fill: no usable content after cut for file " << entry.file_path << " start_cut=" << filled.start_cut << "ms stop_cut=" << filled.stop_offset << "ms, skipping this file";
                continue;
            }
            lo_fill.push_back(move(filled));
        }
    }

    // ------------------------------------------------------------------
    // 3d. Merge hi + lo_fill, sort by: start time in unix-ms
    //     For hi: start = stamp * 1000
    //     For lo fill: start = stamp * 1000 + start_cut
    // ------------------------------------------------------------------
    vector<SegmentEntry> result;
    result.insert(result.end(), hi_fill.begin(), hi_fill.end());
    result.insert(result.end(), lo_fill.begin(), lo_fill.end());

    // For hi files: start_cut=0 (always from beginning), stop_cut=0 (natural EOF)
    // (already default 0)
    sort(result.begin(), result.end(), [](const SegmentEntry &a, const SegmentEntry &b) {
        uint64_t a_start = a.stamp * 1000 + a.start_cut;
        uint64_t b_start = b.stamp * 1000 + b.start_cut;
        return a_start < b_start;
    });

    // ── Diagnostic log: sorted segment list ──────────────────────────────────
    DebugL << "[MultiMP4] buildSortedSegmentList: " << result.size() << " entries";
    for (size_t i = 0; i < result.size(); ++i) {
        const auto &e = result[i];
        uint64_t eff_stop = (e.stop_offset > 0) ? e.stop_offset : e.dur_ms;
        // Extract just the filename for readability
        auto slash = e.file_path.rfind('/');
        auto fname = (slash != string::npos) ? e.file_path.substr(slash + 1) : e.file_path;
        DebugL << "[MultiMP4]  [" << i << "] " << e.quality
              << " stamp=" << e.stamp << "s"
              << " dur=" << e.dur_ms << "ms"
              << " start_cut=" << e.start_cut << "ms"
              << " stop_cut=" << e.stop_offset << "ms"
              << "  " << fname;
    }

    return result;
}

void MultiMP4Demuxer::prefetchNextSegmentBatch(const MediaTuple &tuple, uint64_t start_time, uint64_t max_duration, bool seek) {
    _next_batch.reset();
    // Query next batch of files (all streams, ~5 min window)
    map<string, map<uint64_t, string>> files;
    auto duration = findSegmentBatch(files, tuple, start_time, max_duration);
    if (duration == 0 || files.empty()) {
        // No more segment found, do nothing
        _next_batch.ready = true;
        return;
    }

    // -------------------------------------------------------------------
    // Build sorted, quality-filtered segment list
    // -------------------------------------------------------------------
    auto all_segs = buildSortedSegmentList(files, _stats.hi_stream, _stats.lo_stream, _stats.quality, _stats.prefered, start_time);
    if (all_segs.empty()) {
        // No valid segment found after processing, do nothing
        _next_batch.ready = true;
        return;
    }
    // Note: all_segs is sorted by effective start time (after applying start_cut)
    uint64_t last_seg_end_ms = 0;
    if (seek) {
        // for seek, next batch should start from the seek position (start_time) rather than the end of the last batch
        last_seg_end_ms = (start_time - _stats.start_time) * 1000;
    } else if (!_demuxers.empty()) {
        auto first_next_seg = all_segs.begin();
        auto last_cur_seg = _current_batch.back();
        if (first_next_seg->file_path == last_cur_seg.file_path) {
            // update stop_offset for last current segment
            last_cur_seg.stop_offset = first_next_seg->stop_offset;
            _demuxer_stop_offsets[_demuxers.rbegin()->first] = first_next_seg->stop_offset;
            // remove duplicate segment
            all_segs.erase(first_next_seg);
        } 
        last_seg_end_ms = _demuxers.rbegin()->first + _demuxer_stop_offsets[_demuxers.rbegin()->first] - _demuxer_start_cuts[_demuxers.rbegin()->first];
    }
    auto next_seg_start_ms = last_seg_end_ms;
    for (auto &entry : all_segs) {
        if (entry.file_path.empty()) {
            continue;
        }
        _next_batch.demuxers.emplace(last_seg_end_ms, entry.demuxer);
        _next_batch.start_cuts.emplace(last_seg_end_ms, entry.start_cut);
        _next_batch.stop_offsets.emplace(last_seg_end_ms, entry.stop_offset);

        last_seg_end_ms += entry.stop_offset - entry.start_cut; // next file starts after the effective content of this file (after applying cuts)
    }
    _next_batch.segments = std::move(all_segs);
    _next_batch.ready = true;

    DebugL << "Prefetched next segment batch: " << _next_batch.demuxers.size() << " files, total duration ~" << (last_seg_end_ms - next_seg_start_ms) << "ms";
}

void MultiMP4Demuxer::openNextSegmentBatch() {
    if (!_next_batch.ready) {
        // Next batch is not ready yet, cannot open
        return;
    }
    // Clear current batch data (if any) before swapping in the new batch to release old MP4Demuxers and free file handles as soon as possible
    _demuxers.clear();
    _demuxer_stop_offsets.clear();
    _demuxer_start_cuts.clear();
    _current_batch.clear();

    // O(1) swap — no blocking I/O at boundary
    _demuxers = std::move(_next_batch.demuxers);
    _demuxer_stop_offsets = std::move(_next_batch.stop_offsets);
    _demuxer_start_cuts = std::move(_next_batch.start_cuts);
    _current_batch = std::move(_next_batch.segments);
    _next_batch.reset();

    // Set next_time for the following batch based on the end of the last segment in this batch
    auto last_seg = _current_batch.back();
    _stats.next_time = last_seg.stamp; // next batch should start from the begin of the last segment in this batch
    DebugL << "Next batch next_time set to " << _stats.next_time;
}

int64_t MultiMP4Demuxer::seekToWithTimeline(int64_t stamp_ms) {
    if (stamp_ms >= (int64_t)getDurationMS()) {
        return -1;
    }

    // Find target_unix in current batch; if not found, prefetch next batch until found or no more batch
    if (!_demuxers.empty()) {
        // Find the demuxer that contains target_unix
        for (auto &pr : _demuxers) {
            uint64_t seg_start_ms = pr.first;
            uint64_t seg_end_ms = pr.first + _demuxer_stop_offsets[pr.first] - _demuxer_start_cuts[pr.first];
            if (stamp_ms >= (int64_t)seg_start_ms && stamp_ms < (int64_t)seg_end_ms) {
                // Found the target demuxer in current batch
                _it = _demuxers.find(pr.first);
                // Seek to the correct offset within the file
                auto start_cut = _demuxer_start_cuts[pr.first];
                auto seek_offset = start_cut + (stamp_ms - seg_start_ms);
                return pr.first + _it->second->seekTo(seek_offset);
            }
        }
    }

    // Compute the target unix timestamp
    uint64_t target_unix = _stats.start_time + (uint64_t)(stamp_ms / 1000);
    _stats.next_time = target_unix;

    prefetchNextSegmentBatch(_stats.tuple, target_unix, 300, true);
    if (_next_batch.ready) {
        // If prefetch is already ready (e.g. no listener or very fast response), we can open the first batch immediately
        openNextSegmentBatch();
    }
    if (_demuxers.empty()) {
        // No segment available, signal EOF
        return -1;
    }
    _it = _demuxers.begin();
    refreshTracksIfChanged();
    // Note: do not seek here; the first file should already be seeked to the correct offset if needed (e.g. for lo fill with start_cut)
    return _it->first;
}

Frame::Ptr MultiMP4Demuxer::readFrameWithTimeline(bool &keyFrame, bool &eof) {
    for (;;) {
        // Trigger pre-fetch when we reach the last file of the current batch
        if (!_next_batch.ready && !_demuxers.empty() && _it != _demuxers.end()) {
            if (std::next(_it) == _demuxers.end()) {
                prefetchNextSegmentBatch(_stats.tuple, _stats.next_time, 300);
            }
        }
        auto ret = _it->second->readFrame(keyFrame, eof);
        // Cut-tail: if the frame DTS has reached the stop offset for this file,
        // force EOF so we switch to the next file immediately.
        if (ret && !eof) {
            auto stop_it = _demuxer_stop_offsets.find(_it->first);
            if (stop_it != _demuxer_stop_offsets.end() && ret->dts() >= stop_it->second) {
                eof = true;
                ret = nullptr;
            }
        }

        if (ret) {
            ret->setIndex(ret->getTrackType());
            auto it = _tracks.find(ret->getIndex());
            auto start_it = _demuxer_start_cuts.find(_it->first);
            if (it != _tracks.end()) {
                auto ret2 = std::make_shared<FrameStamp>(ret);
                ret2->setStamp(_it->first + ret->dts() - start_it->second, _it->first + ret->pts() - start_it->second);
                ret = std::move(ret2);
                it->second->inputFrame(ret);
            }
        }

        if (eof && _it != _demuxers.end()) {
            // Switch to the next file
            if (++_it == _demuxers.end()) {
                // Batch exhausted — use pre-fetched batch if available
                if (_next_batch.ready) {
                    openNextSegmentBatch();
                    if (_demuxers.empty()) {
                        // No more segment available, signal EOF
                        eof = true;
                        return nullptr;
                    }
                    _it = _demuxers.begin();
                } else {
                    // No more batch available, signal EOF
                    eof = true;
                    return nullptr;
                }
            }
            // The next file starts from scratch
            // Note: do not seek here; the next file should already be seeked to the correct offset if needed (e.g. for lo fill with start_cut)
            refreshTracksIfChanged();
            continue;
        }
        return ret;
    }
}

bool MultiMP4Demuxer::refreshTracksIfChanged() {
    if (!_on_tracks_changed) {
        return false;
    }
    auto new_tracks = _it->second->getTracks(false);
    bool changed = (new_tracks.size() != _tracks.size());
    if (!changed) {
        for (auto &new_track : new_tracks) {
            auto it = _tracks.find(new_track->getTrackType());
            if (it == _tracks.end() || it->second->getCodecId() != new_track->getCodecId()) {
                changed = true;
                break;
            }
            // Also check extra data (SPS/PPS for H264/H265, AudioSpecificConfig for AAC)
            auto old_extra = it->second->getExtraData();
            auto new_extra = new_track->getExtraData();
            bool old_has = old_extra && old_extra->size() > 0;
            bool new_has = new_extra && new_extra->size() > 0;
            if (old_has != new_has) {
                changed = true;
                break;
            }
            if (old_has && new_has) {
                if (old_extra->size() != new_extra->size() ||
                    memcmp(old_extra->data(), new_extra->data(), old_extra->size()) != 0) {
                    changed = true;
                    break;
                }
            }
        }
    }
    if (!changed) {
        return false;
    }
    _tracks.clear();
    std::vector<Track::Ptr> updated_tracks;
    DebugL << "Tracks changed for new segment batch, refreshing tracks map and notifying: ";
    for (auto &track : new_tracks) {
        auto clone_track = track->clone();
        clone_track->setIndex(clone_track->getTrackType());
        _tracks.emplace(clone_track->getIndex(), clone_track);
        updated_tracks.emplace_back(clone_track);
        DebugL << "track index: " << track->getIndex() << " -> " << clone_track->getIndex();
    }
    _on_tracks_changed(updated_tracks);
    return true;
}

}//namespace mediakit
#endif// ENABLE_MP4
