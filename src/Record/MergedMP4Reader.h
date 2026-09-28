#ifndef S3MEDIAKIT_MERGEDMP4READER_H
#define S3MEDIAKIT_MERGEDMP4READER_H

#ifdef ENABLE_MP4

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/MultiMediaSourceMuxer.h"
#include "Common/config.h"
#include "Record/MP4Demuxer.h"

namespace mediakit {

/**
 * Parse the replay start time (seconds) from a replay stream id
 * "<app>/<sub path>/<start>[.replay.<md5>]". Returns false when the last path
 * component is not a plain decimal number.
 */
bool parseReplayStartTime(const std::string &stream, uint64_t &start_s);

/**
 * Timeline placement of one opened clip. Frames before play_from_ms overlap the
 * previous clip and are dropped so the merged DTS never runs backwards.
 *
 * play_from_ms normally equals start_ms. On a Main<->Sub switch the next clip
 * starts at its keyframe at or before the switch point instead: the previous
 * clip's end_ms is moved back to that keyframe and the next clip plays from it,
 * so the switch loses no GOP.
 */
struct ReplayClipWindow {
    int64_t start_ms = 0;       // clip start on the merged timeline (planner)
    int64_t end_ms = 0;         // clip end on the merged timeline
    int64_t file_offset_ms = 0; // clip start relative to the file's first sample
    int64_t play_from_ms = 0;   // first timeline time played from this clip

    int64_t map(int64_t raw_ms) const { return start_ms + raw_ms - file_offset_ms; }
    bool beforeStart(int64_t mapped_ms) const { return mapped_ms < play_from_ms; }
    bool pastEnd(int64_t mapped_ms) const { return mapped_ms >= end_ms; }
};

ReplayClipWindow makeReplayClipWindow(const Broadcast::ReplayClip &clip, uint64_t replay_start_s);

/**
 * Seek to a keyframe at or before target_ms. `seek` has MP4Demuxer::seekTo
 * semantics: it lands on the nearest keyframe (possibly after the target) and
 * returns its file time, or -1 on failure. The result can still be after
 * target_ms when the file has no earlier keyframe.
 */
int64_t seekKeyframeAtOrBefore(const std::function<int64_t(int64_t)> &seek, int64_t target_ms);

/**
 * Switch point for playing `next` directly after `prev`, when next's demuxer
 * landed on a keyframe at landed_file_ms. Returns false when the keyframe is
 * not strictly inside (prev.play_from_ms, next.start_ms]; the caller then keeps
 * the planner boundary.
 */
bool replaySwitchPoint(const ReplayClipWindow &prev, const ReplayClipWindow &next,
                       int64_t landed_file_ms, int64_t &switch_ms);

/**
 * Replays a manager-provided timeline of recorded Main/Sub clips as one media
 * source through the enabled output adapters. The public playback URL is
 * unchanged. The manager layer builds
 * data-only ReplayClip entries from the recording index; this class only opens
 * clips, maintains timestamps/IDR boundaries and feeds the output muxer.
 */
class MergedMP4Reader : public std::enable_shared_from_this<MergedMP4Reader>, public MediaSourceEvent {
public:
    using Ptr = std::shared_ptr<MergedMP4Reader>;

    MergedMP4Reader(const MediaTuple &tuple,
                    toolkit::EventPoller::Ptr poller = nullptr,
                    bool enable_audio = true,
                    bool add_mute_audio = true);

    MergedMP4Reader(const MediaTuple &tuple,
                    const ProtocolOption &option,
                    toolkit::EventPoller::Ptr poller = nullptr);

    ~MergedMP4Reader();

    /**
     * Builds the replay timeline and starts playing it. The output source
     * registers on the reader's poller once the first clip's tracks are ready,
     * so it is not yet findable when this returns.
     * @return false when the timeline has no clip at the requested time or its
     *         first clip cannot be opened
     */
    bool openForReplay(uint64_t sample_ms = 0, bool ref_self = true, bool file_repeat = false);
    void stopReplay();

private:
    void setup(const MediaTuple &tuple, const ProtocolOption &option, toolkit::EventPoller::Ptr poller);

    void initOutputMuxer(const std::vector<Track::Ptr> &tracks);
    void switchToMuxer(const std::vector<Track::Ptr> &tracks);
    void inputFrame(const Frame::Ptr &frame);
    void setupMuteAudio(std::vector<Track::Ptr> &tracks);

    bool startTimelineReplay(uint64_t sample_ms, bool ref_self, bool file_repeat);
    bool readTimelineTick();
    bool openTimelineClip(size_t index);
    void prepareNextClip(size_t index);
    void resetPreparedClip();
    int64_t clipPlayFromMs(size_t index) const;
    void configureTimelineAudio();
    void closeTimelineClip();
    std::vector<Track::Ptr> normalizeTracks(const std::vector<Track::Ptr> &tracks) const;
    bool sameTrackSet(const std::vector<Track::Ptr> &a, const std::vector<Track::Ptr> &b) const;

    using TrackEntry = std::pair<Track::Ptr, bool /* got frame */>;
    using TrackMap = std::unordered_map<int, TrackEntry>;
    void onTrackReady(TrackMap &map,
                      const std::vector<Track::Ptr> &tracks,
                      std::function<void(const Frame::Ptr &)> cb);
    void onTrackFrame(TrackMap &map,
                      const Frame::Ptr &frame,
                      const std::function<void(const Frame::Ptr &)> &cb);

    bool seekTo(uint32_t stamp_seek);

    bool seekTo(MediaSource &sender, uint32_t stamp) override;
    bool pause(MediaSource &sender, bool pause) override;
    bool speed(MediaSource &sender, float speed) override;
    bool close(MediaSource &sender) override;
    MediaOriginType getOriginType(MediaSource &sender) const override;
    std::string getOriginUrl(MediaSource &sender) const override;
    toolkit::EventPoller::Ptr getOwnerPoller(MediaSource &sender) override;
    int totalReaderCount(MediaSource &sender) override;

private:
    MultiMediaSourceMuxer::Ptr _muxer;
    MediaTuple _tuple;
    ProtocolOption _option;

    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _replay_timer;
    std::recursive_mutex _mtx;

    bool _replay_mode = false;
    bool _replay_file_repeat = false;
    bool _replay_paused = false;
    float _replay_speed = 1.0f;
    uint64_t _replay_start_time_s = 0;
    uint64_t _replay_current_ms = 0;
    uint64_t _replay_total_dur_s = 0;

    std::vector<Broadcast::ReplayClip> _timeline_clips;
    size_t _timeline_clip_idx = 0;
    MP4Demuxer::Ptr _timeline_demuxer;
    std::vector<Track::Ptr> _timeline_tracks;
    TrackMap _timeline_track_map;
    ReplayClipWindow _timeline_window;
    // Next clip opened ahead of time on a Main<->Sub switch; its demuxer is
    // already positioned at play_from_ms.
    struct PreparedClip {
        size_t index = 0;
        MP4Demuxer::Ptr demuxer;
        int64_t play_from_ms = 0;
    };
    PreparedClip _timeline_next;
    Frame::Ptr _timeline_pending_frame;
    bool _timeline_pending_key = false; // MP4 sync-sample flag of _timeline_pending_frame
    bool _timeline_priming = false;
    uint64_t _timeline_discard_before_ms = 0;
    bool _timeline_mode = false;

    bool _enable_audio = true;
    bool _add_mute_audio = true;
    MuteAudioMaker::Ptr _mute_audio_maker;
};

} // namespace mediakit
#endif // ENABLE_MP4
#endif // S3MEDIAKIT_MERGEDMP4READER_H
