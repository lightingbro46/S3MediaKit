#include "ObjectStorageMetricsProvider.h"

#ifdef ENABLE_AWS_SDK

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>

#include <curl/curl.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace managerkit {

namespace {

using JwtTraits = jwt::traits::open_source_parsers_jsoncpp;

const size_t kMaxMetricsResponseBytes = 4U * 1024U * 1024U;
const long kMetricsTimeoutMs = 3000;

struct CurlWriteContext {
    std::string body;
    size_t max_bytes = 0;
    bool limit_exceeded = false;
};

size_t writeMetricsBody(char *ptr, size_t size, size_t count, void *opaque) {
    CurlWriteContext *context = static_cast<CurlWriteContext *>(opaque);
    if (!context || size == 0)
        return 0;
    if (count > std::numeric_limits<size_t>::max() / size) {
        if (context)
            context->limit_exceeded = true;
        return 0;
    }
    const size_t bytes = size * count;
    if (bytes > context->max_bytes - context->body.size()) {
        context->limit_exceeded = true;
        return 0;
    }
    try {
        context->body.append(ptr, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}

ObjectStorageMetricsHttpResponse performMetricsGet(
        const std::string &url,
        const std::vector<std::string> &headers,
        long timeout_ms) {
    ObjectStorageMetricsHttpResponse response;
    CURL *curl = curl_easy_init();
    if (!curl) {
        response.error = "Cannot initialize HTTP client";
        return response;
    }

    CurlWriteContext write_context;
    write_context.max_bytes = kMaxMetricsResponseBytes;
    char error_buffer[CURL_ERROR_SIZE] = {0};
    struct curl_slist *header_list = nullptr;
    for (const auto &header : headers)
        header_list = curl_slist_append(header_list, header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeMetricsBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    if (header_list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    if (header_list)
        curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    response.body.swap(write_context.body);
    response.transport_ok = result == CURLE_OK && !write_context.limit_exceeded;
    if (write_context.limit_exceeded) {
        response.error = "Metrics response exceeds size limit";
    } else if (result != CURLE_OK) {
        response.error = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
    }
    return response;
}

bool parseFirstMetric(const std::string &body,
                      const std::string &name,
                      double &out_value) {
    std::istringstream input(body);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.compare(0, name.size(), name) != 0)
            continue;
        if (line.size() <= name.size())
            continue;
        const char separator = line[name.size()];
        if (separator != '{' && !std::isspace(static_cast<unsigned char>(separator)))
            continue;

        size_t value_pos = name.size();
        if (separator == '{') {
            const size_t labels_end = line.find('}', value_pos + 1);
            if (labels_end == std::string::npos)
                continue;
            value_pos = labels_end + 1;
        }
        while (value_pos < line.size() &&
               std::isspace(static_cast<unsigned char>(line[value_pos]))) {
            ++value_pos;
        }
        if (value_pos >= line.size())
            continue;

        std::istringstream value_stream(line.substr(value_pos));
        double value = 0.0;
        if (!(value_stream >> value) || !std::isfinite(value) || value < 0.0)
            continue;
        out_value = value;
        return true;
    }
    return false;
}

bool metricToUint64(const std::string &body,
                    const std::string &name,
                    uint64_t &out_value) {
    double value = 0.0;
    if (!parseFirstMetric(body, name, value) ||
        value > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        return false;
    }
    out_value = static_cast<uint64_t>(value);
    return true;
}

std::string trimEndpoint(std::string endpoint) {
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    return endpoint;
}

std::string httpFailureMessage(const ObjectStorageMetricsHttpResponse &response) {
    if (!response.transport_ok)
        return response.error.empty() ? "MinIO metrics request failed" : response.error;
    if (response.status_code == 401 || response.status_code == 403)
        return "MinIO metrics authorization failed (HTTP " +
               std::to_string(response.status_code) + ")";
    return "MinIO metrics returned HTTP " + std::to_string(response.status_code);
}

} // namespace

MinioPrometheusMetricsProvider::MinioPrometheusMetricsProvider(
        ObjectStorageMetricsHttpExecutor http_executor,
        ObjectStorageMetricsClock clock)
    : _http_executor(http_executor ? http_executor : performMetricsGet),
      _clock(clock ? clock : []() { return std::chrono::system_clock::now(); }) {}

std::string MinioPrometheusMetricsProvider::createPrometheusToken(
        const std::string &access_key,
        const std::string &secret_key,
        const std::chrono::system_clock::time_point &now) {
    return jwt::create<JwtTraits>()
        .set_type("JWT")
        .set_issuer("prometheus")
        .set_subject(access_key)
        .set_expires_at(now + std::chrono::minutes(15))
        .sign(jwt::algorithm::hs512(secret_key));
}

ObjectStorageMetricsResult MinioPrometheusMetricsProvider::collect(
        const ObjectStorageMetricsContext &context) const {
    ObjectStorageMetricsResult result;
    if (context.endpoint.empty() || context.access_key.empty() || context.secret_key.empty()) {
        result.capacity_message = "MinIO metrics credentials are incomplete";
        return result;
    }

    std::string token;
    try {
        token = createPrometheusToken(context.access_key, context.secret_key, _clock());
    } catch (const std::exception &ex) {
        result.capacity_message = std::string("Cannot create MinIO metrics token: ") + ex.what();
        return result;
    }

    const std::vector<std::string> headers{
        "Authorization: Bearer " + token,
        "Accept: text/plain"
    };
    const std::string endpoint = trimEndpoint(context.endpoint);
    const ObjectStorageMetricsHttpResponse cluster = _http_executor(
        endpoint + "/minio/v2/metrics/cluster", headers, kMetricsTimeoutMs);
    if (!cluster.transport_ok || cluster.status_code != 200) {
        result.capacity_message = httpFailureMessage(cluster);
        return result;
    }

    uint64_t total = 0;
    uint64_t free = 0;
    if (!metricToUint64(cluster.body, "minio_cluster_capacity_raw_total_bytes", total) ||
        !metricToUint64(cluster.body, "minio_cluster_capacity_raw_free_bytes", free) ||
        total == 0 || free > total) {
        result.capacity_message = "MinIO capacity metrics are missing or invalid";
        return result;
    }

    result.capacity_available = true;
    result.capacity_status = "AVAILABLE";
    result.total_bytes = total;
    result.free_bytes = free;
    result.used_bytes = total - free;

    const ObjectStorageMetricsHttpResponse bucket = _http_executor(
        endpoint + "/minio/v2/metrics/bucket", headers, kMetricsTimeoutMs);
    uint64_t rx = 0;
    uint64_t tx = 0;
    if (bucket.transport_ok && bucket.status_code == 200 &&
        metricToUint64(bucket.body, "minio_bucket_traffic_received_bytes", rx) &&
        metricToUint64(bucket.body, "minio_bucket_traffic_sent_bytes", tx)) {
        result.traffic_available = true;
        result.rx_bytes = rx;
        result.tx_bytes = tx;
    }
    return result;
}

} // namespace managerkit

#endif // ENABLE_AWS_SDK
