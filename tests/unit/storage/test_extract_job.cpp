#include <gtest/gtest.h>

#include "Storage/ExtractJob.h"

using namespace managerkit;

TEST(ExtractJobTest, ExposesPersistentSchema) {
    EXPECT_EQ("extract_jobs", EntityTraits<ExtractJob>::tableName());
    EXPECT_EQ(27U, EntityTraits<ExtractJob>::getColumns().size());
}

TEST(ExtractJobTest, SerializesSuccessCallbackContract) {
    ExtractJob job;
    job.file_id = "clip-1";
    job.status = "SUCCESS";
    job.size_bytes = 5242880;
    job.duration_seconds = 15;
    job.content_type = "video/mp4";
    job.completed_at = 1784253723;

    const Json::Value value = job.toCallbackJson();
    EXPECT_EQ("clip-1", value["fileId"].asString());
    EXPECT_EQ("SUCCESS", value["status"].asString());
    EXPECT_EQ(5242880, value["file"]["sizeBytes"].asInt64());
    EXPECT_EQ(15, value["file"]["durationSeconds"].asInt64());
    EXPECT_EQ("video/mp4", value["file"]["contentType"].asString());
    EXPECT_EQ(1784253723, value["completedAt"].asInt64());
    EXPECT_FALSE(value.isMember("error"));
}

TEST(ExtractJobTest, SerializesFailureCallbackContract) {
    ExtractJob job;
    job.file_id = "clip-2";
    job.status = "FAILED";
    job.error_code = "RECORDING_NOT_FOUND";
    job.error_message = "No recording covers the requested interval";
    job.completed_at = 1784253723;

    const Json::Value value = job.toCallbackJson();
    EXPECT_EQ("FAILED", value["status"].asString());
    EXPECT_EQ("RECORDING_NOT_FOUND", value["error"]["code"].asString());
    EXPECT_EQ("No recording covers the requested interval", value["error"]["message"].asString());
    EXPECT_FALSE(value.isMember("file"));
}

TEST(ExtractJobTest, SerializesListItemDetails) {
    ExtractJob job;
    job.file_id = "clip-list";
    job.camera_id = "cam-001";
    job.stream_id = "main";
    job.start_time = 1784253600;
    job.end_time = 1784253615;
    job.format = "mp4";
    job.status = "FAILED";
    job.error_code = "RECORDING_NOT_FOUND";
    job.error_message = "No recording covers the requested interval";
    job.created_at = 1784253601;
    job.updated_at = 1784253723;
    job.completed_at = 1784253723;

    const Json::Value value = job.toJson();
    EXPECT_EQ("clip-list", value["fileId"].asString());
    EXPECT_EQ("cam-001", value["cameraId"].asString());
    EXPECT_EQ(1784253600, value["startTime"].asInt64());
    EXPECT_EQ(1784253601, value["createdAt"].asInt64());
    EXPECT_EQ(1784253723, value["updatedAt"].asInt64());
    EXPECT_EQ("RECORDING_NOT_FOUND", value["error"]["code"].asString());
    EXPECT_FALSE(value.isMember("uploadUrl"));
}
