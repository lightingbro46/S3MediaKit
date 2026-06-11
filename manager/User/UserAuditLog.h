#ifndef USER_AUDIT_LOG_H
#define USER_AUDIT_LOG_H

#include "Common/config.h"
#include "json/json.h"

namespace managerkit {

namespace SystemAuditLogType {
const std::string START = "MEDIA_SERVER_START";
const std::string SHUT_DOWN  = "MEDIA_SERVER_SHUTTING_DOWN";
const std::string RESTART_SCHEDULE = "MEDIA_SERVER_RESTART_SCHEDULE";
const std::string RESTART_CONFIG = "MEDIA_SERVER_RESTART_CONFIG";

} // namespace SystemAuditLogType


namespace UserAuditLogType {
const std::string LIVE_VIEW = "LIVE_VIEW";
const std::string PLAYBACK  = "PLAYBACK";
const std::string EXTRACT_VIDEO = "EXTRACT_VIDEO";

} // namespace UserAuditLogType

struct UserAuditLogArgs {
    std::string user_id;
    std::string user_name;
    uint64_t action_created_at = 0;
    uint64_t action_duration = 0;
    std::string message;

    Json::Value toJson() {
        Json::Value ret;
        ret["user_id"] = user_id;
        ret["user_name"] = user_name;
        ret["action_created_at"] = (Json::UInt64)action_created_at;
        ret["action_duration"] = (Json::UInt64)action_duration;
        ret["message"] = message;
        return ret;
    }
};

struct ExtractAuditLogArgs : public UserAuditLogArgs {
    std::string camera_id;
    std::string camera_name;
    std::string stream_id;
    std::string stream_profile;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint64_t duration = 0;
    uint64_t file_size = 0;
    std::string filename;
    bool is_success = false;

    Json::Value toJson() {
        Json::Value ret = UserAuditLogArgs::toJson();
        ret["camera_id"] = camera_id;
        ret["camera_name"] = camera_name;
        ret["stream_id"] = stream_id;
        ret["stream_profile"] = stream_profile;
        ret["start_time"] = (Json::UInt64)start_time;
        ret["end_time"] = (Json::UInt64)end_time;
        ret["duration"] = (Json::UInt64)duration;
        ret["file_size"] = (Json::UInt64)file_size;
        ret["filename"] = filename;
        ret["is_success"] = is_success;
        return ret;
    }
};

struct LiveViewAuditLogArgs : public UserAuditLogArgs {
    std::string camera_id;
    std::string camera_name;
    std::string stream_id;
    std::string stream_profile;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    uint64_t duration = 0;

    Json::Value toJson() {
        Json::Value ret = UserAuditLogArgs::toJson();
        ret["camera_id"] = camera_id;
        ret["camera_name"] = camera_name;
        ret["stream_id"] = stream_id;
        ret["stream_profile"] = stream_profile;
        ret["start_time"] = (Json::UInt64)start_time;
        ret["end_time"] = (Json::UInt64)end_time;
        ret["duration"] = (Json::UInt64)duration;
        return ret;
    }
};

} // namespace managerkit

#endif // USER_AUDIT_LOG_H