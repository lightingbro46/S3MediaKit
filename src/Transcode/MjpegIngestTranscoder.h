#ifndef TRANSCODE_MJPEGINGESTTRANSCODER_H
#define TRANSCODE_MJPEGINGESTTRANSCODER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Extension/Track.h"
#include "Poller/EventPoller.h"

namespace mediakit {

/**
 * Re-encodes the MJPEG video track of a pulled camera once at ingest so every protocol, recording and
 * processor downstream sees H264/H265 (browsers, RTMP/FLV and MPEG-TS cannot carry MJPEG).
 *
 * Wiring (PlayerProxy):
 *   JPEGTrack --addDelegate--> MjpegIngestTranscoder --> outputTrack() --addDelegate--> MultiMediaSourceMuxer
 *
 * outputTrack() is an H264/H265 track with the source track index and getOriginCodecId() == CodecJPEG, so
 * the APIs can report both the delivered codec and the camera codec.
 *
 * Threading: inputFrame() runs on the player poller and only queues the JPEG frame. Decoding and encoding
 * run on the FFmpegDecoder's own thread, never on a poller. Encoded frames are posted back to the player
 * poller, which owns outputTrack() and the muxer behind it.
 */
class MjpegIngestTranscoder : public FrameWriterInterface {
public:
    using Ptr = std::shared_ptr<MjpegIngestTranscoder>;

    /**
     * Create a transcoder for a JPEG video track when mjpeg.ingestTranscode is on, FFmpeg is built in and
     * mjpeg.ingestMaxSessions is not reached; returns nullptr otherwise (the caller passes the track through).
     */
    static Ptr create(const Track::Ptr &source, const toolkit::EventPoller::Ptr &poller);

    /** Whether new pulls of a JPEG track should be transcoded (config on and FFmpeg built in). */
    static bool enabled();

    /** Number of live transcoders. */
    static size_t activeCount();

    ~MjpegIngestTranscoder() override;

    /** Encoded output track to add to the muxer in place of the source track. */
    const Track::Ptr &outputTrack() const { return _output; }

    /** JPEG frame from the source track (player poller). */
    bool inputFrame(const Frame::Ptr &frame) override;

private:
    class Impl;

    MjpegIngestTranscoder(const Track::Ptr &source, const toolkit::EventPoller::Ptr &poller, CodecId codec);
    void onEncoded(const Frame::Ptr &frame);

private:
    Track::Ptr _source;
    Track::Ptr _output;
    toolkit::EventPoller::Ptr _poller;
    std::weak_ptr<MjpegIngestTranscoder> _weak_self;
    std::atomic<uint64_t> _frames_out { 0 };
    uint64_t _frames_in = 0;
    // Owns the decoder thread; reset first in the destructor so no callback outlives the members above.
    std::unique_ptr<Impl> _impl;
};

} // namespace mediakit

#endif // TRANSCODE_MJPEGINGESTTRANSCODER_H
