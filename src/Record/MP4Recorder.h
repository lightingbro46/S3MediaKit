#ifndef MP4MAKER_H_
#define MP4MAKER_H_

#include <mutex>
#include <memory>
#include <vector>
#include "Common/MediaSink.h"
#include "Record/Recorder.h"
#include "MP4Muxer.h"
#include "Thread/TaskExecutor.h"

namespace mediakit {

#ifdef ENABLE_MP4
class MP4Muxer;

class MP4Recorder final : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<MP4Recorder>;

    MP4Recorder(const MediaTuple &tuple, const std::string &path, size_t max_second);
    ~MP4Recorder() override;

    // Single-thread pool ("rec close N") that closes the segments of `stream_key`
    // (MediaTuple::shortUrl()); the same key always maps to the same pool.
    static toolkit::TaskExecutor::Ptr closeExecutorFor(const std::string &stream_key);

    struct ClosePoolStats {
        std::string name;
        size_t queued = 0;
        size_t running = 0;
        uint64_t completed = 0;
        uint64_t cancelled = 0;
        double oldest_queued_ms = 0;
        double wait_avg_ms = 0;
        double wait_max_ms = 0;
        double run_avg_ms = 0;
        double run_max_ms = 0;
    };
    // Snapshot of initialized shards, without starting the pools or resetting counters.
    static std::vector<ClosePoolStats> closePoolStats();

    // Event clips start on demand, not in a startup burst: keep their first file full length.
    // Call before the first frame.
    void disableStartStagger() { _first_max_second = _max_second; }

    /**
     * Reset all Tracks
     */
    void resetTracks() override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Refresh output all frame cache
     */
    void flush() override;

    /**
     * Add ready state track
     */
    bool addTrack(const Track::Ptr & track) override;

private:
    void createFile();
    void closeFile();
    void asyncClose();

private:
    bool _have_video = false;
    size_t _max_second;
    size_t _first_max_second;
    /**
     * Override the wall-clock time used to name the very next file created.
     * Pass 0 to clear the override (use current time).
     * Useful for event records that include backfilled GOP history so that
     * the first file name reflects the actual content start time.
     */
    time_t _next_file_time_ms = 0; // 0 = use ::time(NULL)
    bool _continuous_file_time = false; // true = next file time is continuous from previous file, false = next file time is wall-clock time
    DeltaStamp _delta_stamp[TrackMax];
    std::atomic<uint64_t> _file_index { 0 };
    std::string _full_path_tmp;
    RecordInfo _info;
    MP4Muxer::Ptr _muxer;
    std::list<Track::Ptr> _tracks;
};

#endif ///ENABLE_MP4

} /* namespace mediakit */

#endif /* MP4MAKER_H_ */
