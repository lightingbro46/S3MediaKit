#ifdef ENABLE_MP4
#include "MergedMP4Reader.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "Extension/Frame.h"
#include "Extension/Factory.h"
#include <algorithm>

using namespace std;
using namespace toolkit;

namespace mediakit {

// Number of staged hi segments before committing to a hi switch
static const int STAGE_SEGS_LIVE   = 2;  // live: need to confirm stream stable
static const int STAGE_SEGS_REPLAY = 1;  // replay: file guaranteed stable

// Mute audio constants (mirror of MediaSink.cpp)
static const int    MUTE_AUDIO_INDEX  = 0xFFFF;
static uint8_t      s_mute_adts_cfg[] = { 0x15, 0x88 }; // AAC-LC 44100Hz stereo

// Returns true only for IDR boundaries that should gate state transitions.
// When a stream has video, only video keyframes mark IDR boundaries because
// audio frames always report keyFrame()==true, which would trigger spurious
// Hi/Lo state-machine transitions.
static bool isVideoIDR(const Frame::Ptr &frame, bool have_video) {
    if (!frame->keyFrame() || frame->configFrame()) return false;
    return !have_video || frame->getTrackType() == TrackVideo;
}

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple, toolkit::EventPoller::Ptr poller, bool gop_cache, bool enable_audio, bool add_mute_audio) {
    ProtocolOption option;
    // Read mp4 file and stream it, do not regenerate mp4/hls file repeatedly
    option.enable_mp4 = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    // mp4 supports multiple tracks
    option.max_track = 16;
    option.enable_audio = enable_audio;
    option.add_mute_audio = add_mute_audio;
    setup(tuple, option, std::move(poller), gop_cache);
}

MergedMP4Reader::MergedMP4Reader(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller, bool gop_cache) {
    setup(tuple, option, std::move(poller), gop_cache);
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
void MergedMP4Reader::setup(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller, bool gop_cache, int hi_dropout_ms) {
    // It is recommended to read and write files in the background thread
    _poller = poller ? std::move(poller) : WorkThreadPool::Instance().getPoller();
    _tuple = tuple;
    _option = option;
    _hi_dropout_ms = hi_dropout_ms;
    _lo_gop_cache_enabled = gop_cache;
    _enable_audio   = option.enable_audio;
    _add_mute_audio = option.add_mute_audio;

    GET_CONFIG(string, app_name, Record::kAppName);
    if (tuple.app == app_name) {
        _replay_mode = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: output muxer management
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::setupMuteAudio(std::vector<Track::Ptr> &tracks) {
    _mute_audio_maker = nullptr;
    if (!_add_mute_audio || !_enable_audio) return;
    bool has_video = false, has_audio = false;
    for (auto &t : tracks) {
        if (t->getTrackType() == TrackVideo) has_video = true;
        if (t->getTrackType() == TrackAudio) has_audio = true;
    }
    if (!has_video || has_audio) return; // nothing to do
    // Create a synthetic mute AAC track (same config as MediaSink::addMuteAudioTrack())
    auto audio = Factory::getTrackByCodecId(CodecAAC);
    audio->setIndex(MUTE_AUDIO_INDEX);
    audio->setExtraData(s_mute_adts_cfg, sizeof(s_mute_adts_cfg));
    tracks.push_back(audio);
    // MuteAudioMaker generates silent AAC frames keyed to video timestamps.
    // Frames are injected directly into the muxer by index.
    auto maker = std::make_shared<MuteAudioMaker>();
    maker->addDelegate([this](const Frame::Ptr &frame) {
        return _muxer->inputFrame(frame);
    });
    _mute_audio_maker = std::move(maker);
    TraceL << "Mute AAC track added (video-only stream)";
}

void MergedMP4Reader::inputFrame(const Frame::Ptr &frame) {
    if (!_enable_audio && frame->getTrackType() == TrackAudio) {
        return; // audio disabled — drop audio frames
    }
    _muxer->inputFrame(frame);
    if (_mute_audio_maker && frame->getTrackType() == TrackVideo) {
        _mute_audio_maker->inputFrame(frame);
    }
}

void MergedMP4Reader::initOutputMuxer(const std::vector<Track::Ptr> &tracks) {
    if (!_muxer) {
        _muxer = std::make_shared<FMP4MediaSourceMuxer>(_tuple, _option);
    }

    auto track_list = tracks; // mutable copy — setupMuteAudio may append a track
    if (!_enable_audio) {
        // Strip audio tracks before passing to the muxer (mirror MediaSink::addTrack)
        track_list.erase(std::remove_if(track_list.begin(), track_list.end(),
            [](const Track::Ptr &t) { return t->getTrackType() == TrackAudio; }),
            track_list.end());
    }
    setupMuteAudio(track_list);
    for (auto &t : track_list) {
        _muxer->addTrack(t);
    }
    _muxer->addTrackCompleted();
    // setListener after addTrackCompleted() so ring + source are registered first.
    // FMP4MediaSourceMuxer::addTrackCompleted() writes the moov box synchronously —
    // no pre-fill loop needed; inputFrame() works immediately after this call.
    _muxer->setListener(shared_from_this());
    DebugL << "Output muxer initialized with " << track_list.size() << " track(s)";
}

// Transition to a new track set: triggers resetTracks() which saves DTS offset,
// then re-adds tracks and calls addTrackCompleted() (emits new moov).
void MergedMP4Reader::switchToMuxer(const std::vector<Track::Ptr> &new_tracks) {
    if (!_muxer) {
        initOutputMuxer(new_tracks);
        return;
    }
    auto track_list = new_tracks; // mutable copy — setupMuteAudio may append a track
    if (!_enable_audio) {
        track_list.erase(std::remove_if(track_list.begin(), track_list.end(),
            [](const Track::Ptr &t) { return t->getTrackType() == TrackAudio; }),
            track_list.end());
    }
    setupMuteAudio(track_list);
    _muxer->resetTracks();
    for (auto &t : track_list) {
        _muxer->addTrack(t);
    }
    _muxer->addTrackCompleted();
    DebugL << "Track switch: " << track_list.size() << " track(s)";
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

    if (isVideoIDR(frame, _have_video_hi)) {
        _last_hi_idr_wall_ms = getCurrentMillisecond();
    }

    switch(_state) {
        case State::PlayingLo:
        case State::StagingLo:
            // Hi IDR arrived — begin staging.
            // If Lo was staging, flush whatever we have buffered first so the
            // client gets something to decode before the Hi switch.
            if (isVideoIDR(frame, _have_video_hi)) {
                if (_state == State::StagingLo && !_lo_stage.empty()) {
                    flushLoStage(); // state → PlayingLo; sends buffered Lo GOP
                }
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
            if (isVideoIDR(frame, _have_video_hi)) {
                _hi_staged_segs++;
                int stage_segs = _replay_mode ? STAGE_SEGS_REPLAY : STAGE_SEGS_LIVE;
                if (_hi_staged_segs >= stage_segs) {
                    flushHiStage();
                }
            }
            break;

        case State::PlayingHi:
            // Normal hi forwarding
            inputFrame(frame);
            break;

        case State::SwitchingToLo:
            // Ignore hi while switching back to lo
            break;
    }
}

void MergedMP4Reader::onLoFrame(const Frame::Ptr &frame) {
    if (!_muxer) return;

    switch (_state) {
        case State::StagingLo:
            // Buffer frames until a full GOP is ready before sending to client.
            // Pre-IDR frames are dropped — they cannot be decoded without a reference.
            if (_lo_stage.empty() && !isVideoIDR(frame, _have_video_lo)) {
                break; // skip until first IDR
            }
            _lo_stage.push_back(frame);
            if (isVideoIDR(frame, _have_video_lo)) {
                _lo_staged_segs++;
                int stage_segs = _replay_mode ? STAGE_SEGS_REPLAY : STAGE_SEGS_LIVE;
                if (_lo_staged_segs > stage_segs) {
                    flushLoStage();            // state → PlayingLo, flush buffered GOP
                }
            }
            break;

        case State::PlayingLo:
            inputFrame(frame);
            break;
        
        case State::PlayingHi:
        case State::StagingHi:
            // Accumulate full GOP so we can flush it instantly on Hi→Lo transition.
            // On a new IDR reset the buffer (old GOP is no longer needed).
            if (_lo_gop_cache_enabled) {
                if (isVideoIDR(frame, _have_video_lo)) {
                    _lo_gop_cache.clear();
                }
                _lo_gop_cache.push_back(frame);
            }
            // Keep dropout check ticking
            checkHiDropout();
            break;
        
        case State::SwitchingToLo:
            // If a cached GOP is available, flush it immediately on the very
            // first Lo frame — no need to wait for an IDR because the cache
            // already ends on an IDR boundary (clean decode point).
            if (_lo_gop_cache_enabled && !_lo_gop_cache.empty()) {
                _state = State::PlayingLo;
                if (!_lo_tracks.empty()) {
                    switchToMuxer(_lo_tracks);
                }
                for (auto &f : _lo_gop_cache) {
                    inputFrame(f);
                }
                _lo_gop_cache.clear();
                inputFrame(frame);
                InfoL << "Switched to lo immediately (flushed GOP cache)";
            } else if (isVideoIDR(frame, _have_video_lo)) {
                // No cache: wait for IDR for a clean decode start
                _state = State::PlayingLo;
                if (!_lo_tracks.empty()) {
                    switchToMuxer(_lo_tracks);
                }
                inputFrame(frame);
                InfoL << "Switched to lo at IDR (burst-read)";
            }
            break;
    }
}

void MergedMP4Reader::flushLoStage() {
    _state = State::PlayingLo;
    // Feed all staged frames into the output muxer
    for (auto &f : _lo_stage) {
        inputFrame(f);
    }
    _lo_stage.clear();
    _lo_staged_segs = 0;
    InfoL << "Lo stage flushed — now PLAYING_LO";
}

void MergedMP4Reader::flushHiStage() {
    _state = State::PlayingHi;
    // Feed all staged frames into the output muxer
    for (auto &f : _hi_stage) {
        inputFrame(f);
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

    // If we have a cached lo GOP, flush it immediately (zero-gap switch);
    // otherwise fall into SwitchingToLo and let burst-read find the next IDR.
    if (_lo_gop_cache_enabled && !_lo_gop_cache.empty()) {
        _state = State::PlayingLo;
        if (!_lo_tracks.empty()) {
            switchToMuxer(_lo_tracks);
        }
        for (auto &f : _lo_gop_cache) {
            inputFrame(f);
        }
        _lo_gop_cache.clear();
        InfoL << "Switched to lo from GOP cache";
    } else {
        _state = State::SwitchingToLo;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Replay mode
// ─────────────────────────────────────────────────────────────────────────────
void MergedMP4Reader::openForReplay(uint64_t sample_ms, bool ref_self, bool file_repeat) {
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
            _have_video_lo = false;
            for (auto &t : _lo_tracks) {
                if (t->getTrackType() == TrackVideo) { _have_video_lo = true; break; }
            }
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
        bool hi_only = _lo_tracks.empty() && !_hi_tracks.empty();

        if (hi_starts_now || hi_only) {
            initOutputMuxer(_hi_tracks);        // moov(Hi) — exactly once
            _state          = State::StagingHi; // skip PlayingLo → no re-switch
            _hi_staged_segs = 0;
            _hi_stage.clear();
        } else {
            auto &init_tracks = !_lo_tracks.empty() ? _lo_tracks : _hi_tracks;
            if (!init_tracks.empty()) {
                initOutputMuxer(init_tracks);
            }
            // StagingLo: buffer until first full GOP before sending to client.
            // flushLoStage() will transition to PlayingLo once STAGE_SEGS_REPLAY
            // IDR boundaries have been seen.
            _state = State::StagingLo;
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
    _lo_gop_cache.clear();
    _lo_stage.clear();
    _lo_staged_segs = 0;

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

    // Fire the first tick immediately so frames start flowing without waiting
    // for the first timer callback (up to sample_ms delay, typically 500ms).
    // StagingLo will buffer subsequent ticks until the first GOP is complete.
    readReplayTick();

    _replay_file_repeat = file_repeat;
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
    _lo_gop_cache.clear();
    _lo_stage.clear();
    _lo_staged_segs = 0;
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

    // Skip tick when paused
    if (_replay_paused) return true;

    GET_CONFIG(uint32_t, sample_ms, Record::kSampleMS);
    uint64_t tick_ms = (uint64_t)(sample_ms * _replay_speed);
    uint64_t target_ms = _replay_current_ms + tick_ms;

    // Track the highest DTS actually dispatched this tick.
    // After a burst-read (StagingHi or SwitchingToLo bypass), frames are consumed
    // far beyond target_ms.  Without updating _replay_current_ms accordingly, the
    // next tick's target_ms would still be << the pending frame's DTS, causing the
    // reader to stall for (gap / sample_ms) ticks before resuming normal output.
    uint64_t hi_last_dts = _replay_current_ms;
    uint64_t lo_last_dts = _replay_current_ms;

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
            if (mapped_dts > hi_last_dts) hi_last_dts = mapped_dts;
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
            if (lo_rel_dts > static_cast<int64_t>(target_ms) && _state != State::SwitchingToLo) {
                _lo_pending_frame = std::move(frame); // save — not dropped
                break;
            }

            // Split multi-NAL Annex B frames before muxer (same reason as Hi path above).
            // DebugL << "Lo rel_dts=" << lo_rel_dts << ", target_ms=" << target_ms;
            uint64_t lo_u_dts = lo_rel_dts > 0 ? uint64_t(lo_rel_dts) : 0;
            if (lo_u_dts > lo_last_dts) lo_last_dts = lo_u_dts;
            onTrackFrame(_lo_track_map, frame, [this](const Frame::Ptr &f) { onLoFrame(f); });
        }
    }

    // Advance to the highest DTS dispatched this tick.  In normal playback this
    // equals target_ms.  After a burst-read (SwitchingToLo / StagingHi bypass)
    // it may be much larger — using the max prevents the next tick from stalling
    // while target_ms slowly catches up to the pending frame's DTS.
    uint64_t advanced_ms = target_ms;
    if (hi_last_dts > advanced_ms) advanced_ms = hi_last_dts;
    if (lo_last_dts > advanced_ms) advanced_ms = lo_last_dts;
    _replay_current_ms = advanced_ms;

    GET_CONFIG(bool, file_repeat, Record::kFileRepeat);
    if (_replay_lo_eof && (file_repeat || _replay_file_repeat)) {
        // Need to start from the beginning
        seekTo(0);
        return true;
    }

    return !_replay_lo_eof;
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
        _have_video_hi = false;
        for (auto &t : _hi_tracks) {
            if (t->getTrackType() == TrackVideo) { _have_video_hi = true; break; }
        }
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

uint32_t MergedMP4Reader::getCurrentStamp() {
    return (uint32_t)(_replay_seek_to + !_replay_paused * _replay_speed * _replay_seek_ticker.elapsedTime());
}

void MergedMP4Reader::setCurrentStamp(uint32_t new_stamp) {
    auto old_stamp = getCurrentStamp();
    _replay_seek_to = new_stamp;
    _replay_seek_ticker.resetTime();
}

bool MergedMP4Reader::seekTo(MediaSource &sender, uint32_t stamp) {
    // Resume playback after seeking (mirrors MP4Reader behaviour)
    pause(sender, false);
    TraceL << getOriginUrl(sender) << ",stamp:" << stamp;
    return seekTo(stamp);
}

bool MergedMP4Reader::pause(MediaSource &sender, bool pause) {
    if (_replay_paused == pause) {
        return true;
    }
    setCurrentStamp(getCurrentStamp());
    _replay_paused = pause;
    TraceL << getOriginUrl(sender) << ",pause:" << pause;
    return true;
}

bool MergedMP4Reader::speed(MediaSource &sender, float speed) {
    if (speed < 0.1 || speed > 20) {
        WarnL << "The playback speed value range is illegal:" << speed;
        return false;
    }
    setCurrentStamp(getCurrentStamp());
    // Resume playback after setting speed
    _replay_paused = false;
    if (_replay_speed == speed) {
        return true;
    }
    _replay_speed = speed;
    TraceL << getOriginUrl(sender) << ",speed:" << speed;
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

bool MergedMP4Reader::seekTo(uint32_t stamp_seek) {
    lock_guard<recursive_mutex> lck(_mtx);
    if (stamp_seek > _replay_total_dur_s * 1000) {
        return false;
    }

    // Seek Lo demuxer to the requested position
    if (_lo_demuxer) {
        auto stamp = _lo_demuxer->seekTo(stamp_seek);
        if (stamp == -1) {
            return false;
        }
    }

    // Update replay cursor and reset lazy DTS origin (re-initialised on next frame)
    _replay_current_ms = stamp_seek;
    _lo_dts_origin     = -1;
    _replay_lo_eof     = !_lo_demuxer;

    // Discard over-read frames that are no longer valid after the seek
    _hi_pending_frame.reset();
    _lo_pending_frame.reset();

    // Find the Hi segment that covers stamp_seek and re-seek its demuxer
    _hi_active_demuxer.reset();
    _hi_seg_idx = 0;
    for (size_t i = 0; i < _hi_segments.size(); ++i) {
        if (_hi_segments[i].rel_end_ms > stamp_seek) {
            _hi_seg_idx = i;
            if (stamp_seek >= _hi_segments[i].rel_start_ms) {
                // stamp_seek falls inside this segment — seek to the offset within it
                uint64_t offset = stamp_seek - _hi_segments[i].rel_start_ms;
                _hi_segments[i].demuxer->seekTo(static_cast<int64_t>(offset));
                _hi_active_demuxer = _hi_segments[i].demuxer;
            }
            break;
        }
    }

    // Reset all staging buffers and state machine
    _hi_stage.clear();
    _hi_staged_segs = 0;
    _lo_stage.clear();
    _lo_staged_segs = 0;
    _lo_gop_cache.clear();

    // Enter StagingHi if we land inside a Hi segment, otherwise StagingLo
    if (_hi_active_demuxer && !_hi_tracks.empty()) {
        _state = State::StagingHi;
    } else {
        _state = State::StagingLo;
    }

    return true;
}

} // namespace mediakit

#endif // ENABLE_MP4