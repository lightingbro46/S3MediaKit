#ifndef MP4MAKER_H_
#define MP4MAKER_H_

#include <mutex>
#include <memory>
#include "Common/MediaSink.h"
#include "Record/Recorder.h"
#include "MP4Muxer.h"

namespace mediakit {

#ifdef ENABLE_MP4
class MP4Muxer;

class MP4Recorder final : public MediaSinkInterface {
public:
    using Ptr = std::shared_ptr<MP4Recorder>;

    MP4Recorder(const MediaTuple &tuple, const std::string &path, size_t max_second);
    ~MP4Recorder() override;

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
     * Override the wall-clock time used to name the very next file created.
     * Pass 0 to clear the override (use current time).
     * Useful for event records that include backfilled GOP history so that
     * the first file name reflects the actual content start time.
     */
    void setNextFileTime(time_t t) { _next_file_time = t; }

private:
    void createFile();
    void closeFile();
    void asyncClose();

private:
    bool _have_video = false;
    size_t _max_second;
    time_t _next_file_time = 0; // 0 = use ::time(NULL)
    time_t _next_replay_file_time_offset = 0;
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
