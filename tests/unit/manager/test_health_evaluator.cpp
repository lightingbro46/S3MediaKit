#include <gtest/gtest.h>
#include <cmath>
#include <map>
#include "Camera/RecordPolicy.h"
#include "Camera/StreamQuality.h"
#include "Server/AlertRegistry.h"
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

TEST(HealthRecording, ExpectedFollowsScheduleMode) {
    EXPECT_FALSE(isRecordingExpected(true, RecordMode::NoRecord, true));
    EXPECT_TRUE(isRecordingExpected(true, RecordMode::RecordAlways, false));
    EXPECT_TRUE(isRecordingExpected(true, RecordMode::RecordLowResAndMotion, false));
    // Motion only: expected while the event is open, a quiet day is not a failure.
    EXPECT_FALSE(isRecordingExpected(true, RecordMode::RecordOnlyMotion, false));
    EXPECT_TRUE(isRecordingExpected(true, RecordMode::RecordOnlyMotion, true));
    // enableRecord = false wins over any schedule.
    EXPECT_FALSE(isRecordingExpected(false, RecordMode::RecordAlways, true));
}

TEST(HealthRecording, StateWithGraceAndStaleTime) {
    const int stale = defaultRecordStaleSec(60);
    EXPECT_EQ(150, stale);
    const int64_t now = 100000;
    // Not expected and nothing written.
    EXPECT_EQ(RecordingState::NOT_EXPECTED, evaluateRecording(false, now - 1000, 0, now, stale));
    // Not expected but still writing (post-record of a motion event): active, never stalled.
    EXPECT_EQ(RecordingState::ACTIVE, evaluateRecording(false, now - 1000, now - 10, now, stale));
    // Became expected 30 s ago, first segment not closed yet: grace.
    EXPECT_EQ(RecordingState::STARTING, evaluateRecording(true, now - 30, 0, now, stale));
    // Expected for long, segment closed recently.
    EXPECT_EQ(RecordingState::ACTIVE, evaluateRecording(true, now - 3600, now - stale, now, stale));
    // Expected for long, last segment too old or never.
    EXPECT_EQ(RecordingState::STALLED, evaluateRecording(true, now - 3600, now - stale - 1, now, stale));
    EXPECT_EQ(RecordingState::STALLED, evaluateRecording(true, now - stale, 0, now, stale));
}

TEST(HealthAlerts, RegistryKeepsOpenAlertsAndRecentEvents) {
    auto &reg = AlertRegistry::Instance();
    reg.clear();
    reg.onEvent("Cpu", "", "CpuWarning", AlertRegistry::WARNING, 85, 80, Json::Value(Json::objectValue), 100);
    reg.onEvent("DiskWrite", "/data/record", "DiskWriteFailed", AlertRegistry::CRITICAL, 2, 0, Json::Value(Json::objectValue), 110);
    // Escalation keeps the start time.
    reg.onEvent("Cpu", "", "CpuCritical", AlertRegistry::CRITICAL, 95, 90, Json::Value(Json::objectValue), 120);
    auto j = reg.toJson(0);
    ASSERT_EQ(2u, j["active"].size());
    EXPECT_EQ(3u, j["recent"].size());
    EXPECT_EQ(3u, j["seq"].asUInt64());
    // Both critical now: oldest first (Cpu since 100, disk since 110).
    EXPECT_EQ("CpuCritical", j["active"][0]["eventCode"].asString());
    EXPECT_EQ(100, j["active"][0]["since"].asInt64());
    EXPECT_EQ(2u, reg.activeCount(AlertRegistry::CRITICAL));

    reg.onEvent("Cpu", "", "CpuRecovered", AlertRegistry::RECOVERED, 20, 80, Json::Value(Json::objectValue), 130);
    EXPECT_EQ(1u, reg.activeCount());
    EXPECT_FALSE(reg.isActive("Cpu", ""));
    EXPECT_TRUE(reg.isActive("DiskWrite", "/data/record"));
    // Only events after `since`.
    auto j2 = reg.toJson(3);
    ASSERT_EQ(1u, j2["recent"].size());
    EXPECT_EQ("CpuRecovered", j2["recent"][0]["eventCode"].asString());
    EXPECT_EQ(100, j2["recent"][0]["since"].asInt64());

    // The ring keeps the newest kRecentCapacity events.
    for (int i = 0; i < 600; ++i) {
        reg.onEvent("CameraStreamLimit", "cam", i % 2 ? "CameraStreamLimitRecovered" : "CameraStreamLimitExceeded",
                    i % 2 ? AlertRegistry::RECOVERED : AlertRegistry::WARNING, i, 10);
    }
    auto j3 = reg.toJson(0);
    EXPECT_EQ(AlertRegistry::kRecentCapacity, j3["recent"].size());
    EXPECT_EQ(604u, j3["seq"].asUInt64());
    EXPECT_EQ(1u, reg.toJson(0, 1)["recent"].size());
    reg.clear();
}

TEST(HealthAlerts, RetentionManagedDiskUsage) {
    auto &reg = AlertRegistry::Instance();
    reg.clear();
    std::map<std::string, std::pair<double, double> > managed;
    managed["/data"] = std::make_pair(93.0, 96.0);
    reg.setManagedDiskThresholds(managed);

    // Retention keeps /data at 85-90 %: the 80/90 raises there are dropped or lowered.
    AlertRegistry::Level level = AlertRegistry::WARNING;
    double th = 80;
    EXPECT_FALSE(reg.filterDiskUsage("/data", 88, level, th));
    level = AlertRegistry::CRITICAL;
    th = 90;
    EXPECT_TRUE(reg.filterDiskUsage("/data", 94, level, th));
    EXPECT_EQ(AlertRegistry::WARNING, level);
    EXPECT_EQ(93, th);
    level = AlertRegistry::CRITICAL;
    EXPECT_TRUE(reg.filterDiskUsage("/data", 97, level, th));
    EXPECT_EQ(AlertRegistry::CRITICAL, level);
    EXPECT_EQ(96, th);

    // Other mounts keep the alert as raised.
    level = AlertRegistry::WARNING;
    th = 80;
    EXPECT_TRUE(reg.filterDiskUsage("/", 85, level, th));
    EXPECT_EQ(AlertRegistry::WARNING, level);
    EXPECT_EQ(80, th);
    reg.clear();
}

TEST(HealthAlerts, SingleEventsStayOutOfActive) {
    auto &reg = AlertRegistry::Instance();
    reg.clear();
    Json::Value extra;
    extra["jobId"] = "job-1";
    reg.addEvent("TieringJob", "job-1", "TieringJobFailed", AlertRegistry::WARNING, NAN, NAN, extra, 100);
    EXPECT_EQ(0u, reg.activeCount());
    EXPECT_FALSE(reg.isActive("TieringJob", "job-1"));
    auto j = reg.toJson(0);
    ASSERT_EQ(1u, j["recent"].size());
    EXPECT_EQ("TieringJobFailed", j["recent"][0]["eventCode"].asString());
    EXPECT_EQ("job-1", j["recent"][0]["entity"].asString());
    EXPECT_EQ("job-1", j["recent"][0]["jobId"].asString());
    EXPECT_EQ(100, j["recent"][0]["at"].asInt64());
    EXPECT_TRUE(j["recent"][0]["currentValue"].isNull());
    EXPECT_EQ(0u, reg.toJson(j["seq"].asUInt64())["recent"].size());
    reg.clear();
}

TEST(HealthCamera, ErrorTextDropsUrlCredentials) {
    EXPECT_EQ("play rtsp://10.0.0.5:554/Streaming/101 failed: 401 Unauthorized",
              sanitizeErrorText("play rtsp://admin:p@ss@10.0.0.5:554/Streaming/101 failed: 401 Unauthorized", 200));
    EXPECT_EQ("rtsp://10.0.0.5 and http://cam/onvif",
              sanitizeErrorText("rtsp://u:p@10.0.0.5 and http://x@cam/onvif", 200));
    EXPECT_EQ("mail a@b, rtsp://10.0.0.5/x?u=a@b", sanitizeErrorText("mail a@b, rtsp://10.0.0.5/x?u=a@b", 200));
    EXPECT_EQ("connection timeout", sanitizeErrorText("connection timeout", 200));
    // Cut on a UTF-8 boundary: "lỗi" = l + 3 bytes + i.
    EXPECT_EQ("l", sanitizeErrorText("l\xe1\xbb\x97i", 3));
    EXPECT_EQ("abc", sanitizeErrorText("abcdef", 3));
}

TEST(StreamQuality, UptimeAndDisconnects) {
    const int64_t t0 = 1000000;
    StreamQuality q(t0);
    q.onLive(true, t0);
    q.onLive(false, t0 + 600);   // 10 min live, then down
    q.onLive(true, t0 + 900);    // 5 min down
    q.onLive(true, t0 + 950);    // same state: ignored
    auto s = q.snapshot(t0 + 1200);
    EXPECT_TRUE(s.live);
    EXPECT_EQ(1, s.disconnects_1h);
    EXPECT_EQ(1, s.disconnects_24h);
    EXPECT_NEAR(75.0, s.uptime_pct_24h, 0.01); // 900 of 1200 s
    EXPECT_EQ(t0, s.window_started_at);
    // Two hours later the disconnect left the 1 h window; a day later the 24 h one too.
    s = q.snapshot(t0 + 600 + 7200);
    EXPECT_EQ(0, s.disconnects_1h);
    EXPECT_EQ(1, s.disconnects_24h);
    s = q.snapshot(t0 + 600 + 90000);
    EXPECT_EQ(0, s.disconnects_24h);
    EXPECT_NEAR(100.0, s.uptime_pct_24h, 0.01);
}

TEST(StreamQuality, BaselineAfterWarmupOnSteadyWindow) {
    StreamBaselineConfig cfg;
    cfg.warmup_sec = 120;
    cfg.window_sec = 300;
    const int64_t t0 = 0;
    StreamQuality q(t0);
    q.onLive(true, t0);
    // Samples during warm-up do not count.
    q.onSample(5, 100, "H264 1920x1080", t0 + 60, cfg);
    EXPECT_LT(q.snapshot(t0 + 60).fps_baseline, 0);
    for (int i = 0; i < 6; ++i) {
        q.onSample(25 + (i % 2), 2000, "H264 1920x1080", t0 + 120 + 60 * i, cfg);
    }
    auto s = q.snapshot(t0 + 500);
    EXPECT_NEAR(25.5, s.fps_baseline, 0.6);
    EXPECT_NEAR(2000, s.bitrate_baseline, 0.1);
    EXPECT_GT(s.baseline_at, 0);
    EXPECT_DOUBLE_EQ(26, s.fps);
}

TEST(StreamQuality, UnsteadyWindowIsDroppedAndBaselineSurvivesDisconnect) {
    StreamBaselineConfig cfg;
    cfg.warmup_sec = 0;
    cfg.window_sec = 240;
    StreamQuality q(0);
    q.onLive(true, 0);
    const double wild[] = { 5, 25, 5, 25, 5 };
    for (int i = 0; i < 5; ++i) {
        q.onSample(wild[i], 1000, "H264 1280x720", 60 * (i + 1), cfg);
    }
    EXPECT_LT(q.snapshot(400).fps_baseline, 0);
    for (int i = 0; i < 5; ++i) {
        q.onSample(15, 1000, "H264 1280x720", 400 + 60 * i, cfg);
    }
    EXPECT_NEAR(15, q.snapshot(700).fps_baseline, 0.01);
    // A reconnect keeps it, but the sample of the old connection is dropped.
    q.onLive(false, 800);
    q.onLive(true, 900);
    EXPECT_NEAR(15, q.snapshot(950).fps_baseline, 0.01);
    EXPECT_LT(q.snapshot(950).fps, 0);
    // A new resolution restarts learning.
    q.onSample(15, 1000, "H264 1920x1080", 1000, cfg);
    EXPECT_LT(q.snapshot(1000).fps_baseline, 0);
}

TEST(StreamQuality, LongDriftIsLearntAgain) {
    StreamBaselineConfig cfg;
    cfg.warmup_sec = 0;
    cfg.window_sec = 120;
    cfg.drift_relearn_sec = 3600;
    StreamQuality q(0);
    q.onLive(true, 0);
    for (int i = 0; i < 4; ++i) {
        q.onSample(25, 1000, "H264", 60 * (i + 1), cfg);
    }
    ASSERT_NEAR(25, q.snapshot(300).fps_baseline, 0.01);
    // The installer lowered the camera to 12 fps: kept as a deviation for an hour...
    int64_t t = 300;
    for (; t < 300 + 3540; t += 60) {
        q.onSample(12, 500, "H264", t, cfg);
    }
    EXPECT_NEAR(25, q.snapshot(t).fps_baseline, 0.01);
    // ... then learnt again.
    for (int i = 0; i < 6; ++i, t += 60) {
        q.onSample(12, 500, "H264", t, cfg);
    }
    EXPECT_NEAR(12, q.snapshot(t).fps_baseline, 0.01);
}

TEST(HealthCamera, EvaluatesOfflineNoVideoAndDegraded) {
    CameraThresholds th = CameraThresholds::fromConfig();
    CameraInput in;
    in.streams[0].present = true;
    in.streams[1].present = true;
    std::vector<std::string> reasons;
    // Both streams down: offline.
    EXPECT_EQ(HealthLevel::CRITICAL, evaluateCamera(in, th, reasons));
    EXPECT_FALSE(cameraOnline(in));
    EXPECT_EQ("offline", reasons[0]);

    // Main live and fine, sub down: online, warning.
    in.streams[0].live = true;
    in.streams[0].last_frame_age_ms = 40;
    reasons.clear();
    EXPECT_EQ(HealthLevel::WARNING, evaluateCamera(in, th, reasons));
    EXPECT_TRUE(cameraOnline(in));

    // Both live; main sends nothing for 15 s: no video = critical.
    in.streams[1].live = true;
    in.streams[1].last_frame_age_ms = 40;
    in.streams[0].last_frame_age_ms = 15000;
    reasons.clear();
    EXPECT_EQ(HealthLevel::CRITICAL, evaluateCamera(in, th, reasons));
    EXPECT_TRUE(cameraNoVideo(in, th));

    // Healthy frames but fps at 40 % of the baseline: only a warning (5.4).
    in.streams[0].last_frame_age_ms = 40;
    in.streams[0].fps_ratio = 0.4;
    reasons.clear();
    EXPECT_EQ(HealthLevel::WARNING, evaluateCamera(in, th, reasons));
    ASSERT_EQ(1u, reasons.size());
    EXPECT_EQ("main.fps_ratio", reasons[0]);

    // No baseline yet (-1): not judged. Not recording while it should: critical.
    in.streams[0].fps_ratio = -1;
    in.recording_stalled = true;
    reasons.clear();
    EXPECT_EQ(HealthLevel::CRITICAL, evaluateCamera(in, th, reasons));
    in.recording_stalled = false;
    in.controller_expected = true;
    in.controller_connected = false;
    reasons.clear();
    EXPECT_EQ(HealthLevel::WARNING, evaluateCamera(in, th, reasons));
    EXPECT_EQ("controller.disconnected", reasons[0]);
}

TEST(HealthMonitorConfig, LowerIsWorsePairsCheckTheOtherWay) {
    using namespace HealthMonitorConfig;
    auto get = [](const std::string &key) {
        return key == "monitor.fpsRatioWarn" ? std::string("0.8") : (key == "monitor.fpsRatioCrit" ? std::string("0.5") : std::string());
    };
    EXPECT_EQ("", validateChanges({ { "monitor.fpsRatioWarn", "0.9" } }, get));
    EXPECT_NE("", validateChanges({ { "monitor.fpsRatioWarn", "0.4" } }, get));
}

TEST(HealthStorage, PoolLevelFollowsPoolHealthCheck) {
    EXPECT_EQ(HealthLevel::OK, poolHealthLevel("OK", false));
    EXPECT_EQ(HealthLevel::WARNING, poolHealthLevel("WARNING", false));
    EXPECT_EQ(HealthLevel::WARNING, poolHealthLevel("HIGH", false));
    EXPECT_EQ(HealthLevel::CRITICAL, poolHealthLevel("CRITICAL", false));
    // MinIO stopped: the health check marks the pool OFFLINE after its failure threshold.
    EXPECT_EQ(HealthLevel::CRITICAL, poolHealthLevel("OFFLINE", false));
    EXPECT_EQ(HealthLevel::UNKNOWN, poolHealthLevel("UNKNOWN", false));
    // No recent check: the last status is not trusted either way.
    EXPECT_EQ(HealthLevel::UNKNOWN, poolHealthLevel("OFFLINE", true));
    EXPECT_EQ(HealthLevel::UNKNOWN, poolHealthLevel("OK", true));
}

TEST(HealthCluster, WebhookFailureRate) {
    const auto t = th(0.01, 20);
    double pct = -1;
    EXPECT_EQ(HealthLevel::OK, failureRateLevel(0, 0, 10, t, pct));
    EXPECT_EQ(0, pct);
    EXPECT_EQ(HealthLevel::OK, failureRateLevel(50, 0, 10, t, pct));
    // Few calls: one lost hook is a warning, never critical.
    EXPECT_EQ(HealthLevel::WARNING, failureRateLevel(1, 1, 10, t, pct));
    EXPECT_EQ(100, pct);
    EXPECT_EQ(HealthLevel::WARNING, failureRateLevel(100, 5, 10, t, pct));
    EXPECT_EQ(HealthLevel::CRITICAL, failureRateLevel(10, 2, 10, t, pct));
    EXPECT_EQ(20, pct);
}

namespace {
// Local time = UTC for the schedule tests.
struct UtcZone {
    UtcZone() {
        const char *tz = getenv("TZ");
        had = tz != nullptr;
        if (had) old = tz;
        setenv("TZ", "UTC", 1);
        tzset();
    }
    ~UtcZone() {
        if (had) setenv("TZ", old.c_str(), 1); else unsetenv("TZ");
        tzset();
    }
    bool had = false;
    std::string old;
};
// 2026-10-12 00:00:00 UTC is a Monday.
const int64_t kMonday = 1791763200;
} // namespace

TEST(HealthCoverage, ScheduleSpansFollowLocalHours) {
    UtcZone utc;
    std::vector<RecordMode> modes(7 * 24, RecordMode::NoRecord);
    modes[1] = RecordMode::RecordAlways;            // Monday 01:00
    modes[2] = RecordMode::RecordLowResAndMotion;   // Monday 02:00
    modes[3] = RecordMode::RecordOnlyMotion;        // motion only: never expected
    modes[5] = RecordMode::RecordAlways;            // Monday 05:00
    auto spans = scheduledRecordSpans(true, modes, kMonday + 1800, kMonday + 5 * 3600 + 600);
    ASSERT_EQ(2u, spans.size());
    // 01:00-03:00 joined, 05:00 cut at `to`.
    EXPECT_EQ(TimeSpan(kMonday + 3600, kMonday + 3 * 3600), spans[0]);
    EXPECT_EQ(TimeSpan(kMonday + 5 * 3600, kMonday + 5 * 3600 + 600), spans[1]);
    EXPECT_TRUE(scheduledRecordSpans(false, modes, kMonday, kMonday + 86400).empty());

    // recordSchedules string: Monday 01:00 always.
    auto parsed = recordModesByHour("[{\"dh\":\"0,1\",\"ty\":3}]");
    EXPECT_EQ(RecordMode::RecordAlways, parsed[1]);
    EXPECT_EQ(RecordMode::NoRecord, parsed[2]);
}

TEST(HealthCoverage, CoverageAndGaps) {
    std::vector<TimeSpan> expected = { { 0, 1000 }, { 2000, 3000 } };
    // 0-400 recorded, 400-500 missing (100 s), 500-1000 recorded, 2000-3000 nothing.
    std::vector<TimeSpan> recorded = { { 500, 1200 }, { -50, 400 } };
    auto cov = computeRecordCoverage(expected, recorded, 60);
    EXPECT_EQ(2000, cov.expected_sec);
    EXPECT_EQ(900, cov.recorded_sec);
    EXPECT_EQ(2, cov.gaps);
    EXPECT_EQ(1000, cov.longest_gap_sec);
    EXPECT_DOUBLE_EQ(45, cov.pct());
    // Holes up to gap_min_sec are not gaps.
    cov = computeRecordCoverage({ { 0, 1000 } }, { { 0, 480 }, { 520, 1000 } }, 60);
    EXPECT_EQ(0, cov.gaps);
    EXPECT_EQ(960, cov.recorded_sec);
    // Nothing expected.
    EXPECT_EQ(-1, computeRecordCoverage({}, recorded, 60).pct());
    // Spans helpers.
    EXPECT_EQ(std::vector<TimeSpan>({ { 0, 30 } }), mergeSpans({ { 10, 30 }, { 0, 10 }, { 5, 6 } }));
    EXPECT_EQ(std::vector<TimeSpan>({ { 5, 10 }, { 20, 25 } }),
              intersectSpans({ { 0, 10 }, { 20, 30 } }, { { 5, 25 } }));
}

TEST(HealthHistory, RssGrowthAndPercentile) {
    std::vector<std::pair<int64_t, int64_t> > samples;
    const int64_t mb = 1024 * 1024;
    for (int i = 0; i <= 180; ++i) {
        // 100 MB/h over 3 h.
        samples.emplace_back(1000 + i * 60, 500 * mb + (int64_t)(i * 60 / 3600.0 * 100 * mb));
    }
    EXPECT_NEAR(100, rssGrowthMbPerHour(samples, 7200), 0.5);
    // Less than the minimum span: not judged.
    samples.resize(60);
    EXPECT_TRUE(std::isnan(rssGrowthMbPerHour(samples, 7200)));

    EXPECT_TRUE(std::isnan(percentileOf({}, 95)));
    std::vector<uint32_t> values;
    for (uint32_t i = 1; i <= 100; ++i) values.push_back(i);
    EXPECT_EQ(95, percentileOf(values, 95));
    EXPECT_EQ(100, percentileOf(values, 100));
    EXPECT_EQ(7, percentileOf({ 7 }, 95));
}

TEST(HealthHistory, BucketsKeepWorstLevelAndAverages) {
    std::vector<HealthMetric> rows;
    for (int i = 0; i < 4; ++i) {
        HealthMetric m;
        m.timestamp = 600 + i * 60;      // 600, 660 in bucket 600; 720, 780 in bucket 720 (bucket 120)
        m.overall = i == 1 ? "critical" : "ok";
        m.score = i == 1 ? 70 : 100;
        m.cameras_online = 10 + i;
        m.proc_rss = 100 + i;
        m.poller_delay_max_ms = (float)i;
        rows.push_back(m);
    }
    auto points = healthHistoryPoints(rows, 120);
    ASSERT_EQ(2u, points.size());
    EXPECT_EQ(600, points[0]["timestamp"].asInt64());
    EXPECT_EQ(2, points[0]["samples"].asInt());
    EXPECT_EQ("critical", points[0]["overall"].asString());
    EXPECT_DOUBLE_EQ(85, points[0]["score"].asDouble());
    EXPECT_EQ(70, points[0]["score_min"].asInt());
    EXPECT_DOUBLE_EQ(10.5, points[0]["cameras_online"].asDouble());
    EXPECT_EQ(101, points[0]["proc_rss"].asInt64());
    EXPECT_EQ("ok", points[1]["overall"].asString());
    EXPECT_DOUBLE_EQ(3, points[1]["poller_delay_max_ms"].asDouble());
    // No bucket: one point per row.
    EXPECT_EQ(4u, healthHistoryPoints(rows, 0).size());
}

TEST(HealthCoverage, CameraWarnsOnLowCoverageAndGaps) {
    auto th = CameraThresholds::fromConfig();
    CameraInput in;
    in.streams[0].present = true;
    in.streams[0].live = true;
    std::vector<std::string> reasons;
    EXPECT_EQ(HealthLevel::OK, evaluateCamera(in, th, reasons));
    in.coverage.pct = 90;   // below the critical 95: still a warning per camera
    in.coverage.gaps = 2;
    reasons.clear();
    EXPECT_EQ(HealthLevel::WARNING, evaluateCamera(in, th, reasons));
    ASSERT_EQ(2u, reasons.size());
    EXPECT_EQ("recording.coverage_pct_24h", reasons[0]);
    EXPECT_EQ("recording.gaps_24h", reasons[1]);
}

TEST(StreamQuality, BaselineRestoredAfterRestartUntilFormatChanges) {
    StreamQuality q(1000);
    StreamBaselineConfig cfg;
    EXPECT_TRUE(q.restoreBaseline("H264 1920x1080", 25, 4000, 500));
    auto s = q.snapshot(1000);
    EXPECT_EQ(25, s.fps_baseline);
    EXPECT_EQ(500, s.baseline_at);
    EXPECT_EQ("H264 1920x1080", s.format);
    // A learnt one is not replaced.
    EXPECT_FALSE(q.restoreBaseline("H264 1920x1080", 10, 1000, 600));
    // Same format: kept; another format: dropped, learnt again.
    q.onLive(true, 1000);
    q.onSample(25, 4000, "H264 1920x1080", 1060, cfg);
    EXPECT_EQ(25, q.snapshot(1060).fps_baseline);
    q.onSample(25, 4000, "H265 1920x1080", 1120, cfg);
    EXPECT_EQ(-1, q.snapshot(1120).fps_baseline);
    // A stream that already reports another format does not take the saved one.
    StreamQuality other(1000);
    other.onLive(true, 1000);
    other.onSample(25, 4000, "H265 1920x1080", 1060, cfg);
    EXPECT_FALSE(other.restoreBaseline("H264 1920x1080", 25, 4000, 500));
}

TEST(HealthMonitorConfig, BaselineWindowHoldsThreeSamples) {
    using namespace HealthMonitorConfig;
    auto none = [](const std::string &) { return std::string(); };
    // Samples every 60 s: a window under 120 s never holds the 3 a baseline needs.
    EXPECT_NE("", validateChanges({ { "monitor.baselineWindowSec", "60" } }, none));
    EXPECT_EQ("", validateChanges({ { "monitor.baselineWindowSec", "120" } }, none));
    EXPECT_EQ("", validateChanges({ { "monitor.historyIntervalSec", "0" }, { "monitor.coverageIntervalSec", "60" } }, none));
    EXPECT_NE("", validateChanges({ { "monitor.coverageIntervalSec", "30" } }, none));
    EXPECT_NE("", validateChanges({ { "monitor.historyRetentionDays", "0" } }, none));
    EXPECT_EQ("", validateChanges({ { "monitor.historyRetentionDays", "31" } }, none));
}

TEST(HealthMonitor, SnapshotStaleAfterThreeCyclesOrThirtySeconds) {
    EXPECT_EQ(30, HealthMonitor::staleAfterSec(10));
    EXPECT_EQ(30, HealthMonitor::staleAfterSec(1));
    EXPECT_EQ(60, HealthMonitor::staleAfterSec(20));
    EXPECT_EQ(38, HealthMonitor::staleAfterSec(12.5));
}
