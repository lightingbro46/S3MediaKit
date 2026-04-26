#ifndef S3MEDIAKIT_MP4DEMUXER_H
#define S3MEDIAKIT_MP4DEMUXER_H
#ifdef ENABLE_MP4

#include <map>
#include "MP4.h"
#include "Extension/Track.h"
#include "Util/ResourcePool.h"
#include "Common/MediaSource.h"

namespace mediakit {

class MP4Demuxer : public TrackSource {
public:
    using Ptr = std::shared_ptr<MP4Demuxer>;

    ~MP4Demuxer() override;

    /**
     * Open file
     * @param file mp4 file path
     */
    void openMP4(const std::string &file);

    /**
     * @brief Close mp4 file
     */
    void closeMP4();

    /**
     * Move timeline to a specific location
     * @param stamp_ms Expected timeline position, in milliseconds
     * @return Timeline position
     */
    int64_t seekTo(int64_t stamp_ms);

    /**
     * Read a frame of data
     * @param keyFrame Whether it is a key frame
     * @param eof Whether the file has been read completely
     * @return Frame data, may be empty
     */
    Frame::Ptr readFrame(bool &keyFrame, bool &eof);

    /**
     * Get all Track information
     * @param trackReady Whether to require the track to be ready
     * @return All Tracks
     */
    std::vector<Track::Ptr> getTracks(bool trackReady) const override;

    /**
     * Get file length
     * @return File length, in milliseconds
     */
    uint64_t getDurationMS() const;

private:
    int getAllTracks();
    void onVideoTrack(uint32_t track_id, uint8_t object, int width, int height, const void *extra, size_t bytes);
    void onAudioTrack(uint32_t track_id, uint8_t object, int channel_count, int bit_per_sample, int sample_rate, const void *extra, size_t bytes);
    Frame::Ptr makeFrame(uint32_t track_id, toolkit::Buffer::Ptr buf, int64_t pts, int64_t dts);

private:
    MP4FileDisk::Ptr _mp4_file;
    MP4FileDisk::Reader _mov_reader;
    uint64_t _duration_ms = 0;
    std::unordered_map<int, Track::Ptr> _tracks;
    toolkit::ResourcePool<toolkit::BufferRaw> _buffer_pool;
};

struct SegmentBatchState {
    MediaTuple tuple;
    uint64_t start_time = 0;
    uint64_t total_dur  = 0;
    uint64_t next_time  = 0;
    uint64_t first_time = 0;
    std::string app;
    std::string hi_stream;           ///< PrimaryStream id (may be empty)
    std::string lo_stream;           ///< SecondaryStream id (may be empty)
    std::string quality  = "auto";   ///< filter: "hi"|"lo"|"auto"
    std::string prefered = "hi";     ///< preferred when both available
};

struct SegmentEntry {
    std::string file_path;
    std::string quality;               // "hi" or "lo"
    uint64_t stamp    = 0;             // unix ts (seconds) — sort/order only
    uint64_t dur_ms      = 0;          // ms-accurate duration from MP4Demuxer
    uint64_t start_cut = 0;            // IDR-aligned seek offset into file (ms); 0 = from start
    uint64_t stop_offset  = 0;         // hard stop offset in file (ms); 0 = natural EOF
    // Pre-opened demuxer — reused by openSegmentDemuxers to avoid double open
    MP4Demuxer::Ptr demuxer;
};

struct SegmentNextBatch {
    // Pre-opened demuxers — boundary switch is O(1) map swap, no blocking I/O
    std::map<uint64_t, MP4Demuxer::Ptr> demuxers;
    std::map<uint64_t, uint64_t>        start_cuts;    ///< timeline_key → start_ms
    std::map<uint64_t, uint64_t>        stop_offsets;  ///< timeline_key → stop_ms
    std::vector<SegmentEntry>           segments;      ///< sorted by timeline offset (stamp)
    bool ready = false;
    void reset() {
        demuxers.clear();
        start_cuts.clear();
        stop_offsets.clear();
        segments.clear();
        ready = false;
    }
};

class MultiMP4Demuxer : public TrackSource {
public:
    using Ptr = std::shared_ptr<MultiMP4Demuxer>;

    ~MultiMP4Demuxer() override = default;

    /**
     * Open mp4 files in batches and treat multiple files as one mp4
     * @param file Multiple mp4 file paths, separated by semicolons; or folders containing multiple mp4 files
     * @param params Parameters for batch opening, such as quality selection and gap-fill logic
     */
    void openMP4(const std::string &file, const std::string &params = "");

    /**
     * @brief Batch Close mp4 Files
     */
    void closeMP4();

    /**
     * Move the overall timeline to somewhere
     * @param stamp_ms The expected overall length position of the timeline, in milliseconds
     * @return Overall length position of timeline
     */
    int64_t seekTo(int64_t stamp_ms);

    /**
     * Read a frame of data
     * @param keyFrame Is a keyframe
     * @param eof Are all files read?
     * @return Frame data, may be empty
     */
    Frame::Ptr readFrame(bool &keyFrame, bool &eof);

    /**
     * Get all the track information in the first file
     * @param trackReady Is it necessary to have track ready?
     * @return All Track information in the first file
     */
    std::vector<Track::Ptr> getTracks(bool trackReady) const override;

    /**
     * Get the total file length
     * @return Total file length, unit milliseconds
     */
    uint64_t getDurationMS() const;

    using OnTracksChangedCB = std::function<void(const std::vector<Track::Ptr> &)>;

    /**
     * Register a callback invoked when the active track set changes (e.g. codec change across files)
     */
    void setOnTracksChangedCB(OnTracksChangedCB cb) { _on_tracks_changed = std::move(cb); }

private:
    /**
     * Open mp4 files in batches and treat multiple files as one mp4, with timeline-based logic to support gap-fill and quality selection
     * @param files Multiple mp4 file paths, separated by semicolons; or folders containing multiple mp4 files
     * @param params Parameters for batch opening, such as quality selection and gap-fill logic
     */
    void openMP4WithTimeline(const std::string &files, const std::string &params = "");

    /**
     * Move the overall timeline to somewhere, with timeline-based logic to support gap-fill and quality selection
     * @param stamp_ms The expected overall length position of the timeline, in milliseconds
     * @return Overall length position of timeline
     */
    int64_t seekToWithTimeline(int64_t stamp_ms);

    /**
     * Read a frame of data, with timeline-based logic to support gap-fill and quality selection
     * @param keyFrame Is a keyframe
     * @param eof Are all files read?
     * @return Frame data, may be empty
     */
    Frame::Ptr readFrameWithTimeline(bool &keyFrame, bool &eof);

    /**
     * Refresh the active track set if the track information in the current file has changed compared to the previous file (e.g. codec change).  Returns true if the track set has changed and the callback has been invoked.
     * Note that track change is only detected at file boundaries, and the callback is only invoked when the track set actually changes (not just on every file switch).
     */
    bool refreshTracksIfChanged();

    /**
     * Pre-fetch the next batch of segments when playing the last file of the
     * current batch.  Sets _next_batch.ready = true and also sets the stop
     * offset for the current last file so its tail is trimmed precisely.
     * @param tuple The media tuple for which to prefetch the next batch
     * @param start_time The expected unix timestamp of the start of the next batch, in seconds
     * @param max_duration The maximum duration to prefetch, in seconds; the actual prefetched duration may be longer if the last segment overlaps with the max_duration boundary
     * @param seek Whether to seek to the correct offset within the first file of the next batch; should be true when prefetching is triggered by seekTo, false when prefetching is triggered by normal playback
     */
    void prefetchNextSegmentBatch(const MediaTuple &tuple, uint64_t start_time, uint64_t max_duration, bool seek = false);

    /**
     * Open the next batch of segments prepared by prefetchNextSegmentBatch.  
     * This is called when we reach the end of the current batch, to seamlessly switch to the next batch without blocking I/O.
     */
    void openNextSegmentBatch();

private: 
    std::map<int, Track::Ptr> _tracks;
    std::map<uint64_t, MP4Demuxer::Ptr>::iterator _it;
    std::map<uint64_t, MP4Demuxer::Ptr> _demuxers;
    bool _use_timeline = false;
    SegmentBatchState _stats;
    OnTracksChangedCB _on_tracks_changed;
    // timeline_key → start_cut_ms: for lo-fill entries that don't start at file pos 0.
    // Used to correct the output DTS: actual_output_dts = timeline_key + file_dts - start_cut.
    std::map<uint64_t, uint64_t> _demuxer_start_cuts;
    // For each entry in _demuxers (keyed by timeline_offset_ms), stores the
    // DTS (ms, relative to file start) at which to stop reading that file and
    // switch to the next one. This trims the overlapping tail of each file so
    // that the next file can be started from position 0 (its IDR frame).
    // Absent entry = read to natural EOF.
    std::map<uint64_t, uint64_t> _demuxer_stop_offsets;
    // Sorted segment list used in multi-stream timeline mode
    std::vector<SegmentEntry> _current_batch;  // sorted by timeline offset (file start time)
    SegmentNextBatch _next_batch;
};

} // namespace mediakit
#endif//ENABLE_MP4
#endif //S3MEDIAKIT_MP4DEMUXER_H