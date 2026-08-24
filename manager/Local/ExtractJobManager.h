#ifndef S3MEDIASERVER_EXTRACT_JOB_MANAGER_H
#define S3MEDIASERVER_EXTRACT_JOB_MANAGER_H

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Storage/ExtractJob.h"
#include "server/FFmpegSource.h"

namespace managerkit {

namespace ExtractJobConfig {
extern const std::string kMaxConcurrent;
extern const std::string kDispatchIntervalSec;
extern const std::string kStartupDelaySec;
extern const std::string kUploadTimeoutSec;
extern const std::string kUploadMaxAttempts;
extern const std::string kCallbackMaxAttempts;
extern const std::string kRetryDelaySec;
extern const std::string kMaxDurationSec;
extern const std::string kAllowedUploadHosts;
} // namespace ExtractJobConfig

struct ExtractJobSubmitRequest {
    std::string file_id;
    std::string camera_id;
    std::string stream_id;
    int64_t start_time = 0;
    int64_t end_time = 0;
    std::string upload_url;
    std::string format;
};

struct ExtractJobSubmitResult {
    ExtractJobRepository::CreateResult result = ExtractJobRepository::CreateResult::FAILED;
    ExtractJob job;
};

class ExtractJobManager : public std::enable_shared_from_this<ExtractJobManager> {
public:
    static ExtractJobManager &Instance();
    ~ExtractJobManager();

    void start();
    void stop();
    ExtractJobSubmitResult submit(const ExtractJobSubmitRequest &request);
    void wakeUp();

    static std::string makeRequestHash(const ExtractJobSubmitRequest &request);
    static std::string contentTypeForFormat(const std::string &format);
    static std::string redactUrl(const std::string &url);
    static bool isAllowedUploadUrl(const std::string &url);

private:
    struct ActiveProcessing {
        using Ptr = std::shared_ptr<ActiveProcessing>;
        ExtractJob job;
        FFmpegExtractor::Ptr extractor;
    };

    ExtractJobManager(const toolkit::EventPoller::Ptr &poller = nullptr);

    void startOnPoller();
    void stopOnPoller();
    void dispatch();
    void startExtract(const ExtractJob &job);
    void onExtractCompleted(const std::string &file_id,
                            const FFmpegExtractor::Status &status,
                            const std::string &output_path,
                            uint64_t duration_seconds,
                            uint64_t size_bytes);
    void startUpload(const ActiveProcessing::Ptr &context);
    void onUploadCompleted(const std::string &file_id,
                           const std::string &error,
                           int status_code);
    void startCallback(const ExtractJob &job);
    void onCallbackCompleted(const std::string &file_id,
                             const std::string &error);
    void finishProcessing(const std::string &file_id, bool delete_output);
    int64_t retryAt(int attempt) const;

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    std::atomic<bool> _running{false};
    int64_t _ready_at = 0;
    std::unordered_map<std::string, ActiveProcessing::Ptr> _active_processing;
    std::unordered_set<std::string> _active_callbacks;
};

} // namespace managerkit

#endif // S3MEDIASERVER_EXTRACT_JOB_MANAGER_H
