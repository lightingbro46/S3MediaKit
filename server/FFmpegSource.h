#ifndef FFMPEG_SOURCE_H
#define FFMPEG_SOURCE_H

#include <mutex>
#include <memory>
#include <functional>
#include "Process.h"
#include "Util/TimeTicker.h"
#include "Common/MediaSource.h"

namespace FFmpeg {
    extern const std::string kSnap;
    extern const std::string kBin;
}

class FFmpegSnap {
public:
    using onSnap = std::function<void(bool success, const std::string &err_msg)>;
    /**
     * Create a screenshot
     * @param async Whether to use asynchronous screenshot method (not the ffmpeg command line, but use the zlm API, but only the stream pull protocol supported by the zlm player)
     * @param play_url The playback URL address, as long as FFmpeg supports it
     * @param save_path The path to save the screenshot JPEG file
     * @param timeout_sec Timeout for generating the screenshot (to prevent blocking for too long)
     * @param cb Callback for whether the screenshot was generated successfully
     */
    static void makeSnap(bool async, const std::string &play_url, const std::string &save_path, float timeout_sec, const onSnap &cb);

private:
    FFmpegSnap() = delete;
    ~FFmpegSnap() = delete;
};

class FFmpegSource : public std::enable_shared_from_this<FFmpegSource> , public mediakit::MediaSourceEventInterceptor{
public:
    using Ptr = std::shared_ptr<FFmpegSource>;
    using onPlay = std::function<void(const toolkit::SockException &ex)>;

    FFmpegSource();
    ~FFmpegSource();

    /**
     * Set the active close callback
     */
    void setOnClose(const std::function<void()> &cb);

    /**
     * Start playing the URL
     * @param ffmpeg_cmd_key FFmpeg stream command configuration item key, users can set multiple command parameter templates in the configuration file at the same time
     * @param src_url FFmpeg stream address
     * @param dst_url FFmpeg push stream address
     * @param timeout_ms Timeout for waiting for the result, in milliseconds
     * @param cb Success or failure callback
     */
    void play(const std::string &ffmpeg_cmd_key, const std::string &src_url, const std::string &dst_url, int timeout_ms, const onPlay &cb);

    const std::string& getSrcUrl() const { return _src_url; }
    const std::string& getDstUrl() const { return _dst_url; }
    const std::string& getCmd() const { return _cmd; }
    const std::string& getCmdKey() const { return _ffmpeg_cmd_key; }
    const mediakit::MediaInfo& getMediaInfo() const { return _media_info; }

    /**
     * Set recording
     * @param enable_hls Whether to enable HLS live streaming or recording
     * @param enable_mp4 Whether to record MP4
     */
    void setupRecordFlag(bool enable_hls, bool enable_mp4);

private:
    void findAsync(int maxWaitMS ,const std::function<void(const mediakit::MediaSource::Ptr &src)> &cb);
    void startTimer(int timeout_ms);
    void onGetMediaSource(const mediakit::MediaSource::Ptr &src);

    ///////MediaSourceEvent override///////
    // Close
    bool close(mediakit::MediaSource &sender) override;
    // Get the media source type
    mediakit::MediaOriginType getOriginType(mediakit::MediaSource &sender) const override;
    // Get the media source URL or file path
    std::string getOriginUrl(mediakit::MediaSource &sender) const override;

private:
    bool _enable_hls = false;
    bool _enable_mp4 = false;
    Process _process;
    toolkit::Timer::Ptr _timer;
    toolkit::EventPoller::Ptr _poller;
    mediakit::MediaInfo _media_info;
    std::string _src_url;
    std::string _dst_url;
    std::string _ffmpeg_cmd_key;
    std::string _cmd;
    std::function<void()> _onClose;
    toolkit::Ticker _replay_ticker;
};


#endif //FFMPEG_SOURCE_H
