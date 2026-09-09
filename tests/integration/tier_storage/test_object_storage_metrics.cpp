#include <gtest/gtest.h>

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>

#include <chrono>
#include <string>
#include <vector>

#include "Local/ObjectStorageMetricsProvider.h"

using namespace managerkit;

namespace {

using JwtTraits = jwt::traits::open_source_parsers_jsoncpp;

ObjectStorageMetricsContext makeContext() {
    ObjectStorageMetricsContext context;
    context.type = "MINIO";
    context.endpoint = "http://minio.test:9000/";
    context.bucket = "recordings";
    context.access_key = "metrics-access";
    context.secret_key = "metrics-secret";
    return context;
}

ObjectStorageMetricsHttpResponse response(long status, const std::string &body) {
    ObjectStorageMetricsHttpResponse value;
    value.transport_ok = true;
    value.status_code = status;
    value.body = body;
    return value;
}

} // namespace

TEST(MinioPrometheusMetricsProviderTest, CreatesHs512TokenFromPoolCredentials) {
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());
    const std::string token = MinioPrometheusMetricsProvider::createPrometheusToken(
        "metrics-access", "metrics-secret", now);

    const auto decoded = jwt::decode<JwtTraits>(token);
    EXPECT_EQ("HS512", decoded.get_algorithm());
    EXPECT_EQ("prometheus", decoded.get_issuer());
    EXPECT_EQ("metrics-access", decoded.get_subject());
    EXPECT_EQ(now + std::chrono::minutes(15), decoded.get_expires_at());
    EXPECT_NO_THROW(jwt::verify<JwtTraits>()
        .allow_algorithm(jwt::algorithm::hs512("metrics-secret"))
        .with_issuer("prometheus")
        .verify(decoded));
}

TEST(MinioPrometheusMetricsProviderTest, ParsesAuthenticatedCapacityAndTrafficMetrics) {
    bool saw_authorization = false;
    ObjectStorageMetricsHttpExecutor executor = [&saw_authorization](
            const std::string &url,
            const std::vector<std::string> &headers,
            long) {
        for (const auto &header : headers) {
            if (header.compare(0, 22, "Authorization: Bearer ") == 0 && header.size() > 22)
                saw_authorization = true;
        }
        if (url.find("/cluster") != std::string::npos) {
            return response(200,
                "# HELP minio capacity\n"
                "minio_cluster_capacity_raw_total_bytes{server=\"node-1\"} 1000\n"
                "minio_cluster_capacity_raw_free_bytes 400\n");
        }
        return response(200,
            "minio_bucket_traffic_received_bytes{bucket=\"recordings\"} 11\n"
            "minio_bucket_traffic_sent_bytes{bucket=\"recordings\"} 22\n");
    };

    MinioPrometheusMetricsProvider provider(executor);
    const ObjectStorageMetricsResult result = provider.collect(makeContext());

    EXPECT_TRUE(saw_authorization);
    EXPECT_TRUE(result.capacity_available);
    EXPECT_TRUE(result.traffic_available);
    EXPECT_EQ("AVAILABLE", result.capacity_status);
    EXPECT_EQ(1000U, result.total_bytes);
    EXPECT_EQ(600U, result.used_bytes);
    EXPECT_EQ(400U, result.free_bytes);
    EXPECT_EQ(11U, result.rx_bytes);
    EXPECT_EQ(22U, result.tx_bytes);
}

TEST(MinioPrometheusMetricsProviderTest, ReportsAuthorizationFailureWithoutValidZeroCapacity) {
    ObjectStorageMetricsHttpExecutor executor = [](
            const std::string &,
            const std::vector<std::string> &,
            long) {
        return response(403, "");
    };

    MinioPrometheusMetricsProvider provider(executor);
    const ObjectStorageMetricsResult result = provider.collect(makeContext());

    EXPECT_FALSE(result.capacity_available);
    EXPECT_EQ("UNAVAILABLE", result.capacity_status);
    EXPECT_NE(std::string::npos, result.capacity_message.find("HTTP 403"));
    EXPECT_EQ(0U, result.total_bytes);
}

TEST(MinioPrometheusMetricsProviderTest, RejectsMissingOrInvalidCapacityMetrics) {
    ObjectStorageMetricsHttpExecutor executor = [](
            const std::string &,
            const std::vector<std::string> &,
            long) {
        return response(200,
            "minio_cluster_capacity_raw_total_bytes 1000\n"
            "minio_cluster_capacity_raw_free_bytes 1200\n");
    };

    MinioPrometheusMetricsProvider provider(executor);
    const ObjectStorageMetricsResult result = provider.collect(makeContext());

    EXPECT_FALSE(result.capacity_available);
    EXPECT_NE(std::string::npos, result.capacity_message.find("missing or invalid"));
}

TEST(MinioPrometheusMetricsProviderTest, KeepsCapacityWhenOptionalTrafficRequestFails) {
    ObjectStorageMetricsHttpExecutor executor = [](
            const std::string &url,
            const std::vector<std::string> &,
            long) {
        if (url.find("/cluster") != std::string::npos) {
            return response(200,
                "minio_cluster_capacity_raw_total_bytes 2000\n"
                "minio_cluster_capacity_raw_free_bytes 500\n");
        }
        return response(403, "");
    };

    MinioPrometheusMetricsProvider provider(executor);
    const ObjectStorageMetricsResult result = provider.collect(makeContext());

    EXPECT_TRUE(result.capacity_available);
    EXPECT_FALSE(result.traffic_available);
    EXPECT_EQ(1500U, result.used_bytes);
}
