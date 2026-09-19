#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <curl/curl.h>

#include "Local/TierObjectStorage.h"

using namespace managerkit;

namespace {

const char *requiredEnv(const char *name) {
    const char *value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

std::string readFile(const std::string &path) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

size_t appendHttpBody(char *data, size_t size, size_t count, void *userdata) {
    const size_t bytes = size * count;
    static_cast<std::string *>(userdata)->append(data, bytes);
    return bytes;
}

bool getPresignedObject(const std::string &url, std::string &body, long &status_code) {
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendHttpBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    curl_easy_cleanup(curl);
    return result == CURLE_OK;
}

class ObjectCleanup {
public:
    TierObjectStorage *storage = nullptr;
    std::string pool_id;
    std::string object_key;
    std::string source_path;
    std::string download_path;
    std::string temp_dir;

    ~ObjectCleanup() {
        if (storage && !pool_id.empty() && !object_key.empty())
            storage->deleteSegment(pool_id, object_key);
        if (!source_path.empty())
            std::remove(source_path.c_str());
        if (!download_path.empty())
            std::remove(download_path.c_str());
        if (!temp_dir.empty())
            ::rmdir(temp_dir.c_str());
    }
};

} // namespace

TEST(TierObjectStorageIntegrationTest, UploadsDownloadsAndDeletesExactSegment) {
    const char *endpoint = requiredEnv("TIER_TEST_S3_ENDPOINT");
    const char *access_key = requiredEnv("TIER_TEST_S3_ACCESS_KEY");
    const char *secret_key = requiredEnv("TIER_TEST_S3_SECRET_KEY");
    if (!endpoint || !access_key || !secret_key) {
        GTEST_SKIP() << "Set TIER_TEST_S3_ENDPOINT, TIER_TEST_S3_ACCESS_KEY and "
                        "TIER_TEST_S3_SECRET_KEY to run the MinIO integration test";
    }

    const char *bucket_env = requiredEnv("TIER_TEST_S3_BUCKET");
    const std::string bucket = bucket_env ? bucket_env : "s3mediakit-tier-test";
    const std::string pool_id = "integration-minio";
    const std::string camera_id = "agent-camera";
    const std::string stream_id = "main";
    const std::string unique_id = std::to_string(static_cast<long long>(::getpid()));
    const std::string base_path = "agent-runs/" + unique_id;
    const std::string segment_path = "2026-08-27/segment-" + unique_id;
    const std::string object_key = TierObjectStorage::makeS3Key(
        base_path, camera_id, stream_id, segment_path);

    char temp_template[] = "/tmp/s3mediakit-tier-minio-XXXXXX";
    char *temp_dir_raw = ::mkdtemp(temp_template);
    ASSERT_NE(nullptr, temp_dir_raw);
    const std::string temp_dir(temp_dir_raw);
    const std::string source_path = temp_dir + "/source.mp4";
    const std::string download_path = temp_dir + "/download.mp4";
    const std::string payload = "S3MediaKit tier storage integration payload " + unique_id;
    {
        std::ofstream output(source_path.c_str(), std::ios::out | std::ios::binary);
        ASSERT_TRUE(output.good());
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ASSERT_TRUE(output.good());
    }

    TierObjectStorage storage;
    ObjectCleanup cleanup;
    cleanup.storage = &storage;
    cleanup.pool_id = pool_id;
    cleanup.object_key = object_key;
    cleanup.source_path = source_path;
    cleanup.download_path = download_path;
    cleanup.temp_dir = temp_dir;

    ASSERT_TRUE(storage.registerPool(
        pool_id, "MINIO", endpoint, bucket, base_path, access_key, secret_key));
    ASSERT_TRUE(storage.isRegistered(pool_id));

    const TierPoolStats stats = storage.getPoolStats(pool_id);
    ASSERT_TRUE(stats.is_online);
    ASSERT_TRUE(stats.capacity_available) << stats.capacity_message;
    EXPECT_EQ("AVAILABLE", stats.capacity_status);
    EXPECT_GT(stats.total_bytes, 0U);
    EXPECT_LE(stats.used_bytes, stats.total_bytes);

    ASSERT_TRUE(storage.uploadSegment(pool_id, source_path, object_key));
    ASSERT_TRUE(storage.segmentExists(pool_id, object_key));
    ASSERT_TRUE(storage.supportsPresignedRead(pool_id));
    std::string presigned_url;
    std::string presign_error;
    ASSERT_TRUE(storage.createPresignedGetUrl(pool_id, object_key, 60,
                                              presigned_url, presign_error))
        << presign_error;
    EXPECT_FALSE(presigned_url.empty());
    EXPECT_NE(std::string::npos, presigned_url.find(object_key));
    std::string presigned_body;
    long presigned_status = 0;
    ASSERT_TRUE(getPresignedObject(presigned_url, presigned_body, presigned_status));
    EXPECT_EQ(200, presigned_status);
    EXPECT_EQ(payload, presigned_body);
    ASSERT_TRUE(storage.downloadSegment(pool_id, object_key, download_path));
    EXPECT_EQ(payload, readFile(download_path));

    ASSERT_TRUE(storage.deleteSegment(pool_id, object_key));
    cleanup.object_key.clear();
    EXPECT_FALSE(storage.segmentExists(pool_id, object_key));
}

TEST(TierObjectStorageIntegrationTest, ListsRemainingObjectsAfterOneIsDeleted) {
    const char *endpoint = requiredEnv("TIER_TEST_S3_ENDPOINT");
    const char *access_key = requiredEnv("TIER_TEST_S3_ACCESS_KEY");
    const char *secret_key = requiredEnv("TIER_TEST_S3_SECRET_KEY");
    if (!endpoint || !access_key || !secret_key) {
        GTEST_SKIP() << "Set TIER_TEST_S3_ENDPOINT, TIER_TEST_S3_ACCESS_KEY and "
                        "TIER_TEST_S3_SECRET_KEY to run the MinIO integration test";
    }

    const char *bucket_env = requiredEnv("TIER_TEST_S3_BUCKET");
    const std::string bucket = bucket_env ? bucket_env : "s3mediakit-tier-test";
    const std::string pool_id = "integration-minio-list";
    const std::string unique_id = std::to_string(static_cast<long long>(::getpid()));
    const std::string base_path = "agent-runs/" + unique_id + "-list";
    const std::string day_prefix = base_path + "/agent-camera/main/2026-08-27/";

    char temp_template[] = "/tmp/s3mediakit-tier-minio-XXXXXX";
    char *temp_dir_raw = ::mkdtemp(temp_template);
    ASSERT_NE(nullptr, temp_dir_raw);
    const std::string temp_dir(temp_dir_raw);
    const std::string source_path = temp_dir + "/source.mp4";
    {
        std::ofstream output(source_path.c_str(), std::ios::out | std::ios::binary);
        output << "list payload";
        ASSERT_TRUE(output.good());
    }

    TierObjectStorage storage;
    ASSERT_TRUE(storage.registerPool(
        pool_id, "MINIO", endpoint, bucket, base_path, access_key, secret_key));
    std::vector<ObjectCleanup> cleanups(3);
    std::vector<std::string> keys;
    for (int i = 0; i < 3; ++i) {
        keys.push_back(TierObjectStorage::makeS3Key(base_path, "agent-camera", "main",
                                                    "2026-08-27/10-0" + std::to_string(i) + "-00"));
        cleanups[i].storage = &storage;
        cleanups[i].pool_id = pool_id;
        cleanups[i].object_key = keys.back();
        ASSERT_TRUE(storage.uploadSegment(pool_id, source_path, keys.back()));
    }
    cleanups.back().source_path = source_path;
    cleanups.back().temp_dir = temp_dir;

    ASSERT_TRUE(storage.deleteSegment(pool_id, keys[1]));
    cleanups[1].object_key.clear();

    std::map<std::string, TierObjectInfo> objects;
    std::string error;
    ASSERT_TRUE(storage.listObjects(pool_id, day_prefix, objects, error)) << error;
    ASSERT_EQ(2U, objects.size());
    EXPECT_EQ(1U, objects.count(keys[0]));
    EXPECT_EQ(1U, objects.count(keys[2]));
    EXPECT_EQ(static_cast<int64_t>(std::string("list payload").size()), objects[keys[0]].size);
    EXPECT_GT(objects[keys[0]].last_modified, 0);
    EXPECT_EQ(TierObjectPresence::ABSENT, storage.headSegment(pool_id, keys[1]));
    EXPECT_EQ(TierObjectPresence::PRESENT, storage.headSegment(pool_id, keys[0]));

    std::vector<std::string> prefixes;
    ASSERT_TRUE(storage.listCommonPrefixes(pool_id, base_path + "/agent-camera/", prefixes, error))
        << error;
    ASSERT_EQ(1U, prefixes.size());
    EXPECT_EQ(base_path + "/agent-camera/main/", prefixes.front());
}
