#ifdef ENABLE_MP4
#include <atomic>
#include <ctime>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include "Util/File.h"
#include "Util/uv_errno.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "MP4Recorder.h"
#include "Thread/ThreadPool.h"
#include "MP4Muxer.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

// A file cut because the video configuration changed and shorter than this is deleted instead of being
// published: it is the few pictures of the old configuration the camera sent right before switching.
static constexpr float kMinConfigChangeSegmentSec = 1.0f;

namespace {
class LeadingConfigFrame : public Frame {
public:
    explicit LeadingConfigFrame(Frame::Ptr frame) : _frame(std::move(frame)) { setIndex(_frame->getIndex()); }

    uint64_t dts() const override { return _frame->dts(); }
    uint64_t pts() const override { return _frame->pts(); }
    size_t prefixSize() const override { return _frame->prefixSize(); }
    bool keyFrame() const override { return true; }
    bool configFrame() const override { return _frame->configFrame(); }
    bool cacheAble() const override { return _frame->cacheAble(); }
    bool dropAble() const override { return _frame->dropAble(); }
    bool decodeAble() const override { return _frame->decodeAble(); }
    char *data() const override { return _frame->data(); }
    size_t size() const override { return _frame->size(); }
    CodecId getCodecId() const override { return _frame->getCodecId(); }

private:
    Frame::Ptr _frame;
};

struct CloseCounters {
    mutex mtx;
    map<uint64_t, uint64_t> queued;
    uint64_t next_id = 0, completed = 0, cancelled = 0, started = 0;
    uint64_t wait_us = 0, wait_max_us = 0, run_us = 0, run_max_us = 0;
    size_t running = 0;
};

struct CloseTicket {
    explicit CloseTicket(shared_ptr<CloseCounters> counters) : counters(std::move(counters)) {
        lock_guard<mutex> lock(this->counters->mtx);
        queued_at = getCurrentMicrosecond(true);
        id = ++this->counters->next_id;
        this->counters->queued.emplace(id, queued_at);
    }
    ~CloseTicket() {
        // A cancelled Task destroys its callable without running it.
        lock_guard<mutex> lock(counters->mtx);
        if (counters->queued.erase(id)) ++counters->cancelled;
    }
    void start() {
        started_at = getCurrentMicrosecond(true);
        lock_guard<mutex> lock(counters->mtx);
        counters->queued.erase(id);
        ++counters->running;
        ++counters->started;
        const auto wait = started_at - queued_at;
        counters->wait_us += wait;
        counters->wait_max_us = std::max(counters->wait_max_us, wait);
    }
    void finish() {
        const auto elapsed = getCurrentMicrosecond(true) - started_at;
        lock_guard<mutex> lock(counters->mtx);
        --counters->running;
        ++counters->completed;
        counters->run_us += elapsed;
        counters->run_max_us = std::max(counters->run_max_us, elapsed);
    }
    shared_ptr<CloseCounters> counters;
    uint64_t id, queued_at, started_at = 0;
};

class ClosePool : public ThreadPool {
public:
    explicit ClosePool(string name)
        : ThreadPool(1, ThreadPool::PRIORITY_NORMAL, true, false, name)
        , name(std::move(name)), counters(make_shared<CloseCounters>()) {}

    Task::Ptr async(TaskIn task, bool may_sync = true) override {
        return ThreadPool::async(wrap(std::move(task)), may_sync);
    }
    Task::Ptr async_first(TaskIn task, bool may_sync = true) override {
        return ThreadPool::async_first(wrap(std::move(task)), may_sync);
    }
    MP4Recorder::ClosePoolStats stats() const {
        MP4Recorder::ClosePoolStats ret;
        ret.name = name;
        lock_guard<mutex> lock(counters->mtx);
        ret.queued = counters->queued.size();
        ret.running = counters->running;
        ret.completed = counters->completed;
        ret.cancelled = counters->cancelled;
        if (!counters->queued.empty())
            ret.oldest_queued_ms = (getCurrentMicrosecond(true) - counters->queued.begin()->second) / 1000.0;
        ret.wait_avg_ms = counters->started ? counters->wait_us / (1000.0 * counters->started) : 0;
        ret.wait_max_ms = counters->wait_max_us / 1000.0;
        ret.run_avg_ms = counters->completed ? counters->run_us / (1000.0 * counters->completed) : 0;
        ret.run_max_ms = counters->run_max_us / 1000.0;
        return ret;
    }
private:
    TaskIn wrap(TaskIn task) {
        auto ticket = make_shared<CloseTicket>(counters);
        return [ticket, task]() {
            ticket->start();
            onceToken finish(nullptr, [ticket]() { ticket->finish(); });
            task();
        };
    }
    string name;
    shared_ptr<CloseCounters> counters;
};

mutex s_close_pools_mutex;
vector<weak_ptr<ClosePool>> s_close_pools;
// Set while a close task emits kBroadcastRecordMP4 on this thread.
thread_local vector<pair<const char *, uint64_t>> *s_close_steps = nullptr;
}

void MP4Recorder::addCloseStep(const char *name, uint64_t ms) {
    if (s_close_steps) {
        s_close_steps->emplace_back(name, ms);
    }
}

vector<MP4Recorder::ClosePoolStats> MP4Recorder::closePoolStats() {
    vector<shared_ptr<ClosePool>> pools;
    {
        lock_guard<mutex> lock(s_close_pools_mutex);
        for (const auto &weak : s_close_pools) {
            auto pool = weak.lock();
            if (pool) pools.emplace_back(std::move(pool));
        }
    }
    vector<ClosePoolStats> ret;
    for (const auto &pool : pools) ret.emplace_back(pool->stats());
    return ret;
}

TaskExecutor::Ptr MP4Recorder::closeExecutorFor(const string &stream_key) {
    // Closing blocks on disk (moov write, rename) and so do the kBroadcastRecordMP4 listeners
    // (timefile, SQLite), so none of it may run on a poller. One single-thread pool per slot keeps
    // the segments of a stream in order while different streams close in parallel.
    // Intentionally leaked, like the replay open pool: closes may still be queued at exit.
    static vector<TaskExecutor::Ptr> *pools = []() {
        GET_CONFIG(int, threads, Record::kCloseThreads);
        threads = std::max(1, threads);
        auto ret = new vector<TaskExecutor::Ptr>();
        for (int i = 0; i < threads; ++i) {
            auto pool = std::make_shared<ClosePool>("rec close " + to_string(i));
            ret->emplace_back(pool);
            lock_guard<mutex> lock(s_close_pools_mutex);
            s_close_pools.emplace_back(pool);
        }
        return ret;
    }();
    return (*pools)[std::hash<string>()(stream_key) % pools->size()];
}

static std::atomic<uint64_t> s_write_errors { 0 };

uint64_t MP4Recorder::writeErrorCount() {
    return s_write_errors.load(std::memory_order_relaxed);
}

MP4Recorder::MP4Recorder(const MediaTuple &tuple, const string &path, size_t max_second) {
    // ///record Business Logic//////
    static_cast<MediaTuple &>(_info) = tuple;
    _info.folder = path;
    auto params = Parser::parseArgs(tuple.params);
    if (params.find("start_time_ms") != params.end()) {
        // Epoch milliseconds exceed int: atoi overflowed into 1970 file names.
        _next_file_time_ms = (time_t)strtoll(params["start_time_ms"].data(), nullptr, 10);
    }
    if (params.find("continuous") != params.end()) {
        _continuous_file_time = (bool)atoi(params["continuous"].data());
    }
    GET_CONFIG(uint32_t, s_max_second, Protocol::kMP4MaxSecond);
    _max_second = max_second ? max_second : s_max_second;
    GET_CONFIG(bool, stagger_start, Record::kSegmentStartStagger);
    _first_max_second = _max_second;
    if (stagger_start && !_continuous_file_time && _max_second >= 2) {
        const size_t half = _max_second / 2;
        _first_max_second = half + std::hash<string>()(_info.shortUrl()) % half;
    }
}

MP4Recorder::~MP4Recorder() {
    try {
        flush();
        closeFile();
    } catch (std::exception &ex) {
        WarnL << ex.what();
    }
}

void MP4Recorder::createFile() {
    if (_continuous_file_time && _muxer) {
        _next_file_time_ms += _muxer->getDuration();
    }
    closeFile();
    const bool after_change_cut = _cfg.after_change_cut;
    _cfg.after_change_cut = false;
    time_t file_time;
    if (_continuous_file_time) {
        file_time = static_cast<time_t>(_next_file_time_ms / 1000);
    } else {
        file_time = (_next_file_time_ms != 0) ? static_cast<time_t>(_next_file_time_ms / 1000) : ::time(NULL);
        if (_next_file_time_ms == 0 && after_change_cut) {
            // The previous file was cut by a video config change and this one follows it directly: shift it
            // by 1s so that it starts after the end of the previous file on the timeline (start_time has a
            // 1s resolution and ::time(NULL) truncates, which puts the two files on the same second).
            file_time += 1;
        }
        _next_file_time_ms = 0; // consume override
    }

    auto date = getTimeStr("%Y-%m-%d", file_time);
    auto file_name = date + "-" + getTimeStr("%H-%M-%S", file_time) + "-" + std::to_string(_file_index++) + ".mp4";
    auto full_path = _info.folder + date + "/" + file_name;
    auto full_path_tmp = _info.folder + date + "/." + file_name;

    // ///record Business Logic//////
    _info.start_time = file_time;
    _info.file_name = file_name;
    _info.file_path = full_path;
    GET_CONFIG(string, appName, Record::kAppName);
    _info.url = appName + "/" + _info.app + "/" + _info.stream + "/" + date + "/" + file_name;

    // A failed open must not leave the previous segment's tmp path behind: closing would rename
    // that file again and announce a segment that is not on disk.
    _full_path_tmp.clear();
    try {
        _muxer = std::make_shared<MP4Muxer>();
        TraceL << "Open tmp mp4 file: " << full_path_tmp;
        _muxer->openMP4(full_path_tmp);
        for (auto &track :_tracks) {
            // Add track
            _muxer->addTrack(track);
        }
        _full_path_tmp = full_path_tmp;
    } catch (std::exception &ex) {
        s_write_errors.fetch_add(1, std::memory_order_relaxed);
        WarnL << ex.what();
    }
}

void MP4Recorder::asyncClose(bool drop_short_segment) {
    auto muxer = _muxer;
    auto full_path_tmp = _full_path_tmp;
    auto info = _info;
    TraceL << "Start close tmp mp4 file: " << full_path_tmp;
    closeExecutorFor(_info.shortUrl())->async([muxer, full_path_tmp, info, drop_short_segment]() mutable {
        Ticker close_ticker;
        info.time_len = muxer->getDuration() / 1000.0f;
        // Closing mp4 can be very time-consuming, so it should be executed in the background thread
        TraceL << "Closing tmp mp4 file: " << full_path_tmp;
        muxer->closeMP4();
        TraceL << "Closed tmp mp4 file: " << full_path_tmp;
        if (drop_short_segment && info.time_len < kMinConfigChangeSegmentSec) {
            // Not published: no record event, so nothing reaches the timeline.
            InfoL << "Drop " << info.time_len << "s mp4 cut by a video config change: " << info.file_path;
            if (!full_path_tmp.empty()) {
                File::delete_file(full_path_tmp);
            }
            return;
        }
        if (full_path_tmp.empty()) {
            // The file could not be opened: nothing was recorded.
            return;
        }
        // Get file size
        info.file_size = File::fileSize(full_path_tmp);
        if (info.file_size < 1024) {
            // The recording file is too small, delete it
            File::delete_file(full_path_tmp);
            return;
        }
        // Change the temporary file name to the official file name to prevent access to the mp4 before it is completed
        if (rename(full_path_tmp.data(), info.file_path.data()) != 0) {
            s_write_errors.fetch_add(1, std::memory_order_relaxed);
            WarnL << "Rename " << full_path_tmp << " failed: " << get_uv_errmsg(false);
            return;
        }
        const auto io_ms = close_ticker.elapsedTime();
        close_ticker.resetTime();
        TraceL << "Emit mp4 record event: " << info.file_path;
        // Trigger mp4 recording slice generation event
        vector<pair<const char *, uint64_t>> steps;
        s_close_steps = &steps;
        onceToken clear_steps(nullptr, []() { s_close_steps = nullptr; });
        NOTICE_EMIT(BroadcastRecordMP4Args, Broadcast::kBroadcastRecordMP4, info);
        const auto listeners_ms = close_ticker.elapsedTime();
        _StrPrinter steps_str;
        for (const auto &step : steps) {
            steps_str << ", " << step.first << "_ms=" << step.second;
        }
        DebugL << "MP4 close timing: io_ms=" << io_ms << ", listeners_ms=" << listeners_ms << steps_str;
        if (io_ms > 1000 || listeners_ms > 1000) {
            WarnL << "Slow MP4 close: io_ms=" << io_ms << ", listeners_ms=" << listeners_ms << steps_str
                  << ", path=" << info.file_path;
        }
    });
}

void MP4Recorder::closeFile(bool drop_short_segment) {
    if (_muxer) {
        asyncClose(drop_short_segment);
        _muxer = nullptr;
        _cfg.file_width = _cfg.file_height = 0;
    }
}

void MP4Recorder::flush() {
    if (_muxer) {
        _muxer->flush();
    }
}

void MP4Recorder::markInterrupted() {
    if (!_have_video) {
        // Audio only: there is no codec configuration that can change, nothing to wait for.
        return;
    }
    if (!_cfg.interrupted) {
        InfoL << "Stream interrupted, hold mp4 until the next key frame: " << _info.file_path;
    }
    if (_muxer) {
        // The last picture before the interruption is still in the FrameMerger: write it now, it is the
        // end of the file if the configuration turns out to have changed.
        _muxer->flush();
    }
    _cfg.interrupted = true;
}

static bool resolutionDiffers(int a_width, int a_height, int b_width, int b_height) {
    return a_width && a_height && b_width && b_height && (a_width != b_width || a_height != b_height);
}

static void trackResolution(const Track::Ptr &track, int &width, int &height) {
    width = height = 0;
    auto video = std::dynamic_pointer_cast<VideoTrack>(track);
    if (!video || !video->ready()) {
        return;
    }
    // The parameter sets are only re-parsed on demand: a track keeps reporting the previous size otherwise.
    video->update();
    width = video->getVideoWidth();
    height = video->getVideoHeight();
}

void MP4Recorder::trackStreamConfig(const Frame::Ptr &frame) {
    if (!frame->configFrame()) {
        return;
    }
    if (!_cfg.stream_track || _cfg.stream_track->getCodecId() != frame->getCodecId()) {
        _cfg.stream_track = Factory::getTrackByCodecId(frame->getCodecId());
    }
    if (_cfg.stream_track) {
        _cfg.stream_track->inputFrame(frame);
    }
}

void MP4Recorder::streamResolution(int &width, int &height) {
    trackResolution(_cfg.stream_track, width, height);
}

void MP4Recorder::videoResolution(int &width, int &height) const {
    width = height = 0;
    for (auto &track : _tracks) {
        if (track->getTrackType() == TrackVideo) {
            trackResolution(track, width, height);
            return;
        }
    }
}

bool MP4Recorder::inputFrame(const Frame::Ptr &frame) {
    if (_tracks.empty()) {
        return false;
    }

    if (frame->getTrackType() == TrackVideo) {
        for (auto &track : _tracks) {
            if (track->getIndex() == frame->getIndex()) {
                if (track->getCodecId() != frame->getCodecId()) {
                    return false;
                }
                break;
            }
        }

        trackStreamConfig(frame);
        if (frame->configFrame() && _muxer && !_cfg.changed) {
            int width = 0, height = 0;
            streamResolution(width, height);
            if (resolutionDiffers(width, height, _cfg.file_width, _cfg.file_height)) {
                InfoL << "Video config changed (" << _cfg.file_width << "x" << _cfg.file_height << " -> " << width << "x"
                      << height << "), close mp4: " << _info.file_path;
                _cfg.changed = true;
            } else if ((!width || !height) && _cfg.file_width && _cfg.file_height) {
                holdVideoFrame(frame);
                return true;
            }
        }
        if (_cfg.changed) {
            if (!frame->keyFrame()) {
                // The new parameter sets belong to the next file: held, then written in front of its first
                // key frame. A picture of another configuration cannot be decoded with this file's header.
                holdVideoFrame(frame);
                return frame->configFrame();
            }
            // MP4Muxer::closeMP4() does not flush its FrameMerger, and the config frames above were kept
            // out of it, so flush explicitly or the last picture of the old configuration would be lost.
            _muxer->flush();
            if (_continuous_file_time) {
                // createFile() only advances the continuous file time from a muxer that is still open.
                _next_file_time_ms += _muxer->getDuration();
            }
            closeFile(true /* drop_short_segment */);
            _cfg.changed = false;
            // The next file created is the one that directly follows this cut.
            _cfg.after_change_cut = true;
        }
    }

    if (_cfg.interrupted) {
        if (frame->getTrackType() != TrackVideo || !frame->keyFrame()) {
            if (frame->getTrackType() == TrackVideo) {
                // Parameter sets of the new session: written in front of its first key frame.
                holdVideoFrame(frame);
            }
            return frame->configFrame();
        }
        InfoL << "Stream resumed, " << (_muxer ? "continue mp4: " + _info.file_path : std::string("open a new mp4"))
              << " (" << _info.shortUrl() << ")";
        _cfg.interrupted = false;
    }

    auto stamp_inc = _delta_stamp[frame->getTrackType()].relativeStamp(frame->pts(), false);
    const auto segment_second = _file_index <= 1 ? _first_max_second : _max_second;
    // Conditions for establishment
    // 1. _muxer is empty (first frame, or the file was just closed on a resolution change)
    // 2. It's time to slice, and there is only audio
    // 3. It's time to slice, there is video and a video keyframe is encountered
    if (!_muxer || (stamp_inc > int64_t(segment_second) * 1000 && (!_have_video || frame->keyFrame()))) {
        int track_width = 0, track_height = 0;
        int stream_width = 0, stream_height = 0;
        if (_have_video) {
            videoResolution(track_width, track_height);
            streamResolution(stream_width, stream_height);
        }
        if (resolutionDiffers(stream_width, stream_height, track_width, track_height)) {
            if (!_cfg.mismatch) {
                _cfg.mismatch = true;
                _cfg.mismatch_dts = frame->dts();
                InfoL << "Skip " << stream_width << "x" << stream_height << " history under a "
                      << track_width << "x" << track_height << " track: " << _info.shortUrl();
            }
            if (frame->dts() < _cfg.mismatch_dts + 60 * 1000) {
                return false;
            }
            // Safety net: whatever the reason the two never agreed, recording must not stop for good. The
            // file is opened unchecked so this does not start over every 60s.
            WarnL << "Stream and track still disagree on the resolution after 60s, record anyway: " << _info.shortUrl();
            track_width = track_height = 0;
        }
        _cfg.mismatch = false;
        createFile();
        _cfg.file_width = track_width;
        _cfg.file_height = track_height;
        for (auto &ref : _delta_stamp) {
            ref.reset();
        }
    }

    if (_muxer) {
        if (frame->getTrackType() == TrackVideo && (frame->keyFrame() || frame->configFrame())) {
            // Parameter sets held back above go in front of the key frame (and of the parameter sets that
            // follow them), so the first sample of the file, or the first one after an interruption, carries
            // the configuration in-band: the file header alone is not enough for a playback that chains
            // the samples of consecutive files into one stream.
            writeHeldConfig();
        }
        // Generate mp4 file
        return _muxer->inputFrame(frame);
    }
    return false;
}

void MP4Recorder::holdVideoFrame(const Frame::Ptr &frame) {
    if (frame->configFrame()) {
        _cfg.held_config.emplace_back(Frame::getCacheAbleFrame(frame));
    } else {
        // A picture went by: the parameter sets held so far do not lead a key frame any more.
        _cfg.held_config.clear();
    }
}

void MP4Recorder::writeHeldConfig() {
    for (auto &config : _cfg.held_config) {
        _muxer->inputFrame(std::make_shared<LeadingConfigFrame>(config));
    }
    _cfg.held_config.clear();
}

bool MP4Recorder::addTrack(const Track::Ptr &track) {
    // Save all tracks in preparation for creating MP4MuxerFile
    _tracks.emplace_back(track);
    if (track->getTrackType() == TrackVideo) {
        _have_video = true;
    }
    return true;
}

void MP4Recorder::resetTracks() {
    closeFile();
    _tracks.clear();
    _have_video = false;
    _cfg.reset();
}

} /* namespace mediakit */

#endif //ENABLE_MP4
