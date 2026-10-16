#include <gtest/gtest.h>
#include <cmath>
#include <map>
#include "Server/GlobalMonitor.h"
#include "Server/HealthEvaluator.h"
#include "Server/HealthMonitor.h"
#include "Server/HealthProbe.h"

using namespace managerkit;

namespace {

HealthThreshold th(double warn, double crit, bool higher = true) {
    HealthThreshold t;
    t.warn = warn;
    t.crit = crit;
    t.higher_is_worse = higher;
    return t;
}

// Feeds `value` for `cycles` cycles and returns the last evaluation.
HealthEvaluation run(HealthHysteresis &hy, int sustain, int64_t &now, int cycles, double value,
                     const std::string &component = "host") {
    HealthEvaluation out;
    for (int i = 0; i < cycles; ++i) {
        hy.setCycle((uint64_t)now);
        HealthEvaluator ev(hy, sustain, now);
        ev.addValue(component, "cpu.usage_pct", value, th(80, 90));
        out = ev.finish();
        now += 10;
    }
    return out;
}

} // namespace

TEST(HealthEvaluator, ClassifiesBothDirections) {
    EXPECT_EQ(HealthLevel::OK, classifyValue(10, th(80, 90)));
    EXPECT_EQ(HealthLevel::WARNING, classifyValue(80, th(80, 90)));
    EXPECT_EQ(HealthLevel::CRITICAL, classifyValue(95, th(80, 90)));
    EXPECT_EQ(HealthLevel::UNKNOWN, classifyValue(NAN, th(80, 90)));
    // fps ratio: lower is worse.
    EXPECT_EQ(HealthLevel::OK, classifyValue(0.95, th(0.8, 0.5, false)));
    EXPECT_EQ(HealthLevel::WARNING, classifyValue(0.7, th(0.8, 0.5, false)));
    EXPECT_EQ(HealthLevel::CRITICAL, classifyValue(0.4, th(0.8, 0.5, false)));
}

TEST(HealthEvaluator, UnknownUntilSustainedThenHysteresis) {
    HealthHysteresis hy;
    int64_t now = 1000;
    // First two samples: not confirmed yet.
    auto e = run(hy, 3, now, 2, 10);
    EXPECT_EQ(HealthLevel::UNKNOWN, e.overall);
    EXPECT_EQ(HealthLevel::UNKNOWN, e.components["host"].level);
    e = run(hy, 3, now, 1, 10);
    EXPECT_EQ(HealthLevel::OK, e.overall);

    // A single spike does not flip the level.
    e = run(hy, 3, now, 1, 95);
    EXPECT_EQ(HealthLevel::OK, e.overall);
    e = run(hy, 3, now, 1, 10);
    EXPECT_EQ(HealthLevel::OK, e.overall);

    // Three critical samples in a row do; "since" is the first of them.
    int64_t first_bad = now;
    e = run(hy, 3, now, 3, 95);
    EXPECT_EQ(HealthLevel::CRITICAL, e.overall);
    ASSERT_EQ(1u, e.reasons.size());
    EXPECT_EQ("cpu.usage_pct", e.reasons[0].metric);
    EXPECT_EQ(90, e.reasons[0].threshold);
    EXPECT_EQ(first_bad, e.reasons[0].since);

    // Recovery needs the same number of samples.
    e = run(hy, 3, now, 2, 10);
    EXPECT_EQ(HealthLevel::CRITICAL, e.overall);
    e = run(hy, 3, now, 1, 10);
    EXPECT_EQ(HealthLevel::OK, e.overall);
    EXPECT_TRUE(e.reasons.empty());
}

TEST(HealthEvaluator, NoDataResetsToUnknown) {
    HealthHysteresis hy;
    int64_t now = 0;
    run(hy, 1, now, 1, 95);
    auto e = run(hy, 1, now, 1, NAN);
    EXPECT_EQ(HealthLevel::UNKNOWN, e.overall);
    EXPECT_TRUE(e.reasons.empty());
}

TEST(HealthEvaluator, ExternalComponentsCapOverallAtWarning) {
    HealthHysteresis hy;
    HealthEvaluator ev(hy, 1, 100);
    ev.addValue("host", "cpu.usage_pct", 10, th(80, 90));
    ev.addLevel("ingest", "cameras.offline_pct", HealthLevel::CRITICAL, 30, 20);
    auto e = ev.finish();
    EXPECT_EQ(HealthLevel::CRITICAL, e.components["ingest"].level);
    EXPECT_EQ(HealthLevel::WARNING, e.overall);
    EXPECT_EQ(70, e.score);

    HealthHysteresis hy2;
    HealthEvaluator ev2(hy2, 1, 100);
    ev2.addLevel("recording", "recording.stalled", HealthLevel::CRITICAL, 5, 0);
    ev2.addLevel("delivery", "play.failure_pct", HealthLevel::WARNING, 6, 5);
    auto e2 = ev2.finish();
    EXPECT_EQ(HealthLevel::CRITICAL, e2.overall);
    EXPECT_EQ(60, e2.score);
    // Most severe reason first.
    ASSERT_EQ(2u, e2.reasons.size());
    EXPECT_EQ("recording", e2.reasons[0].component);
}

TEST(HealthEvaluator, DisabledAndEmptyComponentsAreNotCounted) {
    HealthHysteresis hy;
    HealthEvaluator ev(hy, 1, 100);
    ev.setEnabled("cluster", false);
    ev.addValue("host", "cpu.usage_pct", 10, th(80, 90));
    auto e = ev.finish();
    EXPECT_FALSE(e.components["cluster"].enabled);
    EXPECT_EQ(HealthLevel::UNKNOWN, e.components["cluster"].level);
    EXPECT_EQ(HealthLevel::UNKNOWN, e.components["ingest"].level);
    EXPECT_EQ(HealthLevel::OK, e.overall);
    EXPECT_EQ(100, e.score);

    HealthHysteresis hy2;
    HealthEvaluator empty(hy2, 1, 100);
    EXPECT_EQ(HealthLevel::UNKNOWN, empty.finish().overall);
}

TEST(HealthEvaluator, InstancesAreTrackedSeparatelyAndSwept) {
    HealthHysteresis hy;
    hy.setCycle(1);
    {
        HealthEvaluator ev(hy, 1, 0);
        ev.addValue("storage", "disk.usage_pct", 95, th(80, 90), "/data");
        ev.addValue("storage", "disk.usage_pct", 10, th(80, 90), "/");
        auto e = ev.finish();
        ASSERT_EQ(1u, e.reasons.size());
        EXPECT_EQ("/data", e.reasons[0].detail);
    }
    EXPECT_EQ(2u, hy.size());
    hy.setCycle(2);
    {
        HealthEvaluator ev(hy, 1, 10);
        ev.addValue("storage", "disk.usage_pct", 10, th(80, 90), "/");
        ev.finish();
    }
    hy.sweep(2);
    EXPECT_EQ(1u, hy.size());
}

TEST(HealthEvaluator, SignatureIgnoresValues) {
    HealthHysteresis hy;
    HealthEvaluator a(hy, 1, 0);
    a.addValue("host", "cpu.usage_pct", 91, th(80, 90));
    auto ea = a.finish();
    HealthEvaluator b(hy, 1, 10);
    b.addValue("host", "cpu.usage_pct", 99, th(80, 90));
    auto eb = b.finish();
    EXPECT_EQ(ea.signature(), eb.signature());
    HealthEvaluator c(hy, 1, 20);
    c.addValue("host", "cpu.usage_pct", 85, th(80, 90));
    EXPECT_NE(eb.signature(), c.finish().signature());
}

TEST(HealthProbe, ParsesProcFiles) {
    double load1 = 0;
    EXPECT_TRUE(HealthProbe::parseLoadAvg("1.25 0.80 0.50 2/345 6789\n", load1));
    EXPECT_DOUBLE_EQ(1.25, load1);
    EXPECT_FALSE(HealthProbe::parseLoadAvg("", load1));

    uint64_t total = 0, free_bytes = 0;
    const char *meminfo = "MemTotal:       16000000 kB\nSwapCached:  0 kB\nSwapTotal:       2048 kB\nSwapFree:        1024 kB\n";
    EXPECT_TRUE(HealthProbe::parseSwap(meminfo, total, free_bytes));
    EXPECT_EQ(2048u * 1024, total);
    EXPECT_EQ(1024u * 1024, free_bytes);
    EXPECT_FALSE(HealthProbe::parseSwap("MemTotal: 1 kB\n", total, free_bytes));

    EXPECT_EQ(42, HealthProbe::parseThreads("Name:\tMediaServer\nThreads:\t42\nSigQ:\t0/1\n"));
    EXPECT_EQ(-1, HealthProbe::parseThreads("Name:\tx\n"));

    uint64_t fds = 0, limit = 0;
    EXPECT_TRUE(HealthProbe::readFdUsage(fds, limit));
    EXPECT_GT(fds, 0u);
}

TEST(HealthMonitorConfig, ValidatesChanges) {
    using namespace HealthMonitorConfig;
    std::map<std::string, std::string> current = {
        { "monitor.pollerDelayMsWarn", "50" },
        { "monitor.pollerDelayMsCrit", "200" },
        { GlobalMonitorConfig::kCpuWarningThreshold, "80" },
        { GlobalMonitorConfig::kCpuCriticalThreshold, "90" },
    };
    auto get = [&](const std::string &key) {
        auto it = current.find(key);
        return it == current.end() ? std::string() : it->second;
    };
    EXPECT_EQ("", validateChanges({ { "monitor.pollerDelayMsWarn", "100" } }, get));
    EXPECT_EQ("", validateChanges({ { GlobalMonitorConfig::kCpuWarningThreshold, "70" } }, get));
    // warn >= crit against the stored crit, and against a crit changed in the same call.
    EXPECT_NE("", validateChanges({ { "monitor.pollerDelayMsWarn", "200" } }, get));
    EXPECT_EQ("", validateChanges({ { "monitor.pollerDelayMsWarn", "300" }, { "monitor.pollerDelayMsCrit", "400" } }, get));
    EXPECT_NE("", validateChanges({ { GlobalMonitorConfig::kCpuCriticalThreshold, "50" } }, get));
    // Unknown keys, api.secret and reader limits are not editable.
    EXPECT_NE("", validateChanges({ { "api.secret", "x" } }, get));
    EXPECT_NE("", validateChanges({ { "monitor.unknown", "1" } }, get));
    // Not a number / out of range.
    EXPECT_NE("", validateChanges({ { "monitor.pollerDelayMsWarn", "abc" } }, get));
    EXPECT_NE("", validateChanges({ { "monitor.pollerDelayMsWarn", "-1" } }, get));
    EXPECT_NE("", validateChanges({ { kIntervalSec, "0" } }, get));
    EXPECT_NE("", validateChanges({ { kIntervalSec, "2.5" } }, get));
    EXPECT_NE("", validateChanges({ { kEnable, "2" } }, get));
    EXPECT_EQ("", validateChanges({ { kEnable, "0" } }, get));
}

TEST(HealthMonitorConfig, ThresholdsFollowConfig) {
    using namespace HealthMonitorConfig;
    auto def = threshold("pollerDelayMs");
    EXPECT_DOUBLE_EQ(50, def.warn);
    EXPECT_DOUBLE_EQ(200, def.crit);
    toolkit::mINI::Instance()["monitor.pollerDelayMsWarn"] = 70;
    EXPECT_DOUBLE_EQ(70, threshold("pollerDelayMs").warn);
    toolkit::mINI::Instance()["monitor.pollerDelayMsWarn"] = 50;
}
