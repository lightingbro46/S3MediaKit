#include "TierObjectStorage.h"

#include "Util/logger.h"
#include "Util/File.h"
#include "Util/util.h"
#include "Util/TimeTicker.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef ENABLE_AWS_SDK

#include <aws/core/utils/logging/DefaultLogSystem.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CompletedPart.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <curl/curl.h>
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
    Aws::Utils::Logging::InitializeAWSLogging(nullptr); // silent
    Aws::InitAPI(_sdk_options);
    _sdk_initialized = true;
    DebugL << "TierObjectStorage: AWS SDK initialised";
#endif
}

TierObjectStorage::~TierObjectStorage() {
#ifdef ENABLE_AWS_SDK
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _pool_map.clear();
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
                                const std::string &secret_key) const {
    Aws::Auth::AWSCredentials creds{Aws::String(access_key), Aws::String(secret_key)};

    Aws::Client::ClientConfiguration cc;
    cc.endpointOverride = Aws::String(endpoint);
    cc.scheme           = Aws::Http::Scheme::HTTP;
    cc.verifySSL        = false;
    cc.connectTimeoutMs = 5000;
    cc.requestTimeoutMs = 60000;
    cc.maxConnections   = 8;
    cc.retryStrategy    = Aws::MakeShared<Aws::Client::StandardRetryStrategy>(
                              "TierObjStorage", 3L);

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

// ---------------------------------------------------------------------------
// Prometheus scraper for MinIO capacity metrics
// ---------------------------------------------------------------------------
static size_t curlWrite(char *ptr, size_t sz, size_t nmemb, void *ud) {
    auto *buf = static_cast<std::string *>(ud);
    buf->append(ptr, sz * nmemb);
    return sz * nmemb;
}

static std::string httpGet(const std::string &url, int timeout_s = 3) {
    CURL *c = curl_easy_init();
    if (!c) return {};
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       static_cast<long>(timeout_s));
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return body;
}

static double firstMetric(const std::string &body, const std::string &name) {
    std::istringstream ss(body);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind(name, 0) != 0) continue;
        auto pos = line.rfind('}');
        try {
            return std::stod(pos != std::string::npos
                             ? line.substr(pos + 1)
                             : line.substr(name.size() + 1));
        } catch (...) {}
    }
    return 0.0;
}

TierPoolStats TierObjectStorage::scrapeMinioPrometheus(const PoolEntry &entry) const {
    TierPoolStats stats;
    std::string base = entry.endpoint;
    if (!base.empty() && base.back() == '/') base.pop_back();

    // Cluster capacity
    auto cluster = httpGet(base + "/minio/v2/metrics/cluster");
    if (!cluster.empty()) {
        double total = firstMetric(cluster, "minio_cluster_capacity_raw_total_bytes");
        double free  = firstMetric(cluster, "minio_cluster_capacity_raw_free_bytes");
        stats.total_bytes = static_cast<uint64_t>(total);
        stats.free_bytes  = static_cast<uint64_t>(free);
        stats.used_bytes  = total > free ? static_cast<uint64_t>(total - free) : 0;
        stats.is_online   = (total > 0);
    }

    // Traffic counters
    auto bucket = httpGet(base + "/minio/v2/metrics/bucket");
    if (!bucket.empty()) {
        stats.rx_bytes = static_cast<uint64_t>(
            firstMetric(bucket, "minio_bucket_traffic_received_bytes"));
        stats.tx_bytes = static_cast<uint64_t>(
            firstMetric(bucket, "minio_bucket_traffic_sent_bytes"));
    }
    return stats;
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
    entry.endpoint  = endpoint;
    entry.bucket    = bucket;
    entry.base_path = base_path;
    entry.access_key = access_key;
    entry.client    = buildClient(endpoint, access_key, secret_key);

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
bool TierObjectStorage::testConnection(const std::string &pool_id,
                                        std::string &out_message, int &out_latency_ms) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _pool_map.find(pool_id);
    if (it == _pool_map.end()) {
        out_message = "Pool not registered: " + pool_id;
        return false;
    }

    const auto &entry = it->second;
    Ticker ticker;
    Aws::S3::Model::HeadBucketRequest req;
    req.SetBucket(entry.bucket.c_str());
    auto out = entry.client->HeadBucket(req);
    out_latency_ms = ticker.elapsedTime();
    if (out.IsSuccess()) {
        out_message = "Connection successful";
        return true;
    }
    out_message = std::string("HeadBucket failed: ") + out.GetError().GetMessage().c_str();
    return false;
}

bool TierObjectStorage::testConnectionParams(const std::string &endpoint,
                                              const std::string &bucket,
                                              const std::string &access_key,
                                              const std::string &secret_key,
                                              std::string &out_message,
                                              int &out_latency_ms) {
    auto client = buildClient(endpoint, access_key, secret_key);
    Ticker ticker;
    Aws::S3::Model::HeadBucketRequest req;
    req.SetBucket(bucket.c_str());
    auto out = client->HeadBucket(req);
    out_latency_ms = ticker.elapsedTime();
    if (out.IsSuccess()) {
        out_message = "Connection successful";
        return true;
    }
    // Bucket might not exist yet — try listing buckets as a connectivity probe
    auto list_out = client->ListBuckets(Aws::S3::Model::ListBucketsRequest{});
    if (list_out.IsSuccess()) {
        out_message = "Connection successful (bucket not found, will be created on first use)";
        return true;
    }
    out_message = std::string("Connection failed: ") + out.GetError().GetMessage().c_str();
    return false;
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

    Aws::Vector<Aws::S3::Model::CompletedPart> completedParts;
    std::ifstream ifs(local_path, std::ios::binary);
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
            Aws::S3::Model::AbortMultipartUploadRequest abort;
            abort.SetBucket(bucket);
            abort.SetKey(key);
            abort.SetUploadId(uploadId);
            entry.client->AbortMultipartUpload(abort);
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

// ============================================================================
// Capacity / health stats
// ============================================================================
TierPoolStats TierObjectStorage::getPoolStats(const std::string &pool_id) {
    PoolEntry entry;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _pool_map.find(pool_id);
        if (it == _pool_map.end()) return {};
        entry = it->second;
    }

    // MinIO exposes Prometheus at /minio/v2/metrics/cluster
    // Generic S3 (AWS) does not — fall back to HeadBucket connectivity check
    TierPoolStats stats = scrapeMinioPrometheus(entry);
    if (!stats.is_online) {
        // Try HeadBucket as a connectivity probe
        Aws::S3::Model::HeadBucketRequest req;
        req.SetBucket(entry.bucket.c_str());
        stats.is_online = entry.client->HeadBucket(req).IsSuccess();
    }
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
    out_msg = "AWS SDK not enabled";
    return false;
}

bool TierObjectStorage::testConnectionParams(const std::string &, const std::string &,
                                              const std::string &, const std::string &,
                                              std::string &out_msg, int &out_latency_ms) {
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
TierPoolStats TierObjectStorage::getPoolStats(const std::string &) { return {}; }

#endif // ENABLE_AWS_SDK

} // namespace managerkit
