#ifndef LOCAL_OBJECT_STORAGE_METRICS_PROVIDER_H
#define LOCAL_OBJECT_STORAGE_METRICS_PROVIDER_H

#ifdef ENABLE_AWS_SDK

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace managerkit {

struct ObjectStorageMetricsContext {
    std::string type;
    std::string endpoint;
    std::string bucket;
    std::string access_key;
    std::string secret_key;
};

struct ObjectStorageMetricsResult {
    bool        capacity_available = false;
    bool        traffic_available  = false;
    uint64_t    total_bytes        = 0;
    uint64_t    used_bytes         = 0;
    uint64_t    free_bytes         = 0;
    uint64_t    rx_bytes           = 0;
    uint64_t    tx_bytes           = 0;
    std::string capacity_status    = "UNAVAILABLE";
    std::string capacity_message;
};

struct ObjectStorageMetricsHttpResponse {
    bool        transport_ok = false;
    long        status_code  = 0;
    std::string body;
    std::string error;
};

using ObjectStorageMetricsHttpExecutor = std::function<ObjectStorageMetricsHttpResponse(
    const std::string &, const std::vector<std::string> &, long)>;
using ObjectStorageMetricsClock = std::function<std::chrono::system_clock::time_point()>;

class ObjectStorageMetricsProvider {
public:
    virtual ~ObjectStorageMetricsProvider() = default;
    virtual ObjectStorageMetricsResult collect(
        const ObjectStorageMetricsContext &context) const = 0;
};

class MinioPrometheusMetricsProvider : public ObjectStorageMetricsProvider {
public:
    explicit MinioPrometheusMetricsProvider(
        ObjectStorageMetricsHttpExecutor http_executor = ObjectStorageMetricsHttpExecutor(),
        ObjectStorageMetricsClock clock = ObjectStorageMetricsClock());

    ObjectStorageMetricsResult collect(
        const ObjectStorageMetricsContext &context) const override;

    static std::string createPrometheusToken(
        const std::string &access_key,
        const std::string &secret_key,
        const std::chrono::system_clock::time_point &now);

private:
    ObjectStorageMetricsHttpExecutor _http_executor;
    ObjectStorageMetricsClock _clock;
};

} // namespace managerkit

#endif // ENABLE_AWS_SDK

#endif // LOCAL_OBJECT_STORAGE_METRICS_PROVIDER_H
