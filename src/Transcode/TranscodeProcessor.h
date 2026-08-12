#ifndef TRANSCODE_TRANSCODEPROCESSOR_H
#define TRANSCODE_TRANSCODEPROCESSOR_H

#if defined(ENABLE_FFMPEG)

#include <functional>
#include <memory>
#include <string>

#include "Codec/Transcode.h"
#include "Common/macros.h"
#include "TranscodeOverlay.h"
#include "Common/MediaSink.h"
#include "Common/MediaSource.h"
#include "Util/util.h"

namespace mediakit {

class MultiMediaSourceMuxer;

struct TranscodeRequest {
    bool enabled = false;
    CodecId codec = CodecH264;
    int width = 0;
    int height = 0;
    int bitrate = 0; // bits/sec, 0 = automatic
    int fps = 5;
    int gop = 0;
};

/** Parse the protocol-independent transcode request parameters. */
bool parseTranscodeRequest(const std::string &params,
                           const ProtocolOption &defaults,
                           TranscodeRequest &request,
                           std::string &error);

/**
 * TranscodeProcessor builds an on-demand, re-encoded video stream from the
 * decoded frames of an existing MediaSource.  It mirrors how http-mp4 obtains
 * its data (reads from a MediaSource) but re-encodes the pixels, optionally
 * applying an image overlay (privacy mask / watermark), and publishes the
 * result through the existing MultiMediaSourceMuxer under a derived stream_id.
 *
 * Pipeline (per decoded video frame):
 *   FFmpegFrame → TranscodeOverlay → FFmpegEncoder(H264) → Frame::Ptr
 *              → MediaSink (track-ready mgmt) → MultiMediaSourceMuxer
 *
 * Audio is passed through unchanged (no transcode).
 *
 * On-demand (transcode_demand=true): encoding follows the muxer's demand gate;
 * the derived source is closed by MediaSourceEvent after the no-reader delay,
 * then the owning MultiMediaSourceProcessor removes this instance.
 *
 * Threading: inputVideoFrame() is called by the processor's video ring reader
 * on its poller; inputAudioFrame() remains on the source processing thread.
 */
class TranscodeProcessor
    : public MediaSink
    , public MediaSourceEventInterceptor
    , public std::enable_shared_from_this<TranscodeProcessor> {
public:
    using Ptr = std::shared_ptr<TranscodeProcessor>;

    struct Config {
        CodecId codec = CodecH264;   // output video codec (H264/H265)
        int width = 0;               // 0 = keep source width
        int height = 0;              // 0 = keep source height
        int fps = 5;
        int bitrate = 0;             // bits/sec, 0 = automatic
        int gop = 0;                 // frames, 0 = 2*fps
        bool demand = true;          // on-demand gating
        std::string overlay_image;   // path to overlay image (empty = no overlay)
        int overlay_x = 0;
        int overlay_y = 0;
        std::vector<OverlayComponent> overlay_components;
        OverlayBuildOptions overlay_options;
        std::string stream_suffix = ".transcode";
        std::string output_schema;
    };

    TranscodeProcessor(const MediaTuple &tuple, const ProtocolOption &option, Config cfg, const toolkit::EventPoller::Ptr &poller = nullptr);
    ~TranscodeProcessor() override;

    /** Wire the outer MediaSourceEvent listener; call after make_shared. */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener);

    /** Called after the delayed output close has completed. */
    void setOnClosed(const std::function<void(const Ptr &)> &callback);

    /** Register the original audio track for pass-through muxing. */
    void addAudioTrack(const Track::Ptr &track);

    /** Signal that all upstream tracks have been announced. */
    void finalizeTracks();

    /** Feed a decoded video frame (from the shared FFmpegDecoder). */
    bool inputVideoFrame(const FFmpegFrame::Ptr &frame);

    /** Feed an original audio frame for pass-through. */
    bool inputAudioFrame(const Frame::Ptr &frame);

    int totalReaderCount() const;

    /** Number of live TranscodeProcessor instances (transcode stream count). */
    static size_t totalCount();

protected:
    // MediaSink overrides
    bool onTrackReady(const Track::Ptr &track) override;
    void onAllTrackReady() override;
    bool onTrackFrame(const Frame::Ptr &frame) override;
    void onReaderChanged(MediaSource &sender, int size) override;
    bool close(MediaSource &sender) override;

private:
    void createMuxer();
    void addTrackToMuxer(const Track::Ptr &track);
    void completeMuxerTracks();

    MediaTuple _tuple;               // derived stream_id
    ProtocolOption _option;
    Config _cfg;
    toolkit::EventPoller::Ptr _poller;
    bool _have_audio = false;
    bool _primed = false;
    bool _last_enabled = false;
    std::function<void(const Ptr &)> _on_closed;
    int64_t _last_overlay_input_pts = AV_NOPTS_VALUE;
    FFmpegSws::Ptr _pre_overlay_sws;
    int _pre_overlay_width = 0;
    int _pre_overlay_height = 0;

    std::shared_ptr<MultiMediaSourceMuxer> _muxer;
    FFmpegEncoder::Ptr _encoder;
    TranscodeOverlay::Ptr _overlay;

    toolkit::ObjectStatistic<TranscodeProcessor> _statistic;
};

} // namespace mediakit

#endif // ENABLE_FFMPEG

#endif // TRANSCODE_TRANSCODEPROCESSOR_H
