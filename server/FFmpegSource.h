#ifndef FFMPEG_SOURCE_H
#define FFMPEG_SOURCE_H

#include <mutex>
#include <memory>
#include <functional>
#include <vector>
#include "Process.h"
#include "Util/TimeTicker.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "User/UserAuthorManager.h"
#include "Local/TimeQuery.h"

namespace mediakit {
struct PrivacyMaskRegion;
}

namespace FFmpeg {
    extern const std::string kSnap;
    extern const std::string kBin;
    extern const std::string kExtract;
    extern const std::string kRecordedThumbnailTimeoutSec;
}

class FFmpegSnap {
public:
    using onSnap = std::function<void(bool success, const std::string &err_msg)>;
    /**
     * Create a screenshot
     * @param async Whether to use asynchronous screenshot method (not the ffmpeg command line, but use the s3m API, but only the stream pull protocol supported by the s3m player)
     * @param play_url The playback URL address, as long as FFmpeg supports it
     * @param save_path The path to save the screenshot JPEG file
     * @param seek_time The time position to seek to before taking the screenshot, in seconds
     * @param timeout_sec Timeout for generating the screenshot (to prevent blocking for too long)
     * @param cb Callback for whether the screenshot was generated successfully
     */
    static void makeSnap(bool async, const std::string &play_url, const std::string &save_path, uint64_t seek_time, float timeout_sec, const onSnap &cb);

    // Create a JPEG through an optional filter_complex graph. The graph must
    // expose its final video as [v]. The caller owns temporary overlay files.
    static void makeSnapWithFilter(const std::string &play_url, const std::string &save_path,
                                   uint64_t seek_time, float timeout_sec,
                                   const std::string &filter_complex, const onSnap &cb);

private:
    FFmpegSnap() = delete;
    ~FFmpegSnap() = delete;
};

class RecordedThumbnailTask : public std::enable_shared_from_this<RecordedThumbnailTask> {
public:
    using Ptr = std::shared_ptr<RecordedThumbnailTask>;

    struct Request {
        mediakit::MediaTuple tuple;
        std::string source_path;
        int64_t segment_start_time = 0;
        int64_t segment_size = 0;
        uint64_t seek_time = 0;
        std::string output_path;
        std::string filter_complex;
        std::vector<std::string> temporary_paths;
        std::string restore_reason;
        float timeout_sec = 5.0f;
    };

    struct Result {
        bool success = false;
        bool timeout = false;
        bool storage_unavailable = false;
        bool restore_unavailable = false;
        bool recording_unavailable = false;
        std::string stage = "RESOLVING";
        float progress_percent = 0.0f;
        std::string error;
    };

    using onComplete = std::function<void(const Result &result)>;

    RecordedThumbnailTask(const Request &request, const onComplete &cb,
                          toolkit::EventPoller::Ptr poller = nullptr);
    ~RecordedThumbnailTask();
    void start();

private:
    bool tick();
    void startSnap(const std::string &source_path);
    void onSnap(bool success, const std::string &error);
    void complete(const Result &result);
    void releaseDeferredResources();

private:
    Request _request;
    onComplete _on_complete;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    toolkit::Ticker _ticker;
    Ptr _self_holder;
    std::string _restore_cache_job_id;
    std::string _temporary_output_path;
    std::string _stage = "RESOLVING";
    float _progress_percent = 0.0f;
    bool _snap_started = false;
    bool _completed = false;
    bool _cleanup_deferred = false;
    std::mutex _mutex;
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

struct ExtractOptions {
    uint64_t start_time;
    uint64_t end_time;
    std::string filename;
    std::string description;
    std::string user_id;
    std::string username;
    // Requesting user's JWT, used to resolve the camera's current watermark/privacy mask
    // policy (kBroadcastMediaViewOverlay) and apply the same role-based exclusion rules.
    std::string jwt_token;
    // Burn an extra bottom-right text stamp (camera name + extracting VMS username) onto the
    // extracted clip, independent of the camera's own watermark/privacy mask policy.
    bool enable_source_stamp;
};

// Builds the FFmpeg filter graph used by thumbnail and archived-video
// extraction. The implementation remains in FFmpegSource.cpp; this declaration
// allows the thumbnail API to use the same builder as FFmpegExtractor.
class FFmpegOverlayFilter {
public:
    static bool buildThumbnailOverlay(const mediakit::MediaTuple &tuple, const std::string &jwt_token,
                                      const std::string &path_prefix,
                                      std::string &filter_complex,
                                      std::vector<std::string> &temporary_paths,
                                      std::string &policy_key);

public:
    static std::string build(const mediakit::MediaTuple &tuple,
                             const mediakit::Broadcast::ViewOverlayPolicy &policy,
                             const ExtractOptions &extract_options,
                             const std::string &svg_path,
                             std::vector<std::string> &temporary_paths,
                             bool &overlay_required);

private:
    static std::string buildPrivacyMask(
        const std::vector<mediakit::PrivacyMaskRegion> &masks,
        int canvas_width, int canvas_height,
        const std::string &path_prefix,
        std::vector<std::string> &temporary_paths,
        std::string &last_label, bool &success);
};

class FFmpegExtractor : public std::enable_shared_from_this<FFmpegExtractor> {
public:
    using Ptr = std::shared_ptr<FFmpegExtractor>;
    using onExtract = std::function<void(const toolkit::SockException &ex)>;

    enum class Stage {
        QUEUED,
        RESTORING,
        EXTRACTING,
        COMPLETED,
        FAILED
    };

    struct PreviewInfo {
        bool hasData = false;
        uint64_t availableDuration = 0;
        std::string streamId;
        std::string streamName;
    };

    struct Status {
        float progress = 0.0f;
        float stage_progress = 0.0f;
        bool finished = false;
        bool success = false;
        int error_code = 0;
        std::string err_msg;
        std::string status = "QUEUED";
        std::string stage = "QUEUED";
        std::string selected_stream_id;
    };
    using OnClose = std::function<void()>;
    using OnComplete = std::function<void(const Status &status,
                                          const std::string &output_path,
                                          uint64_t duration_seconds,
                                          uint64_t size_bytes)>;
    
    FFmpegExtractor(mediakit::MediaTuple &tuple, ExtractOptions &options, int timeout_ms = 500, toolkit::EventPoller::Ptr poller = nullptr);
    ~FFmpegExtractor();

    /**
     * Set user session cache, which will be used for permission verification when creating recording tasks and generating download links. 
     * If the session cache is not set, it will be considered as having no permission.
     */
    void setSessionCache(const managerkit::UserSessionCache::Ptr &session) { _session = session; }

    /**
     * Set the active close callback
     */
    void setOnClose(const OnClose &cb);
    void setOnComplete(const OnComplete &cb);
    void setAutoCleanup(bool enabled) { _auto_cleanup = enabled; }

    void makeExtract(const std::string &key, const std::string &download_path, const onExtract &cb);
    // Remove manifests/logs/overlay images while retaining the extracted media
    // for a persistent upload retry.
    void cleanupTemporaryFiles();
    void cleanup();
    void cancel();

    static PreviewInfo getExtractPreview(const std::string &camera_id, const std::string &stream_id,
                                         uint64_t start_time, uint64_t end_time);

    const std::string& getFilename() const { return _options.filename; }
    const std::string& getSavePath() const { return _save_path; }
    const std::string& getCmd() const { return _cmd; }
    const std::string& getCameraId() const { return _tuple.app; }
    Status status() const;
    float progress() const { return status().progress; }
    bool finished() const { return status().finished; }
    bool success() const { return status().success; }
    std::string errMsg() const { return status().err_msg; }

    static const char *stageToString(Stage stage);
    static float lifecycleProgress(Stage stage, float stage_progress);

private:
    // create txt file include mp4 list
    void create_src_path(std::string &src_path);
    // create Timer to check ffmpeg status
    void startTimer();
    void prepareExtractSource();
    void startRestore();
    bool pollRestore();
    void startFfmpeg();
    void updateLifecycleStatus(Stage stage, float stage_progress);
    void notifyStartOnce(const toolkit::SockException &ex);
    bool retainRestoreCache();
    void releaseRestoreCache();
    // create delay task to close process
    void closeAfterDelaySec();
    // Close
    bool close();
    // Emit event to notify the result of the extract task
    void emitAuditEvent(bool success, const std::string &err_msg = "");
    void completeOnce(bool success, const std::string &err_msg = "", int error_code = 0);

private:
    mediakit::MediaTuple _tuple;
    ExtractOptions _options;
    Process _process;
    toolkit::Ticker _ticker;
    toolkit::Ticker _restore_ticker;
    toolkit::Timer::Ptr _timer;
    toolkit::EventPoller::Ptr _poller;
    std::string _src_path;
    std::string _save_path;
    std::vector<std::string> _overlay_temp_paths;
    std::vector<TimeBlock> _time_blocks;
    std::string _log_file;
    std::string _cmd;
    OnClose _onClose;
    OnComplete _onComplete;
    onExtract _onStart;
    std::string _extract_key;
    std::string _extract_root;
    std::string _restore_job_id;
    std::string _retained_restore_job_id;
    uint64_t _created_at;
    int _timeout_ms;
    uint32_t _duration = 0;
    float _progress = 0.0;
    bool _finished = false;
    bool _success = false;
    bool _complete_emitted = false;
    bool _started = false;
    // Set by cancel(); a prepare step still queued on _poller then does nothing.
    bool _cancelled = false;
    bool _start_emitted = false;
    bool _restore_cache_retained = false;
    bool _restore_started = false;
    bool _auto_cleanup = true;
    Stage _stage = Stage::QUEUED;
    float _stage_progress = 0.0f;
    int _error_code = 0;
    std::string _err_msg;
    mutable std::mutex _status_mtx;
    managerkit::UserSessionCache::Ptr _session;
};

struct ProbeInfo {
    std::string url;
    bool hasVideo = false;
    std::string vcodec;
    int width = 0;
    int height = 0;
    int bitrate = 0;
    float fps = 0.0;
    float quality = 0.0;
};

class FFmpegProbe {
public:
    using onProbe = std::function<void(bool success, const std::string &err_msg, const ProbeInfo &info)>;
    /**
     * Probe url to get source information
     * @param play_url The playback URL address, as long as FFmpeg supports it
     * @param timeout_sec Timeout for probe url (to prevent blocking for too long)
     * @param cb Callback for whether the screenshot was generated successfully
     */
    static void makeProbe(const std::string &play_url, float timeout_sec, const onProbe &cb, const toolkit::EventPoller::Ptr &poller = nullptr);

private:
    FFmpegProbe() = delete;
    ~FFmpegProbe() = delete;
};

#endif // FFMPEG_SOURCE_H
