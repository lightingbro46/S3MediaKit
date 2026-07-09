#ifndef S3MANAGERKIT_SDCARDSYNCMANAGER_H
#define S3MANAGERKIT_SDCARDSYNCMANAGER_H

#include "GenericRtspCameraImp.h"
#include "DisconnectSessionManager.h"
#include "Player/PlayerProxy.h"

namespace managerkit {

struct CameraContext {
    std::string device_id;
    std::string username;
    std::string password;
    DisconnectSessionManager tracker;

    std::string shortUrl(const std::string& stream_id) const {
        return std::string(DEFAULT_VHOST) + "/" + mediakit::kReplayPrefix + device_id + "/" + stream_id;
    }

    bool isValid() const {
        return !device_id.empty();
    }
};

class SdCardSyncManager : public std::enable_shared_from_this<SdCardSyncManager> {
public:

    static SdCardSyncManager& Instance();
    ~SdCardSyncManager() = default;

    void onManager(const std::string &device_id, const std::string &username, const std::string &password, const OnvifControl::Ptr &onvif, const SdCardSyncConfig &sdSyncConfig, const bool &ready = false);

    bool resetFailedSession(const std::string& device_id, const std::string& session_id);
    
    bool resetFailedSegment(const std::string& device_id, const std::string& session_id, const std::string& stream_id, int32_t segment_index);
    
    bool stopSession(const std::string& device_id);
    
    bool stopSegment(const std::string& device_id, const std::string& stream_id, int32_t segment_index);

    void onDeviceStateChanged(const std::string &device_id, const bool &connect);

    Json::Value makeRemainingSegmentsJson(const std::string& device_id);

    Json::Value makeDisconnectSessionJson(const std::string& device_id, const std::string& session_id);

    Json::Value makeDisconnectSessionsJson(const std::string& device_id);
private:
    SdCardSyncManager();

    void start(CameraContext& ctx, const std::string& stream_id, const std::string& url, const uint64_t& earliestRecord, const uint64_t& start, const uint64_t& end);

    void addCamera(const std::string& device_id, const std::string& username, const std::string& password);

    bool hasCamera(const std::string& device_id);

    void onManageCamera(CameraContext& ctx);

    void handleProcessingSession(CameraContext& ctx, DisconnectSession& session);

    void startPendingSession(CameraContext& ctx, DisconnectSession& session);

    bool tryMakeStreamInformation(const std::unordered_map<std::string, VideoEncoderConfig>& encoderConfigMap, const std::vector<RecordingInformation>& recordingInformations, std::vector<StreamInfo>& streams);

    void handleReplayInformation(const std::string device_id, const std::unordered_map<int, std::vector<VideoSegment>>& segments, const std::vector<RecordingInformation>& rec_infos);

    std::string buildReplayUrl(const std::string& username, const std::string& password, const std::string& replay_uri, uint64_t start_ms, uint64_t end_ms);

    void onSegmentShutdown(const std::string &proxyKey, const float &progress);

    bool onSegmentRetry(const std::string &proxyKey, const float &progress, std::string &newUrl);

    DisconnectSessionManager loadConnectionTrackerFromDb(const std::string& deviceId);

    void syncConnectionTrackerToDb(const std::string& deviceId, DisconnectSessionManager& tracker);
private:
    std::recursive_mutex _mtx;
    std::map<std::string, CameraContext> _camera_map;   // key = device_id
    int _retry_count = 0;
};

} // namespace managerkit

#endif // S3MANAGERKIT_SDCARDSYNCMANAGER_H