#ifndef S3MANAGERKIT_MEDIAGAPFILLMANAGER_H
#define S3MANAGERKIT_MEDIAGAPFILLMANAGER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <json/json.h>

#include "Camera/GenericRtspCamera.h"
#include "Control/OnvifDeviceControl.h"
#include "Storage/LocalResourceStatusGap.h"
#include "Storage/MediaGapFillTask.h"
#include "Storage/MediaGapFillSession.h"
#include "Poller/Timer.h"

namespace managerkit {

struct MediaGapFillRequest {
    std::string id;
    std::string source_type;
    std::string gap_id;
    std::string session_id;
    std::string device_id;
    std::string stream_id;
    std::string origin_url;
    std::string username;
    std::string password;
    uint64_t source_begin_time = 0;
    uint64_t begin_time = 0;
    uint64_t end_time = 0;
    uint64_t disconnect_time = 0;
    uint64_t reconnect_time = 0;
    int32_t segment_index = -1;
    int retry_limit = 0;
};

struct MediaGapDiscoveryResult {
    bool success = true;
    std::vector<MediaGapFillRequest> requests;
    std::string error;
};

struct MediaGapStreamMapping {
    std::string stream_id;
    size_t recording_index = 0;
};

struct MediaGapPlaybackProgress {
    uint64_t processed_up_to = 0;
    bool completed = false;
};

class MediaGapFillManager {
public:
    using DiscoverCallback = std::function<MediaGapDiscoveryResult(const LocalResourceStatusGap &)>;
    using ClosedCallback = std::function<void(bool completed, uint64_t processed_up_to,
                                               const std::string &reason)>;
    using StartCallback = std::function<bool(const MediaGapFillRequest &, const ClosedCallback &)>;
    using StopCallback = std::function<void(const MediaGapFillRequest &request)>;
    using ProgressCallback = std::function<bool(const MediaGapFillRequest &request,
                                                 uint64_t &processed_up_to)>;
    using PrepareCallback = std::function<void(MediaGapFillRequest &request)>;

    struct SourceProvider {
        DiscoverCallback discover;
        StartCallback start;
        StopCallback stop;
        ProgressCallback progress;
        PrepareCallback prepare;
    };

    static MediaGapFillManager &Instance();

    // Compatibility entry point for the old SD-card sync lifecycle.
    void onManager(const std::string &device_id, const std::string &username,
                   const std::string &password, const OnvifControl::Ptr &onvif,
                   const SdCardSyncConfig &config, bool ready = false);

    bool registerSource(const std::string &source_type, SourceProvider provider);
    void unregisterSource(const std::string &source_type);

    size_t fillDevice(const std::string &device_id, const std::string &source_type);
    bool enqueue(MediaGapFillRequest request);
    bool stop(const std::string &connection_id);
    bool resetFailedSession(const std::string &device_id, const std::string &session_id);
    bool resetFailedSegment(const std::string &device_id, const std::string &session_id,
                            const std::string &stream_id, int32_t segment_index);
    bool stopSession(const std::string &device_id);
    bool stopSegment(const std::string &device_id, const std::string &stream_id,
                     int32_t segment_index);
    void onDeviceStateChanged(const std::string &device_id, bool connected,
                              const std::string &source_type = std::string());
    void setDefaultSourceType(const std::string &source_type);
    Json::Value makeRemainingSegmentsJson(const std::string &device_id);
    Json::Value makeDisconnectSessionJson(const std::string &device_id,
                                          const std::string &session_id);
    Json::Value makeDisconnectSessionsJson(const std::string &device_id);
    void setMaxConnections(size_t max_connections);
    void checkpointProgress();

    static std::vector<MediaGapStreamMapping> mapRecordingsToStreams(
        const std::unordered_map<std::string, VideoEncoderConfig> &encoder_configs,
        const std::vector<RecordingInformation> &recordings);

    static MediaGapPlaybackProgress calculatePlaybackProgress(
        uint64_t begin_time, uint64_t end_time, double raw_progress);
    static uint64_t calculateRetryDelay(int retry_count);

    static SourceProvider makeRtspPlaybackProvider(DiscoverCallback discover);

private:
    struct ConnectionState {
        MediaGapFillRequest request;
        uint64_t processed_up_to = 0;
        uint64_t attempt_begin = 0;
        uint64_t generation = 0;
        uint64_t next_retry_time = 0;
        int retry_count = 0;
    };

    MediaGapFillManager();

    void schedule();
    void onConnectionClosed(const std::string &connection_id, uint64_t generation, bool completed,
                            uint64_t processed_up_to, const std::string &reason);
    void updateSessionCompletion(const MediaGapFillRequest &request);
    void armRetryTimer(uint64_t wakeup_time);
    static bool validRequest(const MediaGapFillRequest &request);
    static Json::Value sessionToJson(const std::vector<MediaGapFillTask> &tasks);

private:
    std::recursive_mutex _mtx;
    std::map<std::string, SourceProvider> _providers;
    std::deque<ConnectionState> _pending;
    std::map<std::string, ConnectionState> _active;
    size_t _max_connections = 4;
    std::string _default_source_type = "sdcard";
    std::map<std::string, std::string> _device_source_types;
    std::map<std::pair<std::string, std::string>, bool> _paused_sessions;
    std::map<std::string, bool> _paused_streams;
    uint64_t _next_generation = 0;
    toolkit::Timer::Ptr _progress_timer;
    toolkit::Timer::Ptr _retry_timer;
    uint64_t _retry_wakeup_time = 0;
};

} // namespace managerkit

#endif // S3MANAGERKIT_MEDIAGAPFILLMANAGER_H
