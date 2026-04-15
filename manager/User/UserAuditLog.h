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
    std::string camera_name;
    std::string stream_profile;
    uint64_t start_time;
    uint64_t end_time;
    uint64_t duration;
    uint64_t file_size;
    uint64_t action_created_at;
    uint64_t action_duration;
    std::string message;
};

inline Json::Value toJson(const UserAuditLogArgs &args) {
    Json::Value json;
    json["camera_name"] = args.camera_name;
    json["stream_profile"] = args.stream_profile;
    json["start_time"] = (Json::UInt64)args.start_time;
    json["end_time"] = (Json::UInt64)args.end_time;
    json["duration"] = (Json::UInt64)args.duration;
    json["file_size"] = (Json::UInt64)args.file_size;
    json["action_created_at"] = (Json::UInt64)args.action_created_at;
    json["action_duration"] = (Json::UInt64)args.action_duration;
    json["message"] = args.message;
    return json;
}

} // namespace managerkit

#endif // USER_AUDIT_LOG_H