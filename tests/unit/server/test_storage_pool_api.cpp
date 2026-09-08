#include <gtest/gtest.h>

#include "Local/TierStorageBase.h"
#include "WebApi.h"
#include "WebApiErrCode.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

TEST(WebApiResponseTest, AcceptsLocalVariableNamedMessage) {
    Json::Value val;
    HttpSession::KeyValue headerOut;
    int response_status = 0;
    std::string response_body;
    HttpSession::HttpResponseInvoker invoker(
        [&](int status, const StrCaseMap &, const std::string &body) {
            response_status = status;
            response_body = body;
        });

    std::string message = "HeadBucket failed";
    RETURN_API_RESPONSE(ApiErrCode::CODE_STORAGE_POOL_CONN_FAILED, message);

    Json::Value response;
    Json::Reader reader;
    ASSERT_TRUE(reader.parse(response_body, response));
    EXPECT_EQ(404, response_status);
    EXPECT_EQ(914005, response["code"].asInt());
    EXPECT_EQ(message, response["msg"].asString());
}

#ifdef ENABLE_TIER_STORAGE
TEST(StoragePoolApiTest, MergesChangedConnectionFieldsAndPreservesMaskedCredentials) {
    StoragePool pool;
    pool.id = "pool-1";
    pool.name = "Old pool";
    pool.type = "MINIO";
    pool.tier = "COLD";
    pool.endpoint = Optional<std::string>("http://old:9000");
    pool.bucket = Optional<std::string>("old-bucket");
    pool.base_path = Optional<std::string>("old-path");
    pool.access_key = Optional<std::string>("old-access");
    pool.secret_key_enc = Optional<std::string>("old-secret");

    Json::Value body;
    body["id"] = pool.id;
    body["endpoint"] = "http://new:9000";
    body["bucket"] = "new-bucket";
    body["base_path"] = "new-path";
    body["access_key"] = "***********";
    body["secret_key"] = "";

    TierStorageBase::applyPoolPatch(pool, body);

    EXPECT_EQ("pool-1", pool.id);
    EXPECT_EQ("http://new:9000", pool.endpoint.value());
    EXPECT_EQ("new-bucket", pool.bucket.value());
    EXPECT_EQ("new-path", pool.base_path.value());
    EXPECT_EQ("old-access", pool.access_key.value());
    EXPECT_EQ("old-secret", pool.secret_key_enc.value());
}

TEST(StoragePoolApiTest, ReplacesCredentialsWhenNewValuesAreProvided) {
    StoragePool pool;
    pool.access_key = Optional<std::string>("old-access");
    pool.secret_key_enc = Optional<std::string>("old-secret");

    Json::Value body;
    body["access_key"] = "new-access";
    body["secret_key"] = "new-secret";
    TierStorageBase::applyPoolPatch(pool, body);

    EXPECT_EQ("new-access", pool.access_key.value());
    EXPECT_EQ("new-secret", pool.secret_key_enc.value());
}

TEST(StoragePoolApiTest, ParsesAdHocObjectPoolAndValidatesRequiredFields) {
    Json::Value body;
    body["name"] = "Test pool";
    body["type"] = "MINIO";
    body["tier"] = "COLD";
    body["endpoint"] = "http://minio:9000";
    body["bucket"] = "recordings";
    body["base_path"] = "archive";
    body["access_key"] = "access";
    body["secret_key"] = "secret";

    StoragePool pool = TierStorageBase::poolFromJson(body);
    std::string error;
    EXPECT_TRUE(TierStorageBase::validatePoolByType(pool, error)) << error;

    body.removeMember("bucket");
    pool = TierStorageBase::poolFromJson(body);
    EXPECT_FALSE(TierStorageBase::validatePoolByType(pool, error));
}
#endif
