#include "TierObjectStorage.h"
#include "ObjectStorageMetricsProvider.h"

#include "Util/logger.h"
#include "Util/File.h"
#include "Util/util.h"
#include "Util/TimeTicker.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef ENABLE_AWS_SDK

#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CompletedPart.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <sys/stat.h>

#endif // ENABLE_AWS_SDK

using namespace std;
using namespace toolkit;

namespace managerkit {

// ============================================================================
// Multipart threshold: 64 MiB — objects smaller than this use PutObject
// ============================================================================
static constexpr size_t MULTIPART_THRESHOLD = 64ULL * 1024 * 1024;
static constexpr size_t MULTIPART_PART_SIZE = 32ULL * 1024 * 1024;

#ifdef ENABLE_AWS_SDK
static std::string parentDir(const std::string &path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos)
        return "";
    return path.substr(0, pos);
}

static bool ensureDirectory(const std::string &dir) {
    if (dir.empty())
        return true;
    std::string mkdir_path = dir;
    if (mkdir_path.back() != '/')
        mkdir_path += '/';
    if (!File::create_path(mkdir_path, 0755)) {
        WarnL << "TierObjectStorage: create_path failed " << dir;
        return false;
    }
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        WarnL << "TierObjectStorage: stat directory failed " << dir
              << ": " << strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        WarnL << "TierObjectStorage: path is not a directory " << dir;
        return false;
    }
    return true;
}
#endif

// ============================================================================
// Constructor / Destructor
// ============================================================================
TierObjectStorage::TierObjectStorage() {
#ifdef ENABLE_AWS_SDK
    _sdk_options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Off;
    Aws::InitAPI(_sdk_options);
    _sdk_initialized = true;
    _metrics_providers["MINIO"] = std::make_shared<MinioPrometheusMetricsProvider>();
    DebugL << "TierObjectStorage: AWS SDK initialised";
#endif
}

TierObjectStorage::~TierObjectStorage() {
#ifdef ENABLE_AWS_SDK
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map.clear();
        _metrics_providers.clear();
    }
    if (_sdk_initialized) {
        Aws::ShutdownAPI(_sdk_options);
        _sdk_initialized = false;
        DebugL << "TierObjectStorage: AWS SDK shut down";
    }
#endif
}

// ============================================================================
// S3 key utility
// ============================================================================
std::string TierObjectStorage::makeS3Key(const std::string &base_path,
                                          const std::string &camera_id,
                                          const std::string &stream_id,
                                          const std::string &segment_path) {
    // segment_path already contains date/time sub-path (e.g. "2024-01-15/10-00-00")
    std::string key;
    if (!base_path.empty()) {
        key = base_path;
        if (key.back() != '/') key += '/';
    }
    key += camera_id + '/' + stream_id + '/' + segment_path + ".mp4";
    return key;
}

// ============================================================================
// ============================================================================
#ifdef ENABLE_AWS_SDK
// ============================================================================

// ---------------------------------------------------------------------------
// buildClient — create a configured Aws::S3::S3Client
// ---------------------------------------------------------------------------
std::shared_ptr<Aws::S3::S3Client>
TierObjectStorage::buildClient(const std::string &endpoint,
                                const std::string &access_key,
                                const std::string &secret_key,
                                long request_timeout_ms,
                                long max_retries) const {
    Aws::Auth::AWSCredentials creds{Aws::String(access_key), Aws::String(secret_key)};

    Aws::Client::ClientConfiguration cc;
    cc.endpointOverride = Aws::String(endpoint);
    cc.scheme           = endpoint.compare(0, 8, "https://") == 0
                            ? Aws::Http::Scheme::HTTPS
                            : Aws::Http::Scheme::HTTP;
    cc.verifySSL        = false;
    cc.connectTimeoutMs = 5000;
    cc.requestTimeoutMs = request_timeout_ms;
    cc.maxConnections   = 8;
    cc.retryStrategy    = Aws::MakeShared<Aws::Client::StandardRetryStrategy>(
                              "TierObjStorage", max_retries);

    // MinIO / Ceph require path-style: bucket in URL path, not virtual host
    return Aws::MakeShared<Aws::S3::S3Client>(
        "TierObjStorage",
        creds,
        cc,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAddressing=*/false);
}

// ---------------------------------------------------------------------------
// ensureBucketExists
// ---------------------------------------------------------------------------
bool TierObjectStorage::ensureBucketExists(const PoolEntry &entry) {
    Aws::S3::Model::HeadBucketRequest head;
    head.SetBucket(entry.bucket.c_str());
    if (entry.client->HeadBucket(head).IsSuccess())
        return true;

    Aws::S3::Model::CreateBucketRequest create;
    create.SetBucket(entry.bucket.c_str());
    auto out = entry.client->CreateBucket(create);
    if (out.IsSuccess()) {
        InfoL << "TierObjectStorage: created bucket '" << entry.bucket << "' on pool " << entry.pool_id;
        return true;
    }
    WarnL << "TierObjectStorage: cannot create bucket '" << entry.bucket << "': " << out.GetError().GetMessage();
    return false;
}

// ============================================================================
// Pool lifecycle
// ============================================================================
bool TierObjectStorage::registerPool(const std::string &pool_id,
                                      const std::string &type,
                                      const std::string &endpoint,
                                      const std::string &bucket,
                                      const std::string &base_path,
                                      const std::string &access_key,
                                      const std::string &secret_key) {
    if (endpoint.empty() || bucket.empty() || access_key.empty() || secret_key.empty()) {
        WarnL << "TierObjectStorage::registerPool: missing credentials for pool " << pool_id;
        return false;
    }

    PoolEntry entry;
    entry.pool_id   = pool_id;
    entry.type      = type;
    entry.endpoint  = endpoint;
    entry.bucket    = bucket;
    entry.base_path = base_path;
    entry.access_key = access_key;
    entry.client    = buildClient(endpoint, access_key, secret_key);
    entry.health_client = buildClient(endpoint, access_key, secret_key, 3000, 0);

    bool ok = ensureBucketExists(entry);

    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map[pool_id]        = std::move(entry);
        _pool_secret_map[pool_id] = secret_key;
    }

    if (ok)
        InfoL << "TierObjectStorage: registered pool " << pool_id << " -> " << endpoint << "/" << bucket;
    else
        WarnL << "TierObjectStorage: registered pool " << pool_id << " but bucket not ready";
    return ok;
}

void TierObjectStorage::unregisterPool(const std::string &pool_id) {
    std::lock_guard<std::mutex> lk(_mtx);
    _pool_map.erase(pool_id);
    _pool_secret_map.erase(pool_id);
}

bool TierObjectStorage::isRegistered(const std::string &pool_id) const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _pool_map.count(pool_id) > 0;
}

// ============================================================================
// Connection tests
// ============================================================================
#ifdef ENABLE_AWS_SDK
static bool probeObjectStorageConnection(const std::shared_ptr<Aws::S3::S3Client> &client,
                                         const std::string &bucket,
                                         std::string &out_message,
                                         int &out_latency_ms) {
    out_latency_ms = 0;
    Ticker ticker;
    Aws::S3::Model::HeadBucketRequest request;
    request.SetBucket(bucket.c_str());
    auto outcome = client->HeadBucket(request);
    out_latency_ms = ticker.elapsedTime();
    if (outcome.IsSuccess()) {
        out_message = "Connection successful";
        return true;
    }

    auto list_outcome = client->ListBuckets(Aws::S3::Model::ListBucketsRequest{});
    out_latency_ms = ticker.elapsedTime();
    if (list_outcome.IsSuccess()) {
        out_message = "Connection successful (bucket not found or inaccessible)";
        return true;
    }
    out_message = std::string("Connection failed: ") + outcome.GetError().GetMessage().c_str();
    return false;
}
#endif

bool TierObjectStorage::testConnection(const std::string &pool_id,
                                        std::string &out_message, int &out_latency_ms) {
    out_latency_ms = 0;
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) {
            out_message = "Pool not registered: " + pool_id;
            return false;
        }
        entry = it->second;
    }
    return probeObjectStorageConnection(entry.client, entry.bucket, out_message, out_latency_ms);
}

bool TierObjectStorage::testConnectionParams(const std::string &endpoint,
                                              const std::string &bucket,
                                              const std::string &access_key,
                                              const std::string &secret_key,
                                              std::string &out_message,
                                              int &out_latency_ms) {
    out_latency_ms = 0;
    auto client = buildClient(endpoint, access_key, secret_key);
    return probeObjectStorageConnection(client, bucket, out_message, out_latency_ms);
}

// ============================================================================
// Segment I/O
// ============================================================================
bool TierObjectStorage::uploadSegment(const std::string &pool_id,
                                       const std::string &local_path,
                                       const std::string &s3_key) {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) {
            WarnL << "TierObjectStorage::uploadSegment: pool not found " << pool_id;
            return false;
        }
        entry = it->second;
    }

    struct stat st{};
    if (stat(local_path.c_str(), &st) != 0) {
        WarnL << "TierObjectStorage::uploadSegment: file not found " << local_path;
        return false;
    }
    size_t file_size = static_cast<size_t>(st.st_size);

    Aws::String bucket(entry.bucket.c_str());
    Aws::String key(s3_key.c_str());

    if (file_size < MULTIPART_THRESHOLD) {
        // ── Single PutObject ──────────────────────────────────────────
        auto stream = Aws::MakeShared<Aws::FStream>(
            "TierUpload", local_path.c_str(),
            std::ios_base::in | std::ios_base::binary);

        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        req.SetBody(stream);
        req.SetContentLength(static_cast<long long>(file_size));

        auto out = entry.client->PutObject(req);
        if (!out.IsSuccess()) {
            WarnL << "TierObjectStorage: PutObject failed (" << s3_key << "): "
                  << out.GetError().GetMessage();
            return false;
        }
        return true;
    }

    // ── Multipart upload for large files ─────────────────────────────
    Aws::S3::Model::CreateMultipartUploadRequest createReq;
    createReq.SetBucket(bucket);
    createReq.SetKey(key);
    auto createOut = entry.client->CreateMultipartUpload(createReq);
    if (!createOut.IsSuccess()) {
        WarnL << "TierObjectStorage: CreateMultipartUpload failed: "
              << createOut.GetError().GetMessage();
        return false;
    }
    const Aws::String uploadId = createOut.GetResult().GetUploadId();
    auto abortUpload = [&]() {
        Aws::S3::Model::AbortMultipartUploadRequest abort;
        abort.SetBucket(bucket);
        abort.SetKey(key);
        abort.SetUploadId(uploadId);
        entry.client->AbortMultipartUpload(abort);
    };

    Aws::Vector<Aws::S3::Model::CompletedPart> completedParts;
    std::ifstream ifs(local_path, std::ios::binary);
    if (!ifs.is_open()) {
        abortUpload();
        WarnL << "TierObjectStorage: cannot read " << local_path;
        return false;
    }
    int partNum = 1;
    size_t offset = 0;

    while (offset < file_size) {
        size_t remaining = file_size - offset;
        size_t partSize  = (remaining > MULTIPART_PART_SIZE) ? MULTIPART_PART_SIZE : remaining;
        // Make sure we don't create a tiny last part (<5 MiB is rejected by S3)
        if (remaining - partSize < (5ULL * 1024 * 1024) && remaining > partSize)
            partSize = remaining;

        std::string chunk(partSize, '\0');
        ifs.read(&chunk[0], static_cast<std::streamsize>(partSize));
        if (ifs.gcount() != static_cast<std::streamsize>(partSize)) {
            abortUpload();
            WarnL << "TierObjectStorage: short read while uploading " << local_path;
            return false;
        }

        auto partStream = Aws::MakeShared<Aws::StringStream>(
            "TierPart", std::move(chunk));

        Aws::S3::Model::UploadPartRequest partReq;
        partReq.SetBucket(bucket);
        partReq.SetKey(key);
        partReq.SetUploadId(uploadId);
        partReq.SetPartNumber(partNum);
        partReq.SetContentLength(static_cast<long long>(partSize));
        partReq.SetBody(partStream);

        auto partOut = entry.client->UploadPart(partReq);
        if (!partOut.IsSuccess()) {
            abortUpload();
            WarnL << "TierObjectStorage: UploadPart " << partNum << " failed: "
                  << partOut.GetError().GetMessage();
            return false;
        }

        Aws::S3::Model::CompletedPart cp;
        cp.SetPartNumber(partNum);
        cp.SetETag(partOut.GetResult().GetETag());
        completedParts.push_back(std::move(cp));

        offset += partSize;
        ++partNum;
    }

    Aws::S3::Model::CompletedMultipartUpload completed;
    completed.SetParts(completedParts);

    Aws::S3::Model::CompleteMultipartUploadRequest completeReq;
    completeReq.SetBucket(bucket);
    completeReq.SetKey(key);
    completeReq.SetUploadId(uploadId);
    completeReq.SetMultipartUpload(completed);

    auto completeOut = entry.client->CompleteMultipartUpload(completeReq);
    if (!completeOut.IsSuccess()) {
        abortUpload();
        WarnL << "TierObjectStorage: CompleteMultipartUpload failed: "
              << completeOut.GetError().GetMessage();
        return false;
    }
    return true;
}

bool TierObjectStorage::downloadSegment(const std::string &pool_id,
                                         const std::string &s3_key,
                                         const std::string &local_path) {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) {
            WarnL << "TierObjectStorage::downloadSegment: pool not found " << pool_id;
            return false;
        }
        entry = it->second;
    }

    Aws::S3::Model::GetObjectRequest req;
    req.SetBucket(entry.bucket.c_str());
    req.SetKey(s3_key.c_str());

    auto out = entry.client->GetObject(req);
    if (!out.IsSuccess()) {
        WarnL << "TierObjectStorage::downloadSegment failed (" << s3_key << "): "
              << out.GetError().GetMessage();
        return false;
    }

    if (!ensureDirectory(parentDir(local_path)))
        return false;

    std::ofstream ofs(local_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        WarnL << "TierObjectStorage: cannot write " << local_path;
        return false;
    }
    ofs << out.GetResult().GetBody().rdbuf();
    if (!ofs.good()) {
        ofs.close();
        std::remove(local_path.c_str());
        WarnL << "TierObjectStorage: failed writing " << local_path;
        return false;
    }
    return true;
}

bool TierObjectStorage::deleteSegment(const std::string &pool_id,
                                       const std::string &s3_key) {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) return false;
        entry = it->second;
    }

    Aws::S3::Model::DeleteObjectRequest req;
    req.SetBucket(entry.bucket.c_str());
    req.SetKey(s3_key.c_str());

    auto out = entry.client->DeleteObject(req);
    if (!out.IsSuccess()) {
        WarnL << "TierObjectStorage::deleteSegment failed (" << s3_key << "): "
              << out.GetError().GetMessage();
        return false;
    }
    return true;
}

bool TierObjectStorage::deleteSegmentsBatch(const std::string &pool_id,
                                              const std::vector<std::string> &s3_keys) {
    if (s3_keys.empty()) return true;

    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) return false;
        entry = it->second;
    }

    constexpr size_t BATCH = 1000;
    bool all_ok = true;

    for (size_t i = 0; i < s3_keys.size(); i += BATCH) {
        Aws::S3::Model::Delete del;
        Aws::Vector<Aws::S3::Model::ObjectIdentifier> identifiers;

        size_t end = std::min(i + BATCH, s3_keys.size());
        for (size_t j = i; j < end; ++j) {
            Aws::S3::Model::ObjectIdentifier oi;
            oi.SetKey(Aws::String(s3_keys[j].c_str()));
            identifiers.push_back(oi);
        }
        del.SetObjects(identifiers);

        Aws::S3::Model::DeleteObjectsRequest req;
        req.SetBucket(entry.bucket.c_str());
        req.SetDelete(del);

        auto out = entry.client->DeleteObjects(req);
        if (!out.IsSuccess()) {
            WarnL << "TierObjectStorage::deleteSegmentsBatch: "
                  << out.GetError().GetMessage();
            all_ok = false;
        } else if (!out.GetResult().GetErrors().empty()) {
            WarnL << "TierObjectStorage::deleteSegmentsBatch: "
                  << out.GetResult().GetErrors().size() << " errors";
            all_ok = false;
        }
    }
    return all_ok;
}

bool TierObjectStorage::segmentExists(const std::string &pool_id,
                                       const std::string &s3_key) {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) return false;
        entry = it->second;
    }

    Aws::S3::Model::HeadObjectRequest req;
    req.SetBucket(entry.bucket.c_str());
    req.SetKey(s3_key.c_str());
    return entry.client->HeadObject(req).IsSuccess();
}

bool TierObjectStorage::supportsPresignedRead(const std::string &pool_id) const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _pool_map.count(pool_id) > 0;
}

bool TierObjectStorage::createPresignedGetUrl(const std::string &pool_id,
                                               const std::string &s3_key,
                                               uint32_t ttl_seconds,
                                               std::string &out_url,
                                               std::string &out_error) const {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) {
            out_error = "Object storage pool is not registered";
            return false;
        }
        entry = it->second;
    }
    const uint32_t bounded_ttl = std::max<uint32_t>(1, std::min<uint32_t>(ttl_seconds, 7U * 24U * 3600U));
    const Aws::String value = entry.client->GeneratePresignedUrl(
        entry.bucket.c_str(), s3_key.c_str(), Aws::Http::HttpMethod::HTTP_GET, bounded_ttl);
    if (value.empty()) {
        out_error = "Failed to create a presigned object read URL";
        return false;
    }
    out_url = value.c_str();
    out_error.clear();
    return true;
}

// ============================================================================
// Capacity / health stats
// ============================================================================
TierPoolStats TierObjectStorage::getPoolStats(const std::string &pool_id) {
    PoolEntry entry;
    std::string secret_key;
    std::shared_ptr<ObjectStorageMetricsProvider> metrics_provider;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) return {};
        entry = it->second;
        auto secret_it = _pool_secret_map.find(pool_id);
        if (secret_it != _pool_secret_map.end())
            secret_key = secret_it->second;
        auto provider_it = _metrics_providers.find(entry.type);
        if (provider_it != _metrics_providers.end())
            metrics_provider = provider_it->second;
    }

    TierPoolStats stats;
    Aws::S3::Model::HeadBucketRequest request;
    request.SetBucket(entry.bucket.c_str());
    auto client = entry.health_client ? entry.health_client : entry.client;
    stats.is_online = client->HeadBucket(request).IsSuccess();
    if (!stats.is_online)
        return stats;

    if (!metrics_provider) {
        stats.capacity_status = "UNSUPPORTED";
        stats.capacity_message = "Capacity metrics are not supported for object storage type " +
                                 entry.type;
        return stats;
    }

    ObjectStorageMetricsContext context;
    context.type = entry.type;
    context.endpoint = entry.endpoint;
    context.bucket = entry.bucket;
    context.access_key = entry.access_key;
    context.secret_key = secret_key;
    const ObjectStorageMetricsResult metrics = metrics_provider->collect(context);
    stats.capacity_available = metrics.capacity_available;
    stats.traffic_available = metrics.traffic_available;
    stats.total_bytes = metrics.total_bytes;
    stats.used_bytes = metrics.used_bytes;
    stats.free_bytes = metrics.free_bytes;
    stats.rx_bytes = metrics.rx_bytes;
    stats.tx_bytes = metrics.tx_bytes;
    stats.capacity_status = metrics.capacity_status;
    stats.capacity_message = metrics.capacity_message;
    return stats;
}

// ============================================================================
#else // !ENABLE_AWS_SDK — stub implementations
// ============================================================================

bool TierObjectStorage::registerPool(const std::string &pool_id,
                                      const std::string &, const std::string &,
                                      const std::string &, const std::string &,
                                      const std::string &, const std::string &) {
    WarnL << "TierObjectStorage: ENABLE_AWS_SDK not set; pool " << pool_id << " not registered";
    return false;
}

void TierObjectStorage::unregisterPool(const std::string &) {}
bool TierObjectStorage::isRegistered(const std::string &) const { return false; }

bool TierObjectStorage::testConnection(const std::string &, std::string &out_msg, int &out_latency_ms) {
    out_latency_ms = 0;
    out_msg = "AWS SDK not enabled";
    return false;
}

bool TierObjectStorage::testConnectionParams(const std::string &, const std::string &,
                                              const std::string &, const std::string &,
                                              std::string &out_msg, int &out_latency_ms) {
    out_latency_ms = 0;
    out_msg = "AWS SDK not enabled";
    return false;
}

bool TierObjectStorage::uploadSegment(const std::string &, const std::string &,
                                       const std::string &) { return false; }
bool TierObjectStorage::downloadSegment(const std::string &, const std::string &,
                                         const std::string &) { return false; }
bool TierObjectStorage::deleteSegment(const std::string &, const std::string &) { return false; }
bool TierObjectStorage::deleteSegmentsBatch(const std::string &,
                                              const std::vector<std::string> &) { return false; }
bool TierObjectStorage::segmentExists(const std::string &, const std::string &) { return false; }
bool TierObjectStorage::supportsPresignedRead(const std::string &) const { return false; }
bool TierObjectStorage::createPresignedGetUrl(const std::string &, const std::string &,
                                               uint32_t, std::string &,
                                               std::string &out_error) const {
    out_error = "Presigned object reads require ENABLE_AWS_SDK";
    return false;
}
TierPoolStats TierObjectStorage::getPoolStats(const std::string &) { return {}; }

#endif // ENABLE_AWS_SDK

} // namespace managerkit
