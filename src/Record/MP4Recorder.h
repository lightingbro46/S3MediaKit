#ifndef MP4MAKER_H_
#define MP4MAKER_H_

#include <mutex>
#include <memory>
#include <list>
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

    // Segments that could not be opened or renamed into place since startup (health monitor).
    static uint64_t writeErrorCount();

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

    // A kBroadcastRecordMP4 listener reports how long its part took; the times are logged with
    // "MP4 close timing" of that segment. No-op outside a segment close.
    static void addCloseStep(const char *name, uint64_t ms);

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

    /**
    * Handles a seamless reconnect without resetting tracks.
    * The current file remains open until the first key frame; it is closed only
    * when the resolution changes.
    * Must be called on the inputFrame() thread.
    */
    void markInterrupted();

private:
    struct VideoConfigState {
        // Codec configuration carried by the frames entering the recorder, kept in a scratch track fed with
        // their parameter sets. It can be ahead of _tracks (the camera changed the configuration in-band, or
        // across a seamless reconnect) or behind it (history replayed from the GOP cache).
        Track::Ptr stream_track;
        // Resolution the open file was opened with; 0x0 when it is unknown or must not be checked.
        int file_width = 0;
        int file_height = 0;
        // Set when the stream changed resolution: the file is closed at the next key frame so that one mp4
        // never holds two resolutions.
        bool changed = false;
        // First frame skipped because the track header cannot describe it, and whether such a skip is running.
        uint64_t mismatch_dts = 0;
        bool mismatch = false;
        // Set by markInterrupted(): nothing is written before the next video key frame.
        bool interrupted = false;
        // The open file was just closed by a video config change: the next file created gets start_time + 1s.
        bool after_change_cut = false;
        // Parameter sets kept out of the file for now, written in front of the key frame they belong to.
        std::list<Frame::Ptr> held_config;

        // Back to the state of a recorder without tracks.
        void reset() { *this = VideoConfigState(); }
    };

    void createFile();
    // drop_short_segment: the file is closed because the video configuration changed; if it holds less
    // than kMinConfigChangeSegmentSec it is deleted instead of being published.
    void closeFile(bool drop_short_segment = false);
    void asyncClose(bool drop_short_segment);

    // Keeps the scratch track fed with the parameter sets carried by the incoming frames.
    void trackStreamConfig(const Frame::Ptr &frame);
    // Resolution the incoming frames are encoded with; 0x0 until a parameter set was seen.
    void streamResolution(int &width, int &height);
    // Resolution of the video track bound to this recorder, which is what its files are opened with.
    void videoResolution(int &width, int &height) const;
    // Parameter sets kept out of the file for now (config change, interruption, incomplete set): held so
    // that they can be written in front of the key frame they belong to.
    void holdVideoFrame(const Frame::Ptr &frame);
    void writeHeldConfig();

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
    VideoConfigState _cfg;
};

#endif ///ENABLE_MP4

} /* namespace mediakit */

#endif /* MP4MAKER_H_ */
