#ifndef S3MEDIAKIT_MP4DEMUXER_H
#define S3MEDIAKIT_MP4DEMUXER_H
#ifdef ENABLE_MP4

#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
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

struct SegmentStats {
    MediaTuple tuple;
    uint64_t start_time = 0;
    uint64_t total_dur = 0;
    uint64_t next_time = 0;
    uint64_t first_time = 0;
};

class MultiMP4Demuxer : public TrackSource {
public:
    using Ptr = std::shared_ptr<MultiMP4Demuxer>;

    ~MultiMP4Demuxer() override = default;

    /**
     * Open mp4 files in batches and treat multiple files as one mp4
     * @param file Multiple mp4 file paths, separated by semicolons; or folders containing multiple mp4 files
     */
    void openMP4(const std::string &file);

    /**
     * Limit a ".../vod/<stamp>" replay to the time window [stamp, stamp + duration_s].
     * Call before openMP4(); 0 (default) plays on and follows new recordings.
     */
    void setMaxDuration(uint64_t duration_s) { _max_duration_s = duration_s; }

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

private:
    void openMP4WithTimeline(const std::string &files);

    int64_t findNextSegment(bool first_segment = false, uint64_t max_duration = 300);

    int64_t seekToWithTimeline(int64_t stamp_ms);

    Frame::Ptr readFrameWithTimeline(bool &keyFrame, bool &eof);

    // Queues the lookup of the batch after the current one on the "replay next" pool, so the
    // reader does not read the recording index on its poller when it reaches the end of a batch.
    void prefetchNextSegment();

    struct NextSegmentLookup {
        std::mutex mtx;
        std::condition_variable done;
        bool pending = false;
        bool ready = false;
        uint64_t start = 0;          // the stamp the lookup was made for
        uint64_t max_duration = 0;
        uint64_t duration = 0;       // findSegmentFiles() result
        std::map<uint64_t, std::string> files;
    };

private:
    std::map<int, Track::Ptr> _tracks;
    std::map<uint64_t, MP4Demuxer::Ptr>::iterator _it;
    std::map<uint64_t, MP4Demuxer::Ptr> _demuxers;
    bool _use_timeline = false;
    uint64_t _max_duration_s = 0;
    SegmentStats _stats;
    std::set<std::pair<uint64_t, std::string>> _timeline_segments;
    std::set<std::pair<uint64_t, std::string>> _consumed_timeline_segments;
    // Shared with the lookup task, which may outlive the demuxer.
    std::shared_ptr<NextSegmentLookup> _next_lookup = std::make_shared<NextSegmentLookup>();
};

}//namespace mediakit
#endif//ENABLE_MP4
#endif //S3MEDIAKIT_MP4DEMUXER_H
