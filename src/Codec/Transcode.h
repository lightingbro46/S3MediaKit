#ifndef S3MEDIAKIT_TRANSCODE_H
#define S3MEDIAKIT_TRANSCODE_H

#if defined(ENABLE_FFMPEG)

#include "Util/TimeTicker.h"
#include "Util/util.h"
#include "Common/MediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "libswscale/swscale.h"
#include "libavutil/avutil.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/imgutils.h"
#include "libavutil/frame.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#ifdef __cplusplus
}
#endif

#define FF_CODEC_VER_7_1 AV_VERSION_INT(61, 0, 0)

namespace mediakit {

/** Calculate automatic bitrate from output size, codec and frame rate. */
int getDefaultTranscodeBitrate(CodecId codec, int width, int height, int fps);

/** Resolve output dimensions and cap them at 1920x1080 while preserving aspect ratio. */
void getTranscodeOutputSize(int source_width, int source_height,
                            int requested_width, int requested_height,
                            int &output_width, int &output_height);

class FFmpegFrame {
public:
    using Ptr = std::shared_ptr<FFmpegFrame>;

    FFmpegFrame(std::shared_ptr<AVFrame> frame = nullptr);
    ~FFmpegFrame();

    AVFrame *get() const;
    void fillPicture(AVPixelFormat target_format, int target_width, int target_height);
    int getChannels() const;
    void reset();
    FFmpegFrame::Ptr clone() const;

private:
    std::unique_ptr<char[]> _data;
    std::shared_ptr<AVFrame> _frame;
};

class FFmpegSwr {
public:
    using Ptr = std::shared_ptr<FFmpegSwr>;

# if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    FFmpegSwr(AVSampleFormat output, AVChannelLayout *ch_layout, int samplerate);
#else
    FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate);
#endif

    ~FFmpegSwr();
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame);

private:

# if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    AVChannelLayout _target_ch_layout;
#else
    int _target_channels;
    int _target_channel_layout;
#endif

    int _target_samplerate;
    AVSampleFormat _target_format;
    SwrContext *_ctx = nullptr;

    toolkit::ResourcePool<FFmpegFrame> _swr_frame_pool;
};

class TaskManager {
public:
    virtual ~TaskManager();

    void setMaxTaskSize(size_t size);
    void stopThread(bool drop_task);

protected:
    void startThread(const std::string &name);
    bool addEncodeTask(std::function<void()> task);
    bool addDecodeTask(bool key_frame, std::function<void()> task);
    bool isEnabled() const;

private:
    void onThreadRun(const std::string &name);

private:
    class ThreadExitException : public std::runtime_error {
    public:
        ThreadExitException() : std::runtime_error("exit") {}
    };

private:
    bool _decode_drop_start = false;
    bool _exit = false;
    size_t _max_task = 30;
    std::mutex _task_mtx;
    toolkit::semaphore _sem;
    toolkit::List<std::function<void()> > _task;
    std::shared_ptr<std::thread> _thread;
};

class FFmpegDecoder : public TaskManager {
public:
    using Ptr = std::shared_ptr<FFmpegDecoder>;
    using onDec = std::function<void(const FFmpegFrame::Ptr &)>;

    FFmpegDecoder(const Track::Ptr &track, int thread_num = 2, const std::vector<std::string> &codec_name = {});
    ~FFmpegDecoder() override;

    bool inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge = true);
    void setOnDecode(onDec cb);
    void flush();
    const AVCodecContext *getContext() const;

private:
    void onDecode(const FFmpegFrame::Ptr &frame);
    bool inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge);
    bool decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame);

private:
    // default merge frame
    bool _do_merger = true;
    toolkit::Ticker _ticker;
    onDec _cb;
    std::shared_ptr<AVCodecContext> _context;
    FrameMerger _merger{FrameMerger::h264_prefix};
    toolkit::ResourcePool<FFmpegFrame> _frame_pool;
    toolkit::ObjectStatistic<FFmpegDecoder> _statistic;
};

class FFmpegSws {
public:
    using Ptr = std::shared_ptr<FFmpegSws>;

    FFmpegSws(AVPixelFormat output, int width, int height);
    ~FFmpegSws();
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame);
    int inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data);

private:
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data);

private:
    int _target_width = 0;
    int _target_height = 0;
    int _src_width = 0;
    int _src_height = 0;
    SwsContext *_ctx = nullptr;
    AVPixelFormat _src_format = AV_PIX_FMT_NONE;
    AVPixelFormat _target_format = AV_PIX_FMT_NONE;
    toolkit::ResourcePool<FFmpegFrame> _sws_frame_pool;
};

// Encode decoded FFmpegFrame(s) into a compressed elementary stream (default H.264
// via libx264) and emit them as mediakit Frame::Ptr, ready to be muxed (e.g. into
// an FMP4MediaSourceMuxer).  The encoder context is opened lazily on the first
// input frame so the output resolution can default to the source resolution.
class FFmpegEncoder {
public:
    using Ptr = std::shared_ptr<FFmpegEncoder>;
    using onEnc = std::function<void(const Frame::Ptr &)>;

    /**
     * @param codec    Target codec (CodecH264 or CodecH265).
     * @param width    Output width  (0 = keep source width).
     * @param height   Output height (0 = keep source height).
     * @param fps      Output frame rate (<=0 = 5).
     * @param bitrate  Target bitrate in bits/sec (<=0 = pick a sane default).
     * @param gop      Keyframe interval in frames (<=0 = 2*fps).
     */
    FFmpegEncoder(CodecId codec, int width = 0, int height = 0, int fps = 5, int bitrate = 0, int gop = 0);
    ~FFmpegEncoder();

    bool inputFrame(const FFmpegFrame::Ptr &frame);
    void setOnEncode(onEnc cb);
    void flush();
    /** Force the next encoded frame to be an IDR keyframe (e.g. on viewer resume). */
    void requestKeyFrame();
    const AVCodecContext *getContext() const;
    CodecId getCodecId() const { return _codec; }

private:
    bool openEncoder(const FFmpegFrame::Ptr &frame);
    bool encodeFrame(AVFrame *frame);
    void onEncode(AVPacket *pkt);

private:
    CodecId _codec;
    int _width = 0;
    int _height = 0;
    int _fps = 5;
    int _bitrate = 0;
    int _gop = 0;
    AVPixelFormat _enc_fmt = AV_PIX_FMT_YUV420P;
    bool _request_idr = false;
    int64_t _last_encoded_pts = AV_NOPTS_VALUE;
    onEnc _cb;
    std::shared_ptr<AVCodecContext> _context;
    FFmpegSws::Ptr _sws;
    toolkit::ObjectStatistic<FFmpegEncoder> _statistic;
};

class FFmpegUtils {
public:
    /**
     * Keep the image as jpeg or png
     * @param frame Decoded frames
     * @param filename Save file path
     * @param fmt jpg:AV_PIX_FMT_YUVJ420P，PNG:AV_PIX_FMT_RGB24
     * @param w h (optional) The size of the cropped image, the default is the same as the input source
     * @param font_path (optional), default DejaVuSans.ttf
     * @return
     */
    static std::tuple<bool, std::string> saveFrame(const FFmpegFrame::Ptr &frame, const char *filename, AVPixelFormat fmt = AV_PIX_FMT_YUVJ420P, int w = 0, int h = 0, const char *font_path = nullptr);

    /**
     * Encode a decoded frame to a JPEG byte buffer in memory (no disk I/O).
     * Useful for live MJPEG streaming without writing temporary files.
     * @param frame  Decoded source frame
     * @param fmt    AV_PIX_FMT_YUVJ420P (JPEG) or AV_PIX_FMT_RGB24 (PNG)
     * @param w, h   Optional target dimensions; 0 = keep source size
     * @return       Shared pointer to encoded bytes, or nullptr on failure
     */
    static std::shared_ptr<std::vector<uint8_t>> encodeFrameToBuffer(
        const FFmpegFrame::Ptr &frame,
        AVPixelFormat fmt = AV_PIX_FMT_YUVJ420P,
        int w = 0, int h = 0);

    /**
     * Draw motion detection results on the frame, and return the result as a new frame
     * @param frame Decoded frames
     * @param grid_rows The number of rows in the grid
     * @param grid_cols The number of columns in the grid
     * @return A tuple containing a boolean indicating success and a string with the result message
     */
    static std::tuple<bool, std::string> drawGrid(const FFmpegFrame::Ptr &frame, int grid_rows, int grid_cols);
};

}//namespace mediakit
#endif// ENABLE_FFMPEG
#endif //S3MEDIAKIT_TRANSCODE_H
