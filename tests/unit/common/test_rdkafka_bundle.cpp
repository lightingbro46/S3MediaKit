// librdkafka as built by cmake/BundledRdKafka.cmake: the features the event producer relies on
// are compiled in, and a message round-trips through the in-process mock cluster with each
// compression codec (so OpenSSL/zstd/lz4 are really linked, not just declared).

#include <gtest/gtest.h>

#include <librdkafka/rdkafka.h>

#include <memory>
#include <string>

namespace {

struct ConfDeleter {
    void operator()(rd_kafka_conf_t *conf) const { rd_kafka_conf_destroy(conf); }
};
struct KafkaDeleter {
    void operator()(rd_kafka_t *rk) const { rd_kafka_destroy(rk); }
};
using ConfPtr = std::unique_ptr<rd_kafka_conf_t, ConfDeleter>;
using KafkaPtr = std::unique_ptr<rd_kafka_t, KafkaDeleter>;

bool setConf(rd_kafka_conf_t *conf, const char *key, const char *value, std::string &err) {
    char buf[512];
    if (rd_kafka_conf_set(conf, key, value, buf, sizeof(buf)) != RD_KAFKA_CONF_OK) {
        err = std::string(key) + ": " + buf;
        return false;
    }
    return true;
}

struct DeliveryCount {
    int ok = 0;
    int failed = 0;
    std::string last_error;
};

void onDelivery(rd_kafka_t *, const rd_kafka_message_t *msg, void *opaque) {
    auto count = static_cast<DeliveryCount *>(opaque);
    if (msg->err) {
        ++count->failed;
        count->last_error = rd_kafka_err2str(msg->err);
    } else {
        ++count->ok;
    }
}

} // namespace

TEST(RdKafkaBundleTest, BuiltWithRequiredFeatures) {
    ConfPtr conf(rd_kafka_conf_new());
    char features[512];
    size_t size = sizeof(features);
    ASSERT_EQ(RD_KAFKA_CONF_OK, rd_kafka_conf_get(conf.get(), "builtin.features", features, &size));
    const std::string list = std::string(",") + features + ",";
    for (const char *feature : { "ssl", "sasl_plain", "sasl_scram", "zstd", "lz4" }) {
        EXPECT_NE(std::string::npos, list.find(std::string(",") + feature + ","))
            << feature << " missing from builtin.features=" << features;
    }
    EXPECT_GE(rd_kafka_version(), 0x020f0000) << rd_kafka_version_str();
}

TEST(RdKafkaBundleTest, AcceptsSaslSslSettings) {
    ConfPtr conf(rd_kafka_conf_new());
    std::string err;
    EXPECT_TRUE(setConf(conf.get(), "security.protocol", "sasl_ssl", err)) << err;
    EXPECT_TRUE(setConf(conf.get(), "sasl.mechanism", "SCRAM-SHA-512", err)) << err;
    EXPECT_TRUE(setConf(conf.get(), "sasl.username", "user", err)) << err;
    EXPECT_TRUE(setConf(conf.get(), "sasl.password", "secret", err)) << err;
}

TEST(RdKafkaBundleTest, IdempotentProduceThroughMockCluster) {
    for (const char *codec : { "lz4", "zstd", "gzip" }) {
        SCOPED_TRACE(codec);
        DeliveryCount count;
        ConfPtr conf(rd_kafka_conf_new());
        std::string err;
        ASSERT_TRUE(setConf(conf.get(), "test.mock.num.brokers", "1", err)) << err;
        ASSERT_TRUE(setConf(conf.get(), "enable.idempotence", "true", err)) << err;
        ASSERT_TRUE(setConf(conf.get(), "compression.codec", codec, err)) << err;
        ASSERT_TRUE(setConf(conf.get(), "linger.ms", "5", err)) << err;
        ASSERT_TRUE(setConf(conf.get(), "message.timeout.ms", "5000", err)) << err;
        rd_kafka_conf_set_dr_msg_cb(conf.get(), onDelivery);
        rd_kafka_conf_set_opaque(conf.get(), &count);

        char errstr[512];
        KafkaPtr rk(rd_kafka_new(RD_KAFKA_PRODUCER, conf.get(), errstr, sizeof(errstr)));
        ASSERT_TRUE(rk) << errstr;
        conf.release(); // owned by rk now

        const int total = 50;
        for (int i = 0; i < total; ++i) {
            const std::string key = "camera-" + std::to_string(i % 5);
            const std::string value = "{\"seq\":" + std::to_string(i) + "}";
            rd_kafka_resp_err_t rc = rd_kafka_producev(
                rk.get(), RD_KAFKA_V_TOPIC("device-events"), RD_KAFKA_V_KEY(key.data(), key.size()),
                RD_KAFKA_V_VALUE(const_cast<char *>(value.data()), value.size()),
                RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_END);
            ASSERT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rc) << rd_kafka_err2str(rc);
        }
        EXPECT_EQ(RD_KAFKA_RESP_ERR_NO_ERROR, rd_kafka_flush(rk.get(), 10000));
        EXPECT_EQ(total, count.ok);
        EXPECT_EQ(0, count.failed) << count.last_error;
    }
}
