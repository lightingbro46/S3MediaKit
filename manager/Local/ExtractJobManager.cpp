#include "ExtractJobManager.h"

#include <algorithm>
#include <cstdlib>
#include <strings.h>

#include "Common/config.h"
#include "Thread/WorkThreadPool.h"
#include "Util/File.h"
#include "Util/SHA1.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "server/WebApi.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace ExtractJobConfig {
#define EXTRACT_JOB_FIELD "extract_job."
const string kMaxConcurrent = EXTRACT_JOB_FIELD "max_concurrent";
const string kDispatchIntervalSec = EXTRACT_JOB_FIELD "dispatch_interval_sec";
const string kStartupDelaySec = EXTRACT_JOB_FIELD "startup_delay_sec";
const string kUploadTimeoutSec = EXTRACT_JOB_FIELD "upload_timeout_sec";
const string kUploadMaxAttempts = EXTRACT_JOB_FIELD "upload_max_attempts";
const string kCallbackMaxAttempts = EXTRACT_JOB_FIELD "callback_max_attempts";
const string kRetryDelaySec = EXTRACT_JOB_FIELD "retry_delay_sec";
const string kMaxDurationSec = EXTRACT_JOB_FIELD "max_duration_sec";
const string kAllowedUploadHosts = EXTRACT_JOB_FIELD "allowed_upload_hosts";

static onceToken token([]() {
    mINI::Instance()[kMaxConcurrent] = 1;
    mINI::Instance()[kDispatchIntervalSec] = 1.0f;
    mINI::Instance()[kStartupDelaySec] = 5;
    mINI::Instance()[kUploadTimeoutSec] = 300;
    mINI::Instance()[kUploadMaxAttempts] = 3;
    mINI::Instance()[kCallbackMaxAttempts] = 10;
    mINI::Instance()[kRetryDelaySec] = 5;
    mINI::Instance()[kMaxDurationSec] = 3600;
    mINI::Instance()[kAllowedUploadHosts] = "";
});
} // namespace ExtractJobConfig

namespace {

static string boundedError(const string &message) {
    const size_t max_size = 1024;
    return message.size() <= max_size ? message : message.substr(0, max_size);
}

static bool isHttpSuccess(int status_code) {
    return status_code >= 200 && status_code < 300;
}

static bool isRetryableHttpFailure(int status_code) {
    return status_code == 0 || status_code == 408 || status_code == 429 || status_code >= 500;
}

static string uploadHost(const string &url) {
    const string prefix = "https://";
    if (!start_with(url, prefix)) {
        return "";
    }
    const size_t authority_start = prefix.size();
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    string authority = url.substr(authority_start, authority_end == string::npos ? string::npos : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != string::npos) {
        return "";
    }
    if (authority[0] == '[') {
        const size_t close = authority.find(']');
        return close == string::npos ? "" : authority.substr(1, close - 1);
    }
    const size_t colon = authority.rfind(':');
    return colon == string::npos ? authority : authority.substr(0, colon);
}

} // namespace

INSTANCE_IMP(ExtractJobManager)

ExtractJobManager::ExtractJobManager(const EventPoller::Ptr &poller) {
    _poller = poller ? poller : WorkThreadPool::Instance().getPoller();
}

ExtractJobManager::~ExtractJobManager() {
    // Normal process shutdown calls stop() from unregisterExtractionApis().
    // Avoid shared_from_this() from the singleton destructor itself.
    _running = false;
    _timer.reset();
}

void ExtractJobManager::start() {
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true)) {
        return;
    }
    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    _poller->async([weak_self]() {
        auto self = weak_self.lock();
        if (self) {
            self->startOnPoller();
        }
    });
}

void ExtractJobManager::startOnPoller() {
    ExtractJobImp repository;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    repository.recoverInterrupted(now);

    GET_CONFIG(int, startup_delay_sec, ExtractJobConfig::kStartupDelaySec);
    GET_CONFIG(float, dispatch_interval_sec, ExtractJobConfig::kDispatchIntervalSec);
    _ready_at = now + std::max(startup_delay_sec, 0);

    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(std::max(dispatch_interval_sec, 0.1f), [weak_self]() {
        auto self = weak_self.lock();
        if (!self || !self->_running.load()) {
            return false;
        }
        self->dispatch();
        return true;
    }, _poller);
    InfoL << "ExtractJobManager started";
}

void ExtractJobManager::stop() {
    if (!_running.exchange(false)) {
        return;
    }
    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    _poller->async([weak_self]() {
        auto self = weak_self.lock();
        if (self) {
            self->stopOnPoller();
        }
    });
}

void ExtractJobManager::stopOnPoller() {
    _timer.reset();
    for (auto &entry : _active_processing) {
        if (entry.second->extractor) {
            entry.second->extractor->cancel();
        }
    }
    _active_processing.clear();
    _active_callbacks.clear();
    InfoL << "ExtractJobManager stopped";
}

ExtractJobSubmitResult ExtractJobManager::submit(const ExtractJobSubmitRequest &request) {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    ExtractJob job;
    job.file_id = request.file_id;
    job.request_hash = makeRequestHash(request);
    job.camera_id = request.camera_id;
    job.stream_id = request.stream_id;
    job.start_time = request.start_time;
    job.end_time = request.end_time;
    job.format = request.format;
    job.upload_url = request.upload_url;
    job.status = extractJobStatusToString(ExtractJobStatus::PENDING);
    job.callback_status = extractCallbackStatusToString(ExtractCallbackStatus::NONE);
    job.created_at = now;
    job.updated_at = now;

    ExtractJobSubmitResult result;
    ExtractJobImp repository;
    result.result = repository.createOrFind(job, result.job);
    if (result.result == ExtractJobRepository::CreateResult::CREATED) {
        wakeUp();
    }
    return result;
}

void ExtractJobManager::wakeUp() {
    if (!_running.load()) {
        return;
    }
    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    _poller->async([weak_self]() {
        auto self = weak_self.lock();
        if (self && self->_running.load()) {
            self->dispatch();
        }
    });
}

string ExtractJobManager::makeRequestHash(const ExtractJobSubmitRequest &request) {
    const string canonical = request.file_id + "\n" + request.camera_id + "\n" + request.stream_id + "\n" +
        to_string(request.start_time) + "\n" + to_string(request.end_time) + "\n" +
        request.format + "\n" + request.upload_url;
    return SHA1::encode(canonical);
}

string ExtractJobManager::contentTypeForFormat(const string &format) {
    if (format == "mp4") return "video/mp4";
    if (format == "mkv") return "video/x-matroska";
    if (format == "avi") return "video/x-msvideo";
    return "application/octet-stream";
}

string ExtractJobManager::redactUrl(const string &url) {
    const size_t query = url.find('?');
    return query == string::npos ? url : url.substr(0, query) + "?<redacted>";
}

bool ExtractJobManager::isAllowedUploadUrl(const string &url) {
    const string host = uploadHost(url);
    if (host.empty()) {
        return false;
    }
    GET_CONFIG(string, allowed_hosts, ExtractJobConfig::kAllowedUploadHosts);
    if (allowed_hosts.empty()) {
        return true;
    }
    for (auto allowed : split(allowed_hosts, ",")) {
        trim(allowed);
        if (!allowed.empty() && strcasecmp(allowed.c_str(), host.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

void ExtractJobManager::dispatch() {
    if (!_running.load()) {
        return;
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (now < _ready_at) {
        return;
    }

    GET_CONFIG(int, max_concurrent, ExtractJobConfig::kMaxConcurrent);
    const size_t capacity = static_cast<size_t>(std::max(max_concurrent, 1));
    ExtractJobImp repository;
    auto jobs = repository.findDueProcessing(now, static_cast<int>(capacity * 2));
    for (const auto &job : jobs) {
        if (_active_processing.size() >= capacity) {
            break;
        }
        if (_active_processing.count(job.file_id)) {
            continue;
        }
        if (job.status == extractJobStatusToString(ExtractJobStatus::PENDING)) {
            startExtract(job);
        } else if (job.status == extractJobStatusToString(ExtractJobStatus::UPLOADING)) {
            auto context = std::make_shared<ActiveProcessing>();
            context->job = job;
            _active_processing[job.file_id] = context;
            startUpload(context);
        }
    }

    auto callbacks = repository.findDueCallbacks(now, static_cast<int>(capacity * 2));
    for (const auto &job : callbacks) {
        if (_active_callbacks.size() >= capacity || _active_callbacks.count(job.file_id)) {
            continue;
        }
        startCallback(job);
    }
}

void ExtractJobManager::startExtract(const ExtractJob &job) {
    ExtractJobImp repository;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (!repository.claimExtract(job.file_id, job.extract_attempts, now)) {
        return;
    }

    auto context = std::make_shared<ActiveProcessing>();
    context->job = job;
    context->job.status = "EXTRACTING";
    context->job.extract_attempts++;

    MediaTuple tuple = {DEFAULT_VHOST, job.camera_id, job.stream_id, ""};
    ExtractOptions options = {static_cast<uint64_t>(job.start_time), static_cast<uint64_t>(job.end_time),
                              job.file_id + "." + job.format, "Service extraction job", "", "", "", false};
    context->extractor = std::make_shared<FFmpegExtractor>(tuple, options);
    context->extractor->setAutoCleanup(false);

    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    const string file_id = job.file_id;
    context->extractor->setOnComplete([weak_self, file_id](const FFmpegExtractor::Status &status,
                                                           const string &output_path,
                                                           uint64_t duration_seconds,
                                                           uint64_t size_bytes) {
        auto self = weak_self.lock();
        if (!self) return;
        self->_poller->async([weak_self, file_id, status, output_path, duration_seconds, size_bytes]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->onExtractCompleted(file_id, status, output_path, duration_seconds, size_bytes);
            }
        });
    });

    _active_processing[job.file_id] = context;
    GET_CONFIG(string, extract_path, API::kExtractRoot);
    const string internal_key = "job-" + SHA1::encode(job.file_id);
    context->extractor->makeExtract(internal_key, extract_path, [file_id](const SockException &ex) {
        if (ex) {
            WarnL << "Extract job failed to start, file_id=" << file_id << ", error=" << ex.what();
        }
    });
}

void ExtractJobManager::onExtractCompleted(const string &file_id,
                                            const FFmpegExtractor::Status &status,
                                            const string &output_path,
                                            uint64_t duration_seconds,
                                            uint64_t size_bytes) {
    auto it = _active_processing.find(file_id);
    if (it == _active_processing.end()) {
        return;
    }
    auto context = it->second;
    ExtractJobImp repository;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (!status.success) {
        const string code = status.err_msg.find("No data") != string::npos ? "RECORDING_NOT_FOUND" : "EXTRACT_FAILED";
        repository.markFailed(file_id, code, boundedError(status.err_msg), now);
        finishProcessing(file_id, true);
        wakeUp();
        return;
    }

    context->job.status = "UPLOADING";
    context->job.local_path = output_path;
    context->job.size_bytes = static_cast<int64_t>(size_bytes);
    context->job.duration_seconds = static_cast<int64_t>(duration_seconds);
    context->job.content_type = contentTypeForFormat(context->job.format);
    if (!repository.markUploading(file_id, output_path, context->job.size_bytes,
                                  context->job.duration_seconds, context->job.content_type, now)) {
        repository.markFailed(file_id, "INTERNAL_ERROR", "Failed to persist extracted file metadata", now);
        finishProcessing(file_id, true);
        wakeUp();
        return;
    }
    startUpload(context);
}

void ExtractJobManager::startUpload(const ActiveProcessing::Ptr &context) {
    ExtractJob &job = context->job;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (job.local_path.empty() || !File::fileExist(job.local_path)) {
        ExtractJobImp repository;
        repository.markFailed(job.file_id, "UPLOAD_FAILED", "Extracted file is missing", now);
        finishProcessing(job.file_id, true);
        wakeUp();
        return;
    }

    ExtractJobImp repository;
    if (!repository.claimUpload(job.file_id, job.upload_attempts, now)) {
        finishProcessing(job.file_id, false);
        return;
    }
    job.upload_attempts++;

    GET_CONFIG(float, upload_timeout_sec, ExtractJobConfig::kUploadTimeoutSec);
    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    const string file_id = job.file_id;
    Broadcast::VideoExtractionUploadInvoker on_result = [weak_self, file_id](const string &error, int status_code) {
        auto self = weak_self.lock();
        if (!self) return;
        self->_poller->async([weak_self, file_id, error, status_code]() {
            auto strong_self = weak_self.lock();
            if (strong_self) {
                strong_self->onUploadCompleted(file_id, error, status_code);
            }
        });
    };
    const float timeout_sec = std::max(upload_timeout_sec, 1.0f);
    const bool emitted = NOTICE_EMIT(BroadcastVideoExtractionUploadArgs,
                                     Broadcast::kBroadcastVideoExtractionUpload,
                                     job.local_path, job.upload_url, job.content_type,
                                     timeout_sec, on_result);
    if (!emitted) {
        on_result("No listener for video extraction upload hook", 0);
    }
}

void ExtractJobManager::onUploadCompleted(const string &file_id,
                                           const string &error,
                                           int status_code) {
    auto it = _active_processing.find(file_id);
    if (it == _active_processing.end()) {
        return;
    }
    auto context = it->second;
    ExtractJobImp repository;
    const int64_t now = static_cast<int64_t>(time(nullptr));

    if (error.empty() && isHttpSuccess(status_code)) {
        repository.markSuccess(file_id, now);
        InfoL << "Extract job uploaded, file_id=" << file_id;
        finishProcessing(file_id, true);
        wakeUp();
        return;
    }

    GET_CONFIG(int, upload_max_attempts, ExtractJobConfig::kUploadMaxAttempts);
    const string upload_error = boundedError(error.empty() ? "Upload returned HTTP " + to_string(status_code) : error);
    if (isRetryableHttpFailure(status_code) && context->job.upload_attempts < std::max(upload_max_attempts, 1)) {
        repository.scheduleUploadRetry(file_id, retryAt(context->job.upload_attempts), upload_error, now);
        WarnL << "Extract job upload will retry, file_id=" << file_id
              << ", target=" << redactUrl(context->job.upload_url)
              << ", error=" << upload_error;
        finishProcessing(file_id, false);
        return;
    }

    const string code = status_code == 401 || status_code == 403 ? "UPLOAD_URL_EXPIRED" : "UPLOAD_FAILED";
    repository.markFailed(file_id, code, upload_error, now);
    finishProcessing(file_id, true);
    wakeUp();
}

void ExtractJobManager::startCallback(const ExtractJob &job) {
    ExtractJobImp repository;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (!repository.claimCallback(job.file_id, job.callback_attempts, now)) {
        return;
    }

    weak_ptr<ExtractJobManager> weak_self = shared_from_this();
    const string file_id = job.file_id;
    _active_callbacks.insert(file_id);
    Broadcast::VideoExtractionResultInvoker on_result = [weak_self, file_id](const Json::Value &, const string &error) {
        auto self = weak_self.lock();
        if (!self || !self->_running.load()) return;
        self->_poller->async([weak_self, file_id, error]() {
            auto strong_self = weak_self.lock();
            if (strong_self && strong_self->_running.load()) {
                strong_self->onCallbackCompleted(file_id, error);
            }
        });
    };
    const Json::Value body = job.toCallbackJson();
    const bool emitted = NOTICE_EMIT(BroadcastVideoExtractionResultArgs,
                                     Broadcast::kBroadcastVideoExtractionResult,
                                     body, file_id, on_result);
    if (!emitted) {
        on_result(Json::nullValue, "No listener for video extraction result hook");
    }
}

void ExtractJobManager::onCallbackCompleted(const string &file_id,
                                             const string &error) {
    _active_callbacks.erase(file_id);
    ExtractJobImp repository;
    auto rows = repository.findByFileId(file_id);
    if (rows.empty()) {
        return;
    }
    const ExtractJob &job = rows.front();
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (error.empty()) {
        repository.markCallbackDelivered(file_id, now);
        return;
    }

    GET_CONFIG(int, callback_max_attempts, ExtractJobConfig::kCallbackMaxAttempts);
    const string bounded_error = boundedError(error);
    const bool dead = job.callback_attempts >= std::max(callback_max_attempts, 1);
    repository.scheduleCallbackRetry(file_id, dead ? 0 : retryAt(job.callback_attempts), bounded_error, dead, now);
    if (dead) {
        WarnL << "Extract result callback is dead, file_id=" << file_id << ", error=" << bounded_error;
    }
}

void ExtractJobManager::finishProcessing(const string &file_id, bool delete_output) {
    auto it = _active_processing.find(file_id);
    if (it == _active_processing.end()) {
        return;
    }
    auto context = it->second;
    if (delete_output) {
        if (context->extractor) {
            context->extractor->cleanup();
        } else if (!context->job.local_path.empty()) {
            File::delete_file(context->job.local_path);
        }
    } else if (context->extractor) {
        context->extractor->cleanupTemporaryFiles();
    }
    _active_processing.erase(it);
}

int64_t ExtractJobManager::retryAt(int attempt) const {
    GET_CONFIG(int, retry_delay_sec, ExtractJobConfig::kRetryDelaySec);
    const int shift = std::max(0, std::min(attempt - 1, 6));
    const int64_t delay = static_cast<int64_t>(std::max(retry_delay_sec, 1)) << shift;
    return static_cast<int64_t>(time(nullptr)) + delay;
}

} // namespace managerkit
