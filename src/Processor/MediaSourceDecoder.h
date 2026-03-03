#ifndef PROCESSOR_VIDEODECODER_H
#define PROCESSOR_VIDEODECODER_H

#include "Common/MediaSink.h"
#include "Common/MediaSource.h"
#include "Codec/Transcode.h"
#include "Common/PacketCache.h"
#include "Util/RingBuffer.h"

#define FRAME_CACHE_SIZE 50

namespace mediakit {

// class RingFFmpegFrame : public toolkit::RingDelegate<FFmpegFrame::Ptr> {
// public:
//     using Ptr = std::shared_ptr<RingFFmpegFrame>;
//     using RingDataType = std::shared_ptr<toolkit::List<FFmpegFrame::Ptr>>;
//     using RingType = toolkit::RingBuffer<RingDataType>;

//     RingFFmpegFrame(const MediaTuple& tuple, int ring_size = FRAME_CACHE_SIZE) : _ring_size(ring_size) {}

//     ~RingFFmpegFrame() override {
//         try {
//             flush();
//         } catch (std::exception &ex) {
//             WarnL << ex.what();
//         }
//     }

//     const RingType::Ptr &getRing() const {
//         return _ring;
//     }

//     void onWrite(FFmpegFrame::Ptr in, bool is_key) override;

// private:
//     bool _have_video = false;
//     RingType::Ptr _ring;
//     int _ring_size;
// };

/**
 * MediaSourceDecoder is responsible for decoding video frames, 
 * and the decoded frames will be forwarded to the next module in the stack, 
 * such as motion detection module or other video processing modules
 */
class MediaSourceDecoder : public MediaSinkInterface {
public:
    ~MediaSourceDecoder() override;
    
    /**
     * Add tracks that are in ready state
     */
    bool addTrack(const Track::Ptr &track) override;

    /**
     * Input frame
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * Reset all tracks
     */
    void resetTracks() override;

    /**
     * Refresh all frame cache output
     */
    void flush() override;

    /**
     * Whether it contains video
     */
    bool haveVideo() const;

protected:
    /**
     * Decode callback
     */
    virtual void onDecode(const FFmpegFrame::Ptr &frame) = 0;

protected:
    bool _started = false;
    bool _track_existed[2] = { false, false };

    struct TrackInfo {
        Stamp stamp;
        uint64_t ntp_stamp { 0 };
        FFmpegDecoder::Ptr decoder;
    };
    std::unordered_map<int, TrackInfo> _tracks;
    // RingFFmpegFrame::RingType::Ptr _ring;
    // RingFFmpegFrame::RingType::Ptr _ringInterceptor;
};

} // namespace mediakit

#endif // PROCESSOR_VIDEODECODER_H