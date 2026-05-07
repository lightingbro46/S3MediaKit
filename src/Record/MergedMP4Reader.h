#ifndef S3MEDIAKIT_MERGEDMP4READER_H
#define S3MEDIAKIT_MERGEDMP4READER_H

#ifdef ENABLE_MP4

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Common/MultiMediaSourceMuxer.h"
#include "Record/MP4Demuxer.h"

namespace mediakit {

/**
 * MergedMP4Reader
 * 
 * Produces a single FMP4 live-stream that automatically switches between a
 * high-quality (motion-triggered, may have gaps) source and a low-quality
 * (continuous) fallback source — for both live and VOD/replay paths.
 * 
 * Live mode
 * ---------
 * Call attachHiSource() / attachLoSource() with the MultiMediaSourceMuxer of
 * each camera stream.  The muxer subscribes to their raw-frame rings and runs
 * the state machine below.
 * 
 * Replay mode
 * -----------
 * Call openForReplay(). The reader queries the event-recording files for the specified start time,
 * pre-opens their MP4 demuxers, and starts a timer to read frames at realtime pace.  
 * The state machine is the same as live mode, except that the hi/lo sources are switched by the presence of hi segments 
 * in the replay timeline instead of motion events.
 * 
 * State machine
 * -------------
 *  PlayingLo ──(hi IDR arrives)──> StagingHi
 *  StagingHi ──(STAGE_SEGS segments buffered)──> PlayingHi
 *  PlayingHi ──(no hi IDR for hi_dropout_ms)──> SwitchingToLo
 *  SwitchingToLo ──(next lo IDR)──> PlayingLo
 * 
 * All transitions are IDR-aligned: the output stream always starts a new
 * segment at an IDR, ensuring the client player can do a seamless codec
 * change via changeType() + staging.
 * 
 * Timestamp continuity is guaranteed via seedStampOffsets() which is already
 * implemented in FMP4MediaSourceMuxer::resetTracks() + addTrackCompleted().
 */
class MergedMP4Reader : public std::enable_shared_from_this<MergedMP4Reader>, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<MergedMP4Reader>;

    /**
     * @param output_tuple  MediaTuple for the virtual output stream
     * @param poller        EventPoller for timers and async operations.  Must be the same poller that calls attachHiSource/attachLoSource.
     * @param hi_dropout_ms How long (ms) to wait after last hi IDR before
     *                      falling back to lo. Default 3000 ms.
     */
    /**
     * @param gop_cache  When true (default), Lo frames are buffered per-GOP while
     *                   playing Hi, and the cached GOP is flushed instantly on
     *                   Hi→Lo transition to eliminate the visual gap.  When false,
     *                   the GOP cache is disabled and the reader burst-reads the Lo
     *                   demuxer to find the next IDR on each Hi→Lo switch.
     */
    MergedMP4Reader(const MediaTuple &tuple, toolkit::EventPoller::Ptr poller = nullptr, bool gop_cache = true, bool enable_audio = true, bool add_mute_audio = true);
    
    MergedMP4Reader(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller = nullptr, bool gop_cache = true);

    ~MergedMP4Reader();

    // ── Live mode ──────────────────────────────────────────────────────────

    /**
     * Attach the high-quality camera stream.  Must be called from the muxer's
     * owner poller thread (or before startReadMP4 in replay mode).
     * Safe to call again to replace an existing hi source.
     */
    void attachHiSource(const MultiMediaSourceMuxer::Ptr &hi_muxer);

    /**
     * Detach the high-quality source (e.g. motion stopped, stream offline).
     * Triggers an immediate switch to lo quality.
     */
    void detachHiSource();

    /**
     * Attach the low-quality (continuous) camera stream.
     */
    void attachLoSource(const MultiMediaSourceMuxer::Ptr &lo_muxer);

    // ── Replay mode ────────────────────────────────────────────────────────

    /**
     * Open hi + lo MP4 file lists (same format as MultiMP4Demuxer::openMP4).
     * A timer is started internally to drive frame reading at realtime pace.
     * @param sample_ms Timer tick interval (ms).  Smaller values reduce latency but increase CPU usage.  Default 0 ms.
     * @param ref_self If true, the timer holds a strong reference to this object, ensuring it stays alive until replay finishes.  If false, the caller must keep a reference to this object until replay finishes.  Default true.
     */
    void openForReplay(uint64_t sample_ms = 0, bool ref_self = true, bool file_repeat = false);

    /**
     * Stop replay and release all resources.
     */
    void stopReplay();

private:
    void setup(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller, bool gop_cache = true, int hi_dropout_ms = 3000);

    void initOutputMuxer(const std::vector<Track::Ptr> &tracks);

    void switchToMuxer(const std::vector<Track::Ptr> &new_tracks);

    void onHiFrame(const Frame::Ptr &frame);
    void onLoFrame(const Frame::Ptr &frame);

    // Commit staged lo segments on startup (first full GOP buffered before sending)
    void flushLoStage();

    // Commit staged hi segments (STAGE_SEGS reached or timeout)
    void flushHiStage();

    // Drop all staged hi segments and return to lo
    void abortHiStage();

    // Check if hi dropout threshold is exceeded and switch to lo if necessary
    void checkHiDropout();

    // Replay helpers
    bool readReplayTick();

    // Query all Hi event-recording files from start_time_s and pre-open their demuxers.
    void queryHiSegments(const MediaTuple &hi_tuple, uint64_t start_time_s, uint64_t max_duration_s);

    // NAL-splitting helpers — mirror MediaSink::_track_map pattern.
    // Each entry holds a cloned Track (with delegate wired to a callback) and a
    // got-frame flag.  The cloned Track's inputFrame() calls H264Track/H265Track
    // splitting logic (splitH264 + insertConfigFrame) before dispatching.
    using TrackEntry = std::pair<Track::Ptr, bool /*got_frame*/>;
    using TrackMap   = std::unordered_map<int /*track_index*/, TrackEntry>;

    // Build/rebuild a track map from a track list.  Each cloned track gets a
    // delegate that forwards every sub-frame to cb.
    void onTrackReady(TrackMap &map, const std::vector<Track::Ptr> &tracks, std::function<void(const Frame::Ptr &)> cb);

    // Route frame through the track map.  Falls back to `cb` directly when
    // the frame index is not found (e.g. audio on a video-only Hi stream).
    void onTrackFrame(TrackMap &map, const Frame::Ptr &frame, const std::function<void(const Frame::Ptr &)> &cb);

    // Seek to the specified timestamp in the replay timeline.  Returns true if successful, false if the timestamp is out of range or an error occurs.
    bool seekTo(uint32_t stamp_seek);

    // Get/set the current playback position (ms), accounting for speed and pause.
    uint32_t getCurrentStamp();
    void setCurrentStamp(uint32_t new_stamp);

    // Forward a frame to the output muxer and, if a mute audio maker is active,
    // also drive it with video frames to generate synchronised silent audio.
    void inputFrame(const Frame::Ptr &frame);

    // If _add_mute_audio is true and `tracks` has video but no audio, appends a
    // synthetic AAC track to `tracks` and initialises _mute_audio_maker.
    void setupMuteAudio(std::vector<Track::Ptr> &tracks);

private:
    //MediaSourceEvent override
    bool seekTo(MediaSource &sender,uint32_t stamp) override;
    bool pause(MediaSource &sender, bool pause) override;
    bool speed(MediaSource &sender, float speed) override;

    bool close(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    int totalReaderCount(MediaSource &sender) override;

private:
    // ── Members ────────────────────────────────────────────────────────────
    enum class State {
        StagingLo,      ///< Buffering lo frames on startup (first GOP not yet complete)
        PlayingLo,      ///< Forwarding lo frames
        StagingHi,      ///< Buffering hi segments, not yet committed
        PlayingHi,      ///< Forwarding hi frames
        SwitchingToLo,  ///< Waiting for next lo IDR
    };
    State _state = State::StagingLo;
    int _hi_dropout_ms;
    uint64_t _last_hi_idr_wall_ms = 0;   ///< wall-clock ms of last hi IDR
    int   _hi_staged_segs = 0;           ///< segments accumulated in hi staging
    int   _lo_staged_segs = 0;           ///< IDR count during lo staging
    
    // Staged hi frames (buffered while waiting for STAGE_SEGS threshold)
    std::vector<Frame::Ptr> _hi_stage;
    // Staged lo frames (buffered during startup until first full GOP is ready)
    std::vector<Frame::Ptr> _lo_stage;

    // When true, Lo frames are accumulated per-GOP while playing Hi and flushed
    // immediately on Hi→Lo transition (zero visual gap).  When false, the Lo
    // demuxer is burst-read to find the next IDR instead (lower memory cost).
    bool _lo_gop_cache_enabled = true;

    // All Lo frames since the last IDR, cached while PLAYING_HI/STAGING_HI.
    // On Hi→Lo transition the full GOP is flushed before the new IDR so the
    // client sees a seamless picture (no visual gap equal to one GOP duration).
    std::vector<Frame::Ptr> _lo_gop_cache;
    int64_t _lo_dts_origin = -1;

    // Frames that exceeded the tick boundary on the previous tick.
    // MP4Demuxer has no unget/peek API — readFrame() advances the cursor
    // unconditionally, so the over-read frame must be saved here and replayed
    // at the start of the next tick to prevent DTS gaps.
    Frame::Ptr _hi_pending_frame; ///< stamped FrameStamp from last Hi tick
    Frame::Ptr _lo_pending_frame; ///< raw frame from last Lo tick

    // Output muxer & source
    FMP4MediaSourceMuxer::Ptr _muxer;
    MediaTuple     _tuple;
    ProtocolOption _option;
    MediaTuple     _hi_tuple;
    MediaTuple     _lo_tuple;
    // Live: ring readers (kept alive to maintain subscription)
    // using FrameRing       = MultiMediaSourceMuxer::RingType;
    // using FrameRingReader = FrameRing::RingReader;
    // std::shared_ptr<FrameRingReader> _hi_reader;
    // std::shared_ptr<FrameRingReader> _lo_reader;
    // Replay: Lo continuous stream + timer
    MultiMP4Demuxer::Ptr     _lo_demuxer;
    toolkit::Timer::Ptr      _replay_timer;
    bool                     _replay_lo_eof     = false;
    bool                     _replay_mode       = false; ///< true while in replay mode
    uint64_t                 _replay_current_ms = 0;     ///< current output position (ms)
    uint64_t                 _replay_start_time_s = 0;   ///< replay start unix timestamp (s)
    uint64_t                 _replay_total_dur_s = 0;    ///< total duration of all segments (s)
    bool                     _replay_file_repeat        = false; // Whether the replay should loop back to the first segment 
                                                                 // after reaching the end of the last segment in replay mode
    bool                     _replay_paused = false;
    float                    _replay_speed = 1.0;
    uint32_t                 _replay_seek_to = 0;
    toolkit::Ticker          _replay_seek_ticker;

    /// One event-based Hi recording (may cover only part of the total replay window).
    struct HiSegment {
        uint64_t        rel_start_ms = 0;  ///< replay-relative start (ms from replay start_time)
        uint64_t        rel_end_ms   = 0;  ///< replay-relative end
        MP4Demuxer::Ptr demuxer;           ///< pre-opened and seeked to 0, ready to read
    };
    // Replay: Hi event-based segments (pre-queried + pre-opened at openForReplay time)
    std::vector<HiSegment>   _hi_segments;
    size_t                   _hi_seg_idx = 0;    ///< next/current segment index
    MP4Demuxer::Ptr          _hi_active_demuxer; ///< demuxer of the currently active Hi segment

    // Track sets for each source
    std::vector<Track::Ptr>  _hi_tracks;
    std::vector<Track::Ptr>  _lo_tracks;
    bool                     _have_video_hi = false; ///< hi stream has at least one video track
    bool                     _have_video_lo = false; ///< lo stream has at least one video track

    // Mute audio
    bool                     _enable_audio   = true; ///< when false, audio tracks and frames are suppressed entirely
    bool                     _add_mute_audio = true; ///< when true, add silent AAC track if stream has video but no audio
    MuteAudioMaker::Ptr      _mute_audio_maker;      ///< generates mute AAC frames driven by video timestamps

    // Track maps — one per source path.  Built in onTrackReady(); cleared in stopReplay().
    TrackMap _lo_track_map;
    TrackMap _hi_track_map;

    // Poller for timers and async operations.  Must be the same poller that calls attachHiSource/attachLoSource.
    toolkit::EventPoller::Ptr _poller;
    // Mutex protecting state machine (may be called from different pollers)
    std::recursive_mutex     _mtx;
};

} // namespace mediakit
#endif // ENABLE_MP4
#endif // S3MEDIAKIT_MERGEDMP4READER_H