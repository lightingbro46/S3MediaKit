#ifndef S3MEDIAKIT_MPEG_H
#define S3MEDIAKIT_MPEG_H

#if defined(ENABLE_HLS) || defined(ENABLE_RTPPROXY)

#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Common/MediaSink.h"
#include "Util/ResourcePool.h"
namespace mediakit {

// This class is used to generate MPEG-TS/MPEG-PS
class MpegMuxer : public MediaSinkInterface {
public:
    MpegMuxer(bool is_ps = false);
    ~MpegMuxer() override;

    /**
     * Add audio and video tracks
     */
    bool addTrack(const Track::Ptr &track) override;

    /**
     * Reset audio and video tracks
     */
    void resetTracks() override;

    /**
     * Input frame data
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Flush all frame buffers in the output
     */
    void flush() override;

protected:
    /**
     * Callback for outputting ts/ps data
     * @param buffer ts/ps data packet
     * @param timestamp Timestamp, in milliseconds
     * @param key_pos Whether it is the first ts/ps packet of a key frame, used to ensure that the first frame of the ts slice is a key frame
     */
    virtual void onWrite(std::shared_ptr<toolkit::Buffer> buffer, uint64_t timestamp, bool key_pos) = 0;

private:
    void createContext();
    void releaseContext();
    void onWrite_l(const void *packet, size_t bytes);
    void flushCache();

private:
    bool _is_ps = false;
    bool _have_video = false;
    bool _key_pos = false;
    uint32_t _max_cache_size = 0;
    uint64_t _timestamp = 0;
    struct mpeg_muxer_t *_context = nullptr;

    class FrameMergerImp : public FrameMerger {
    public:
        FrameMergerImp() : FrameMerger(FrameMerger::h264_prefix) {}
    };

    struct MP4Track {
        int track_id = -1;
        FrameMergerImp merger;
    };
    std::unordered_map<int, MP4Track> _tracks;
    toolkit::BufferRaw::Ptr _current_buffer;
    toolkit::ResourcePool<toolkit::BufferRaw> _buffer_pool;
};

}//mediakit

#else

#include "Common/MediaSink.h"

namespace mediakit {

class MpegMuxer : public MediaSinkInterface {
public:
    MpegMuxer(bool is_ps = false) {}
    bool addTrack(const Track::Ptr &track) override { return false; }
    void resetTracks() override {}
    bool inputFrame(const Frame::Ptr &frame) override { return false; }

protected:
    virtual void onWrite(std::shared_ptr<toolkit::Buffer> buffer, uint64_t timestamp, bool key_pos) = 0;
};

}//namespace mediakit

#endif

#endif //S3MEDIAKIT_MPEG_H
