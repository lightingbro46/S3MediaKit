#ifdef ENABLE_MP4
#include "MergedMP4Reader.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "Extension/Frame.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static const int STAGE_SEGS = 2;          // Number of staged hi segments before committing to a hi switch
static const uint64_t MAX_QUERY_S = 3600; // 1 hour query window for replay

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple, toolkit::EventPoller::Ptr poller) {
    ProtocolOption option;
    // Read mp4 file and stream it, do not regenerate mp4/hls file repeatedly
    option.enable_mp4 = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    // mp4 supports multiple tracks
    option.max_track = 16;
    setup(tuple, option, std::move(poller), 3000);
}

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller) {
    setup(tuple, option, std::move(poller), 3000);
}

MergedMP4Reader::~MergedMP4Reader() {
    stopReplay();
}

/**
 * Common setup for constructors: initialize members from parameters, query stream quality mapping if in replay mode, etc.
 * For replay mode: 
 *       tuple.app = {app_name} (configured in Record::kAppName) 
 *       tuple.stream = {app}/vod/{start_time_s}
 *       tuple.params = "quality={auto|hi|lo}" (optional)
 * For live mode:
 *       tuple.app = {app}
 *       tuple.stream = ""
 *       tuple.params = "quality={auto|hi|lo}&prefered={hi|lo}" (optional)
 */
void MergedMP4Reader::setup(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller, int hi_dropout_ms) {
    // It is recommended to read and write files in the background thread
    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();
    _tuple = tuple;
    _option = option;
    _hi_dropout_ms = hi_dropout_ms;

    GET_CONFIG(string, app_name, Record::kAppName);
    if (tuple.app == app_name) {
        _replay_mode = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: output muxer management
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::initOutputMuxer(const std::vector<Track::Ptr> &tracks) {
    if (!_muxer) {
        _muxer = std::make_shared<FMP4MediaSourceMuxer>(_tuple, _option);
    }

    for (auto &t : tracks) {
        _muxer->addTrack(t);
    }
    _muxer->addTrackCompleted();
    // setListener after addTrackCompleted() so ring + source are registered first.
    // FMP4MediaSourceMuxer::addTrackCompleted() writes the moov box synchronously —
    // no pre-fill loop needed; inputFrame() works immediately after this call.
    _muxer->setListener(shared_from_this());
    DebugL << "Output muxer initialized with " << tracks.size() << " track(s)";
}

// Transition to a new track set: triggers resetTracks() which saves DTS offset,
// then re-adds tracks and calls addTrackCompleted() (emits new moov).
void MergedMP4Reader::switchToMuxer(const std::vector<Track::Ptr> &new_tracks) {
    if (!_muxer) {
        initOutputMuxer(new_tracks);
        return;
    }
    _muxer->resetTracks();
    for (auto &t : new_tracks) {
        _muxer->addTrack(t);
    }
    _muxer->addTrackCompleted();
    DebugL << "Track switch: " << new_tracks.size() << " track(s)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Live mode: source attachment
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::attachLoSource(const MultiMediaSourceMuxer::Ptr &lo_muxer) {
    // lock_guard<recursive_mutex> lck(_mtx);
    // _lo_reader.reset(); // detach old reader

    // auto ring = lo_muxer->getFrameRing();
    // if (!ring) {
    //     WarnL << "lo source has no frame ring — GOP cache not enabled";
    //     return;
    // }

    // // Capture tracks for lo
    // _lo_tracks = lo_muxer->getTracks(true);

    // // If no output muxer yet, initialise with lo tracks now
    // if (!_muxer && !_lo_tracks.empty()) {
    //     _initOutputMuxer(_lo_tracks);
    // }

    // weak_ptr<QualityAwareFMP4Reader> weak_self = shared_from_this();
    // auto poller = WorkThreadPool::Instance().getPoller();
    // _lo_reader = ring->attach(poller, false);
    // _lo_reader->setReadCB([weak_self](const Frame::Ptr &frame) {
    //     if (auto self = weak_self.lock()) {
    //         lock_guard<recursive_mutex> lck(self->_mtx);
    //         self->_onLoFrame(frame);
    //     }
    // });
    // InfoL << "lo source attached";
}

void MergedMP4Reader::attachHiSource(const MultiMediaSourceMuxer::Ptr &hi_muxer) {
    // lock_guard<recursive_mutex> lck(_mtx);
    // _hi_reader.reset(); // detach old reader

    // auto ring = hi_muxer->getFrameRing();
    // if (!ring) {
    //     WarnL << "hi source has no frame ring — GOP cache not enabled";
    //     return;
    // }

    // _hi_tracks = hi_muxer->getTracks(true);

    // weak_ptr<QualityAwareFMP4Reader> weak_self = shared_from_this();
    // auto poller = WorkThreadPool::Instance().getPoller();
    // _hi_reader = ring->attach(poller, false);
    // _hi_reader->setReadCB([weak_self](const Frame::Ptr &frame) {
    //     if (auto self = weak_self.lock()) {
    //         lock_guard<recursive_mutex> lck(self->_mtx);
    //         self->_onHiFrame(frame);
    //     }
    // });
    // InfoL << "hi source attached";
}

void MergedMP4Reader::detachHiSource() {
    // lock_guard<recursive_mutex> lck(_mtx);
    // _hi_reader.reset();
    // _hi_tracks.clear();
    // if (_state == State::PlayingHi || _state == State::StagingHi) {
    //     _abortHiStage();
    //     _state = State::SwitchingToLo;
    //     InfoL << "hi source detached — switching to lo";
    // }
}

// ─────────────────────────────────────────────────────────────────────────────
// State machine: frame handlers
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::onHiFrame(const Frame::Ptr &frame) {
    if (!_muxer) return;

    checkHiDropout();

    if (frame->keyFrame() && !frame->configFrame()) {
        _last_hi_idr_wall_ms = getCurrentMillisecond();
    }

    switch(_state) {
        case State::PlayingLo:
            // Hi IDR arrived — begin staging
            if (frame->keyFrame() && !frame->configFrame()) {
                _state = State::StagingHi;
                _hi_staged_segs = 0;
                _hi_stage.clear();
                // Switch muxer to hi track set (emits new moov to clients)
                if (!_hi_tracks.empty()) {
                    switchToMuxer(_hi_tracks);
                }
                _hi_stage.push_back(frame);
                InfoL << "lo->hi staging started";
            }
            break;

        case State::StagingHi:
            _hi_stage.push_back(frame);
            // Count IDR boundaries as segment markers
            if (frame->keyFrame() && !frame->configFrame()) {
                _hi_staged_segs++;
                if (_hi_staged_segs >= STAGE_SEGS) {
                    flushHiStage();
                }
            }
            break;

        case State::PlayingHi:
            // Normal hi forwarding
            _muxer->inputFrame(frame);
            break;

        case State::SwitchingToLo:
            // Ignore hi while switching back to lo
            break;
    }
}

void MergedMP4Reader::onLoFrame(const Frame::Ptr &frame) {
    if (!_muxer) return;

    switch (_state) {
        case State::PlayingLo:
            _muxer->inputFrame(frame);
            break;
        
        case State::PlayingHi:
        case State::StagingHi:
            // Cache the most recent lo IDR for instant fallback
            if (frame->keyFrame() && !frame->configFrame()) {
                _lo_idr_cache = frame;
            }
            // Keep dropout check ticking
            checkHiDropout();
            break;
        
        case State::SwitchingToLo:
            // Wait for lo IDR to start clean
            if (frame->keyFrame() && !frame->configFrame()) {
                _state = State::PlayingLo;
                // Switch muxer back to lo tracks (new moov for clients)
                if (!_lo_tracks.empty()) {
                    switchToMuxer(_lo_tracks);
                }
                _muxer->inputFrame(frame);
                _lo_idr_cache = nullptr;
                InfoL << "Switched to lo at IDR";
            }
            break;
    }
}

void MergedMP4Reader::flushHiStage() {
    _state = State::PlayingHi;
    // Feed all staged frames into the output muxer
    for (auto &f : _hi_stage) {
        _muxer->inputFrame(f);
    }
    _hi_stage.clear();
    _hi_staged_segs = 0;
    InfoL << "Hi stage flushed — now PLAYING_HI";
}

void MergedMP4Reader::abortHiStage() {
    _hi_stage.clear();
    _hi_staged_segs = 0;
}

void MergedMP4Reader::checkHiDropout() {
    // In replay mode, Hi segment boundaries are managed explicitly in readReplayTick.
    if (_replay_mode) return;
    if (_state != State::PlayingHi && _state != State::StagingHi) return;
    if (!_last_hi_idr_wall_ms) return;

    auto elapsed = static_cast<int>(toolkit::getCurrentMillisecond() - _last_hi_idr_wall_ms);
    if (elapsed < _hi_dropout_ms) return;

    InfoL << "Hi dropout detected (" << elapsed << "ms) — switching to lo";

    abortHiStage();

    // If we have a cached lo IDR, use it immediately (zero-gap switch)
    if (_lo_idr_cache) {
        _state = State::PlayingLo;
        if (!_lo_tracks.empty()) {
            switchToMuxer(_lo_tracks);
        }
        _muxer->inputFrame(_lo_idr_cache);
        _lo_idr_cache = nullptr;
        InfoL << "Switched to lo at cached IDR";
    } else {
        _state = State::SwitchingToLo;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay mode
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::openForReplay(uint64_t sample_ms, bool ref_self) {
    if (!_replay_mode) {
        WarnL << "Replay mode not implemented yet";
        return;
    }

    string vhost = _tuple.vhost;
    string app = _tuple.app;
    string stream = _tuple.stream;
    string params = _tuple.params;

    auto parts = split(stream, "/");
    if (parts.size() > 2) {
        uint64_t start_time_s = stoull(parts.back()); 
        app = parts.front(); // app is the first part of the stream name in replay mode
        stream = ""; // stream is not used in replay mode
        _replay_start_time_s = start_time_s;
    }

    string hi_stream_id, lo_stream_id;
    Broadcast::StreamQualityInvoker invoker = [&](const std::map<int, std::string> &quality_map) {
        auto hi_it = quality_map.find(0); // PrimaryStream = hi
        auto lo_it = quality_map.find(1); // SecondaryStream = lo
        if (hi_it != quality_map.end()) hi_stream_id = hi_it->second;
        if (lo_it != quality_map.end()) lo_stream_id = lo_it->second;
    };
    NOTICE_EMIT(BroadcastGetStreamQualityArgs, Broadcast::kBroadcastGetStreamQuality, vhost, app, invoker);

    CHECK(!hi_stream_id.empty() || !lo_stream_id.empty() || _replay_start_time_s > 0);

    // ── Lo: continuous recording → standard timeline demuxer ──────────────
    {
        _lo_demuxer = std::make_shared<MultiMP4Demuxer>();
        string lo_vod_path = "/record/" + app + "/" + lo_stream_id + "/vod/" + to_string(_replay_start_time_s);
        try {
            _lo_demuxer->openMP4(lo_vod_path);
            _lo_tracks = _lo_demuxer->getTracks(false);
            _replay_total_dur_s = _lo_demuxer->getDurationMS() / 1000;
            // _lo_demuxer->seekTo(0); // Ensure demuxer is ready at the start of the timeline
            DebugL << "Lo demuxer opened: " << lo_vod_path << ", duration=" << _replay_total_dur_s << "s, tracks=" << _lo_tracks.size();
        } catch (exception &ex) {
            WarnL << "Lo demuxer open failed: " << ex.what();
            _lo_demuxer.reset();
        }
    }

    // ── Hi: event-based recordings → query + pre-open all segments ────────
    {
        MediaTuple hi_tuple = {vhost, app, hi_stream_id, ""};
        queryHiSegments(hi_tuple, _replay_start_time_s, _replay_total_dur_s);
    }

    // If Hi is available from the very start of the replay window (rel_start_ms==0)
    // or Lo is entirely unavailable, initialise the muxer with Hi tracks directly
    // and pre-enter StagingHi.  This prevents emitting a moov(Lo) that would be
    // immediately superseded by moov(Hi) on the first tick, and avoids the duplicate
    // moov(Hi) that the PlayingLo→StagingHi transition in onHiFrame would cause.
    {
        bool hi_starts_now = !_hi_segments.empty()
                            && _hi_segments[0].rel_start_ms == 0
                            && !_hi_tracks.empty();
        bool lo_only = _lo_tracks.empty() && !_hi_tracks.empty();

        if (hi_starts_now || lo_only) {
            initOutputMuxer(_hi_tracks);        // moov(Hi) — exactly once
            _state          = State::StagingHi; // skip PlayingLo → no re-switch
            _hi_staged_segs = 0;
            _hi_stage.clear();
        } else {
            auto &init_tracks = !_lo_tracks.empty() ? _lo_tracks : _hi_tracks;
            if (!init_tracks.empty()) {
                initOutputMuxer(init_tracks);
            }
            _state = State::PlayingLo;
        }
    }

    // Build splitter maps so readReplayTick() can route frames through the
    // cloned tracks' splitH264 logic instead of passing merged Annex B frames
    // directly to FrameMerger (which would wrap the whole buffer as one AVCC NAL).
    onTrackReady(_lo_track_map, _lo_tracks, [this](const Frame::Ptr &f) { onLoFrame(f); });
    onTrackReady(_hi_track_map, _hi_tracks, [this](const Frame::Ptr &f) { onHiFrame(f); });

    _replay_lo_eof     = !_lo_demuxer;
    _replay_current_ms = 0;
    _lo_dts_origin     = -1;        // reset per-replay origin
    _hi_seg_idx        = 0;
    _hi_active_demuxer.reset();
    _hi_pending_frame.reset();
    _lo_pending_frame.reset();

    GET_CONFIG(uint32_t, sampleMS, Record::kSampleMS);
    auto timer_sec = (sample_ms ? sample_ms : sampleMS) / 1000.0f;

    if (ref_self) {
        // Capture a strong reference so the timer keeps the muxer (and its
        // registered _output_src) alive until replay finishes.
        auto strong_self = shared_from_this();
        _replay_timer = std::make_shared<Timer>(timer_sec, [strong_self]() {
            return strong_self->readReplayTick();
        }, _poller);
    } else {
        weak_ptr<MergedMP4Reader> weak_self = shared_from_this();
        _replay_timer = std::make_shared<Timer>(timer_sec, [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            return strong_self->readReplayTick();
        }, _poller);
    }
    InfoL << "Replay started: app=" << app << ", hi_stream=" << hi_stream_id << ", lo_stream=" << lo_stream_id << ", start_time=" << _replay_start_time_s;
}

void MergedMP4Reader::stopReplay() {
    _replay_timer.reset();
    _lo_demuxer.reset();
    _hi_segments.clear();
    _hi_active_demuxer.reset();
    _lo_track_map.clear();
    _hi_track_map.clear();
    _hi_pending_frame.reset();
    _lo_pending_frame.reset();
    _hi_seg_idx    = 0;
    _replay_lo_eof = true;
    _lo_dts_origin = -1;
    // NOTE: do NOT reset _replay_mode — it is set at construction time from
    // tuple.app and must survive stopReplay()/openForReplay() cycles.
    // Resetting it breaks checkHiDropout()'s "if (_replay_mode) return" guard.
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay: read frame from demuxers
// ─────────────────────────────────────────────────────────────────────────────
bool MergedMP4Reader::readReplayTick() {
    bool hi_available = (_hi_seg_idx < _hi_segments.size()) || _hi_active_demuxer;
    if (_replay_lo_eof && !hi_available) {
        InfoL << "Replay EOF";
        return false;
    }

    if (!_muxer) return true;

    GET_CONFIG(uint32_t, sample_ms, Record::kSampleMS);
    uint64_t target_ms = _replay_current_ms + sample_ms;

    // ── Hi segment lifecycle management ─────────────────────────────────────

    // Expire the active segment if replay time has passed its end.
    if (_hi_active_demuxer && _hi_seg_idx < _hi_segments.size()) {
        if (_replay_current_ms >= _hi_segments[_hi_seg_idx].rel_end_ms) {
            _hi_pending_frame.reset(); // discard any over-read frame from expiring segment
            _hi_active_demuxer.reset();
            if (_state == State::PlayingHi || _state == State::StagingHi) {
                abortHiStage();
                _state = State::SwitchingToLo;
            }
            InfoL << "Hi segment " << _hi_seg_idx << " expired, switching to Lo";
            _hi_seg_idx++;
        }
    }

    // Activate the next segment once replay time reaches its start.
    if (!_hi_active_demuxer && _hi_seg_idx < _hi_segments.size()) {
        auto &seg = _hi_segments[_hi_seg_idx];
        if (_replay_current_ms >= seg.rel_start_ms) {
            _hi_active_demuxer = seg.demuxer;
            InfoL << "Hi segment " << _hi_seg_idx << " activated at replay_ms=" << _replay_current_ms;
        }
    }

    // ── Read Hi frames from the active segment ───────────────────────────────
    if (_hi_active_demuxer && _hi_seg_idx < _hi_segments.size()) {
        auto &seg = _hi_segments[_hi_seg_idx];
        for (;;) {
            // Replay a frame that was over-read (exceeded target_ms) on the previous tick.
            // MP4Demuxer has no unget/peek API, so we save it rather than drop it.
            Frame::Ptr stamped;
            if (_hi_pending_frame) {
                stamped = std::move(_hi_pending_frame);
            } else {
                bool key, eof;
                auto raw = _hi_active_demuxer->readFrame(key, eof);
                if (eof) {
                    _hi_active_demuxer.reset();
                    if (_state == State::PlayingHi || _state == State::StagingHi) {
                        abortHiStage();
                        _state = State::SwitchingToLo;
                    }
                    DebugL << "Hi segment " << _hi_seg_idx << " finished (EOF), switching to Lo";
                    _hi_seg_idx++;
                    break;
                }
                if (!raw) break;

                // Map in-file DTS (start at 0) → replay-relative DTS.
                // MP4Demuxer::makeFrame() may return a merged Annex B frame containing
                // multiple NALs (e.g. [SPS][PPS][IDR] from a single MP4 sample).
                // splitVideoFrame() via onTrackFrame splits them into individual NALs.
                int64_t rdts = int64_t(raw->dts());
                int64_t rpts = int64_t(raw->pts());
                uint64_t mapped_dts = seg.rel_start_ms + uint64_t(rdts > 0 ? rdts : 0);
                uint64_t mapped_pts = seg.rel_start_ms + uint64_t(rpts > 0 ? rpts : 0);
                auto fs = std::make_shared<FrameStamp>(raw);
                fs->setStamp(int64_t(mapped_dts), int64_t(mapped_pts));
                stamped = std::move(fs);
            }

            uint64_t mapped_dts = stamped->dts();
            if (mapped_dts > target_ms && _state != State::StagingHi) {
                _hi_pending_frame = std::move(stamped); // save — not dropped
                break;
            }

            // DebugL << "Hi rel_dts=" << mapped_dts << ", target_ms=" << target_ms;
            onTrackFrame(_hi_track_map, stamped, [this](const Frame::Ptr &f) { onHiFrame(f); });
        }
    }

    // ── Read Lo frames ───────────────────────────────────────────────────────
    if (!_replay_lo_eof && _lo_demuxer) {
        for (;;) {
            // Replay a frame that was over-read (exceeded target_ms) on the previous tick.
            Frame::Ptr frame;
            if (_lo_pending_frame) {
                frame = std::move(_lo_pending_frame);
            } else {
                bool key, eof;
                frame = _lo_demuxer->readFrame(key, eof);
                if (eof) {
                    _replay_lo_eof = true;
                    break;
                }
                if (!frame) break;
            }

            // Lazy-init: normalize absolute DTS from timeline seek (e.g. 1800000ms)
            // so lo_rel_dts shares the same [0, duration) coordinate as _replay_current_ms.
            if (_lo_dts_origin < 0) {
                _lo_dts_origin = static_cast<int64_t>(frame->dts());
            }
            int64_t lo_rel_dts = static_cast<int64_t>(frame->dts()) - _lo_dts_origin;
            if (lo_rel_dts > static_cast<int64_t>(target_ms)) {
                _lo_pending_frame = std::move(frame); // save — not dropped
                break;
            }

            // Split multi-NAL Annex B frames before muxer (same reason as Hi path above).
            // DebugL << "Lo rel_dts=" << lo_rel_dts << ", target_ms=" << target_ms;
            onTrackFrame(_lo_track_map, frame, [this](const Frame::Ptr &f) { onLoFrame(f); });
        }
    }

    _replay_current_ms = target_ms;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay: Hi segment discovery
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::queryHiSegments(const MediaTuple &hi_tuple, uint64_t start_time_s, uint64_t max_duration_s) {
    _hi_segments.clear();
    
    // Ask for all Hi recording files from start_time_s onwards.
    // A 1-hour window is used to capture all events in a typical hour's replay.
    std::map<string, std::map<uint64_t, string>> multi_files;
    Broadcast::RecordedMP4Invoker invoker = [&](const std::map<std::string, std::map<uint64_t, std::string>> &files) {
        multi_files = files;
    };
    NOTICE_EMIT(BroadcastGetRecordedMP4Args, Broadcast::kBroadcastGetRecordedMP4, hi_tuple, start_time_s, max_duration_s, invoker);
    
    if (multi_files.empty() || multi_files.find(hi_tuple.stream) == multi_files.end()) {
        InfoL << "No Hi files found for stream=" << hi_tuple.stream;
        return;
    }

    // Prefer the entry whose key matches hi_tuple.stream
    auto it = multi_files.find(hi_tuple.stream);
    // Each DB block = one event recording file → one HiSegment.
    // DTS inside each file starts from 0; we map it to replay-relative time
    // by adding rel_start_ms when feeding frames to the state machine.
    for (const auto &f : it->second) {
        const auto &abs_start_s = f.first;
        const auto &file_path = f.second;
        try {
            auto demuxer = std::make_shared<MP4Demuxer>();
            demuxer->openMP4(file_path);
            auto dur_ms = demuxer->getDurationMS();
            if (dur_ms == 0) {
                WarnL << "Hi file has zero duration, skipping: " << file_path;
                continue;
            }
            // Convert absolute file timestamp → replay-relative offset (ms).
            // Files that started before the replay window start are clamped to 0.
            int64_t rel_start = (int64_t(abs_start_s) - int64_t(start_time_s)) * 1000;
            if (rel_start < 0) rel_start = 0;

            HiSegment seg;
            seg.rel_start_ms = uint64_t(rel_start);
            seg.rel_end_ms   = seg.rel_start_ms + dur_ms;
            seg.demuxer      = std::move(demuxer);

            DebugL << "Hi segment queued: ["
                  << "file=" << file_path
                  << " abs_start=" << abs_start_s
                  << " rel_start=" << seg.rel_start_ms
                  << " dur=" << dur_ms
                  << "]";
            _hi_segments.push_back(std::move(seg));
        } catch (const std::exception &e) {
            WarnL << "Failed to open Hi file, skipping: " << file_path << " error: " << e.what();
        }
    }

    // Capture Hi tracks from the first segment for use in initOutputMuxer.
    if (!_hi_segments.empty() && _hi_segments[0].demuxer) {
        _hi_tracks = _hi_segments[0].demuxer->getTracks(false);
    }

    DebugL << "Hi segments queued: " << _hi_segments.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// NAL-splitting helpers (mirror of MediaSink::_track_map pattern)
// ─────────────────────────────────────────────────────────────────────────────

// Clone each track, attach a delegate that forwards every post-split sub-frame
// to cb, and register in map.  The cloned H264Track/H265Track::inputFrame()
// calls splitH264() internally for composite Annex B samples (e.g. SPS+PPS+IDR
// in one MP4 sample), and auto-injects insertConfigFrame() when an IDR arrives
// without preceding inline SPS/PPS — the exact same logic MediaSink provides.
void MergedMP4Reader::onTrackReady(TrackMap &map, const std::vector<Track::Ptr> &tracks, std::function<void(const Frame::Ptr &)> cb) {
    map.clear();
    for (auto &t : tracks) {
        auto clone = t->clone(); // copies codec info + SPS/PPS, NOT delegates
        clone->addDelegate([cb](const Frame::Ptr &f) {
            cb(f);
            return true;
        });
        map[t->getIndex()] = {std::move(clone), false};
    }
}

// Route a frame through the splitter.  If the track index is in the map the
// cloned track's inputFrame() is called — it splits multi-NAL frames and fires
// the delegate for each sub-frame.  Falls back to cb directly for unknown
// indices (e.g. audio on a video-only Hi segment, or unmapped tracks).
void MergedMP4Reader::onTrackFrame(TrackMap &map, const Frame::Ptr &frame, const std::function<void(const Frame::Ptr &)> &cb) {
    auto it = map.find(frame->getIndex());
    if (it == map.end()) {
        cb(frame);
        return;
    }
    it->second.second = true;
    it->second.first->inputFrame(frame); // splitting + delegate dispatch
}

bool MergedMP4Reader::seekTo(MediaSource &sender,uint32_t stamp) {
    return true;
}

bool MergedMP4Reader::pause(MediaSource &sender, bool pause) {
    //todo:
    return true;
}

bool MergedMP4Reader::speed(MediaSource &sender, float speed) {
    //todo:
    return true;
}

bool MergedMP4Reader::close(MediaSource &sender) {
    _replay_timer = nullptr;
    WarnL << "close media: " << sender.getUrl();
    return true;
}

MediaOriginType MergedMP4Reader::getOriginType(MediaSource &sender) const {
    return MediaOriginType::mp4_vod;
}

string MergedMP4Reader::getOriginUrl(MediaSource &sender) const {
    return _tuple.shortUrl();
}

toolkit::EventPoller::Ptr MergedMP4Reader::getOwnerPoller(MediaSource &sender) {
    return _poller;
}

int MergedMP4Reader::totalReaderCount(MediaSource &sender) {
    return _muxer ? _muxer->readerCount() : 0;
}

} // namespace mediakit

#endif // ENABLE_MP4