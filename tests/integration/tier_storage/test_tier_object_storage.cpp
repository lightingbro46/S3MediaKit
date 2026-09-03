#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

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

    ASSERT_TRUE(storage.uploadSegment(pool_id, source_path, object_key));
    ASSERT_TRUE(storage.segmentExists(pool_id, object_key));
    ASSERT_TRUE(storage.downloadSegment(pool_id, object_key, download_path));
    EXPECT_EQ(payload, readFile(download_path));

    ASSERT_TRUE(storage.deleteSegment(pool_id, object_key));
    cleanup.object_key.clear();
    EXPECT_FALSE(storage.segmentExists(pool_id, object_key));
}
