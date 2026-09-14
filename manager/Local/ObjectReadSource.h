#ifndef LOCAL_OBJECT_READ_SOURCE_H
#define LOCAL_OBJECT_READ_SOURCE_H

#include <cstdint>
#include <algorithm>
#include <limits>
#include <string>

namespace managerkit {

enum class ObjectReadSourceKind {
    LOCAL_PATH,
    PRESIGNED_URL,
    RESTORE_REQUIRED,
    UNAVAILABLE
};

enum class ObjectReadPurpose {
    THUMBNAIL,
    EXTRACT
};

enum class ObjectReadStrategy {
    LOCAL,
    PRESIGNED,
    RESTORE
};

struct ObjectReadRequest {
    std::string camera_id;
    std::string stream_id;
    std::string timefile_path;
    int64_t segment_start_time = 0;
    int64_t segment_size = 0;
    int64_t required_ttl_seconds = 0;
    ObjectReadPurpose purpose = ObjectReadPurpose::THUMBNAIL;
    bool retain_restore_cache = false;
    bool enqueue_restore = false;
    std::string restore_reason;
};

struct ObjectReadSource {
    ObjectReadSourceKind kind = ObjectReadSourceKind::UNAVAILABLE;
    std::string local_path;
    std::string url;
    std::string restore_job_id;
    std::string message;
    float progress_percent = 0.0f;
    bool storage_unavailable = false;
    bool restore_unavailable = false;
};

struct ObjectReadPlan {
    ObjectReadStrategy strategy = ObjectReadStrategy::LOCAL;
    int64_t total_bytes = 0;
    size_t segment_count = 0;
};

inline int64_t requiredObjectReadTtlSeconds(int64_t start_time, int64_t end_time,
                                            ObjectReadPurpose purpose) {
    const int64_t duration = std::max<int64_t>(0, end_time - start_time);
    if (purpose == ObjectReadPurpose::THUMBNAIL)
        return 10;
    const int64_t max_runtime = 12 * 3600;
    const int64_t estimated_runtime = duration > max_runtime / 10
        ? max_runtime : std::min<int64_t>(duration * 10, max_runtime);
    return estimated_runtime > std::numeric_limits<int64_t>::max() - 60
        ? std::numeric_limits<int64_t>::max() : estimated_runtime + 60;
}

} // namespace managerkit

#endif // LOCAL_OBJECT_READ_SOURCE_H
