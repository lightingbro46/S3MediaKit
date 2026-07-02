#ifndef LOCAL_STORAGETIER_H
#define LOCAL_STORAGETIER_H

#include <string>
#include <vector>
#include <json/json.h>

namespace managerkit {

// ===================================================================
// Tier type
// ===================================================================
typedef enum {
    HotTier  = 0,
    WarmTier = 1,
    ColdTier = 2,
    TierMax
} TierType;

inline std::string tierTypeToString(TierType t) {
    switch (t) {
        case HotTier:  return "HOT";
        case WarmTier: return "WARM";
        case ColdTier: return "COLD";
        default:       return "UNKNOWN";
    }
}

inline TierType tierTypeFromString(const std::string &s) {
    if (s == "HOT")  return HotTier;
    if (s == "WARM") return WarmTier;
    if (s == "COLD") return ColdTier;
    return TierMax;
}

// ===================================================================
// Pool type
// ===================================================================
enum class StoragePoolType {
    LOCAL_DISK,
    NAS,
    MINIO,
    S3,
    ARCHIVE
};

inline std::string poolTypeToString(StoragePoolType t) {
    switch (t) {
        case StoragePoolType::LOCAL_DISK: return "LOCAL_DISK";
        case StoragePoolType::NAS:        return "NAS";
        case StoragePoolType::MINIO:      return "MINIO";
        case StoragePoolType::S3:         return "S3";
        case StoragePoolType::ARCHIVE:    return "ARCHIVE";
        default:                          return "UNKNOWN";
    }
}

inline StoragePoolType poolTypeFromString(const std::string &s) {
    if (s == "NAS")     return StoragePoolType::NAS;
    if (s == "MINIO")   return StoragePoolType::MINIO;
    if (s == "S3")      return StoragePoolType::S3;
    if (s == "ARCHIVE") return StoragePoolType::ARCHIVE;
    return StoragePoolType::LOCAL_DISK;
}

inline bool poolTypeIsObjectStorage(const std::string &type) {
    return (type == "MINIO" || type == "S3" || type == "ARCHIVE");
}

inline Json::Value poolTypeSupport(TierType t) {
    Json::Value v;
    if (t == HotTier) {
        v["LOCAL_DISK"] = true;
        v["NAS"]        = true;
        v["MINIO"]      = false;
        v["S3"]         = false;
        v["ARCHIVE"]    = false;
    } else if (t == WarmTier) {
        v["LOCAL_DISK"] = true;
        v["NAS"]        = true;
        v["MINIO"]      = false;
        v["S3"]         = false;
        v["ARCHIVE"]    = false;
    } else if (t == ColdTier) {
        v["LOCAL_DISK"] = false;
        v["NAS"]        = true;
        v["MINIO"]      = true;
        v["S3"]         = false;
        v["ARCHIVE"]    = false;
    }
    return v;
}

// ===================================================================
// Pool / tier health
// ===================================================================
enum class StorageHealth {
    OK,
    WARNING,
    HIGH,
    CRITICAL,
    OFFLINE
};

inline std::string healthToString(StorageHealth h) {
    switch (h) {
        case StorageHealth::OK:       return "OK";
        case StorageHealth::WARNING:  return "WARNING";
        case StorageHealth::HIGH:     return "HIGH";
        case StorageHealth::CRITICAL: return "CRITICAL";
        case StorageHealth::OFFLINE:  return "OFFLINE";
        default:                      return "OK";
    }
}

// ===================================================================
// Tier data mode
// ===================================================================
enum class TierDataMode {
    FULL_VIDEO,
    EVENT_VIDEO_ONLY,
    SNAPSHOT_ONLY,
    METADATA_ONLY,
    MOTION_INDEX_ONLY
};

inline std::string tierDataModeToString(TierDataMode m) {
    switch (m) {
        case TierDataMode::EVENT_VIDEO_ONLY:  return "EVENT_VIDEO_ONLY";
        case TierDataMode::SNAPSHOT_ONLY:     return "SNAPSHOT_ONLY";
        case TierDataMode::METADATA_ONLY:     return "METADATA_ONLY";
        case TierDataMode::MOTION_INDEX_ONLY: return "MOTION_INDEX_ONLY";
        default:                              return "FULL_VIDEO";
    }
}

// ===================================================================
// Overflow / delete actions
// ===================================================================
enum class OverflowAction {
    MOVE_TO_NEXT_TIER,
    DELETE_OLDEST,
    STOP_RECORDING_AND_ALERT
};

inline std::string overflowActionToString(OverflowAction a) {
    switch (a) {
        case OverflowAction::DELETE_OLDEST:            return "DELETE_OLDEST";
        case OverflowAction::STOP_RECORDING_AND_ALERT: return "STOP_RECORDING_AND_ALERT";
        default:                                       return "MOVE_TO_NEXT_TIER";
    }
}

enum class DeleteMode {
    DELETE_AUTOMATICALLY,
    MARK_EXPIRED_WAIT_APPROVAL,
    MOVE_TO_EXTERNAL_STORAGE
};

inline std::string deleteModeToString(DeleteMode m) {
    switch (m) {
        case DeleteMode::MARK_EXPIRED_WAIT_APPROVAL: return "MARK_EXPIRED_WAIT_APPROVAL";
        case DeleteMode::MOVE_TO_EXTERNAL_STORAGE:   return "MOVE_TO_EXTERNAL_STORAGE";
        default:                                     return "DELETE_AUTOMATICALLY";
    }
}

// ===================================================================
// Job status
// ===================================================================
enum class JobStatus {
    PENDING,
    RUNNING,
    DONE,
    FAILED,
    CANCELLED
};

inline std::string jobStatusToString(JobStatus s) {
    switch (s) {
        case JobStatus::RUNNING:   return "RUNNING";
        case JobStatus::DONE:      return "DONE";
        case JobStatus::FAILED:    return "FAILED";
        case JobStatus::CANCELLED: return "CANCELLED";
        default:                   return "PENDING";
    }
}

inline JobStatus jobStatusFromString(const std::string &s) {
    if (s == "RUNNING")   return JobStatus::RUNNING;
    if (s == "DONE")      return JobStatus::DONE;
    if (s == "FAILED")    return JobStatus::FAILED;
    if (s == "CANCELLED") return JobStatus::CANCELLED;
    return JobStatus::PENDING;
}

// ===================================================================
// Policy source (effective policy resolution)
// ===================================================================
enum class PolicySource {
    CAMERA,
    GROUP,
    PROJECT,
    SYSTEM_DEFAULT
};

inline std::string policySourceToString(PolicySource s) {
    switch (s) {
        case PolicySource::CAMERA:  return "CAMERA";
        case PolicySource::GROUP:   return "GROUP";
        case PolicySource::PROJECT: return "PROJECT";
        default:                    return "SYSTEM_DEFAULT";
    }
}

// ===================================================================
// Segment status on timeline
// ===================================================================
enum class SegmentStatus {
    AVAILABLE,
    RESTORING,
    EXPIRED,
    DELETED,
    MISSING
};

inline std::string segmentStatusToString(SegmentStatus s) {
    switch (s) {
        case SegmentStatus::RESTORING: return "RESTORING";
        case SegmentStatus::EXPIRED:   return "EXPIRED";
        case SegmentStatus::DELETED:   return "DELETED";
        case SegmentStatus::MISSING:   return "MISSING";
        default:                       return "AVAILABLE";
    }
}

// ===================================================================
// Structured sub-objects for policy
// ===================================================================
struct PolicyTierConfig {
    std::string tier;                                   // HOT | WARM | COLD
    bool        enabled                  = false;
    std::string pool_id;
    int         retain_until_days        = 0;
    std::string data_mode                = "FULL_VIDEO";
    std::string overflow_action          = "MOVE_TO_NEXT_TIER";
    int         high_watermark_percent   = 80;
    int         critical_watermark_percent = 90;
    std::string priority                 = "NORMAL";   // HIGH | NORMAL | LOW

    Json::Value toJson() const {
        Json::Value v;
        v["tier"]                        = tier;
        v["enabled"]                     = enabled;
        v["pool_id"]                     = pool_id;
        v["retain_until_days"]           = retain_until_days;
        v["data_mode"]                   = data_mode;
        v["overflow_action"]             = overflow_action;
        v["high_watermark_percent"]      = high_watermark_percent;
        v["critical_watermark_percent"]  = critical_watermark_percent;
        v["priority"]                    = priority;
        return v;
    }

    static PolicyTierConfig fromJson(const Json::Value &v) {
        PolicyTierConfig c;
        c.tier                       = v.get("tier", "HOT").asString();
        c.enabled                    = v.get("enabled", false).asBool();
        c.pool_id                    = v.get("pool_id", "").asString();
        c.retain_until_days          = v.get("retain_until_days", 0).asInt();
        c.data_mode                  = v.get("data_mode", "FULL_VIDEO").asString();
        c.overflow_action            = v.get("overflow_action", "MOVE_TO_NEXT_TIER").asString();
        c.high_watermark_percent     = v.get("high_watermark_percent", 80).asInt();
        c.critical_watermark_percent = v.get("critical_watermark_percent", 90).asInt();
        c.priority                   = v.get("priority", "NORMAL").asString();
        return c;
    }
};

struct PolicyDeleteConfig {
    int         delete_after_days                  = 0;
    std::string delete_mode                        = "DELETE_AUTOMATICALLY";
    bool        skip_protected_video               = true;
    bool        skip_evidence_video                = true;
    bool        require_approval_before_delete     = false;
    std::string external_pool_id;

    Json::Value toJson() const {
        Json::Value v;
        v["delete_after_days"]              = delete_after_days;
        v["delete_mode"]                    = delete_mode;
        v["skip_protected_video"]           = skip_protected_video;
        v["skip_evidence_video"]            = skip_evidence_video;
        v["require_approval_before_delete"] = require_approval_before_delete;
        return v;
    }

    static PolicyDeleteConfig fromJson(const Json::Value &v) {
        PolicyDeleteConfig c;
        c.delete_after_days              = v.get("delete_after_days", 0).asInt();
        c.delete_mode                    = v.get("delete_mode", "DELETE_AUTOMATICALLY").asString();
        c.skip_protected_video           = v.get("skip_protected_video", true).asBool();
        c.skip_evidence_video            = v.get("skip_evidence_video", true).asBool();
        c.require_approval_before_delete = v.get("require_approval_before_delete", false).asBool();
        c.external_pool_id               = v.get("external_pool_id", "").asString();
        return c;
    }
};

struct PolicyAdvancedRules {
    bool enable_early_move_when_pool_high       = true;
    bool prefer_move_no_event_video_first       = true;
    bool prefer_keep_event_video_longer         = true;
    bool skip_move_if_pool_offline              = true;
    bool alert_when_pool_critical               = true;
    int  min_segment_age_minutes_before_move    = 30;

    Json::Value toJson() const {
        Json::Value v;
        v["enable_early_move_when_pool_high"]     = enable_early_move_when_pool_high;
        v["prefer_move_no_event_video_first"]     = prefer_move_no_event_video_first;
        v["prefer_keep_event_video_longer"]       = prefer_keep_event_video_longer;
        v["min_segment_age_minutes_before_move"]  = min_segment_age_minutes_before_move;
        return v;
    }

    static PolicyAdvancedRules fromJson(const Json::Value &v) {
        PolicyAdvancedRules r;
        r.enable_early_move_when_pool_high    = v.get("enable_early_move_when_pool_high", true).asBool();
        r.prefer_move_no_event_video_first    = v.get("prefer_move_no_event_video_first", true).asBool();
        r.prefer_keep_event_video_longer      = v.get("prefer_keep_event_video_longer", true).asBool();
        r.skip_move_if_pool_offline           = v.get("skip_move_if_pool_offline", true).asBool();
        r.alert_when_pool_critical            = v.get("alert_when_pool_critical", true).asBool();
        r.min_segment_age_minutes_before_move = v.get("min_segment_age_minutes_before_move", 30).asInt();
        return r;
    }
};

inline PolicyTierConfig getSystemDefaultPolicyTierConfig() {
    PolicyTierConfig c;
    c.tier = tierTypeToString(HotTier);
    c.enabled = true;
    c.pool_id = "";
    c.retain_until_days = 30;
    c.data_mode = tierDataModeToString(TierDataMode::FULL_VIDEO);
    c.overflow_action = overflowActionToString(OverflowAction::DELETE_OLDEST);
    c.high_watermark_percent = 80;
    c.critical_watermark_percent = 90;
    c.priority = "NORMAL";
    return c;
}

inline PolicyDeleteConfig getSystemDefaultPolicyDeleteConfig() {
    PolicyDeleteConfig c;
    c.delete_after_days = 0;
    c.delete_mode = "DELETE_AUTOMATICALLY";
    c.skip_protected_video = true;
    c.skip_evidence_video = true;
    c.require_approval_before_delete = false;
    c.external_pool_id = "";
    return c;
}

inline PolicyAdvancedRules getSystemDefaultPolicyAdvancedRules() {
    PolicyAdvancedRules r;
    r.enable_early_move_when_pool_high = true;
    r.prefer_move_no_event_video_first = true;
    r.prefer_keep_event_video_longer = true;
    r.skip_move_if_pool_offline = true;
    r.alert_when_pool_critical = true;
    r.min_segment_age_minutes_before_move = 30;
    return r;
}

// ===================================================================
// Runtime result types used across the manager APIs
// ===================================================================
struct PoolHealthInfo {
    std::string pool_id;
    std::string health_status;   // OK | WARNING | HIGH | CRITICAL | OFFLINE
    int64_t     used_bytes  = 0;
    int64_t     total_bytes = 0;
    float       usage_pct   = 0.0f;
    int64_t     timestamp   = 0;
};

struct EffectivePolicyResult {
    std::string camera_id;
    std::string policy_id;
    std::string policy_name;
    std::string source;          // CAMERA | GROUP | PROJECT | SYSTEM_DEFAULT
    bool        allow_camera_override = true;
};

struct TierSummaryInfo {
    std::string tier;            // HOT | WARM | COLD
    std::string pool_id;
    int64_t     used_bytes   = 0;
    int64_t     total_bytes  = 0;
    float       usage_pct    = 0.0f;
    int64_t     segment_count = 0;
    int64_t     oldest_segment_time = 0;
    int64_t     newest_segment_time = 0;
};

struct CameraStorageSummary {
    std::string               camera_id;
    int64_t                   total_used_bytes = 0;
    int64_t                   total_segments   = 0;
    int64_t                   oldest_time      = 0;
    int64_t                   newest_time      = 0;
    std::vector<TierSummaryInfo> tiers;
};

struct TimelineRange {
    int64_t     start   = 0;
    int64_t     end     = 0;
    std::string tier;           // HOT | WARM | COLD
    std::string status;         // AVAILABLE | RESTORING | EXPIRED | DELETED | MISSING
    bool        has_event = false;
};

} // namespace managerkit

#endif // LOCAL_STORAGETIER_H
