#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "Common/config.h"
#include "Poller/EventPoller.h"
#include "Thread/ThreadPool.h"
#include "Util/NoticeCenter.h"

#include "Server/DiskIoMonitor.h"
#include "Server/HddMonitor.h"
#include "Server/StorageWriteMonitor.h"

using namespace managerkit;
using namespace toolkit;

namespace {

struct IoAlert {
    std::string mount_point;
    std::string device;
    std::string metric;
    float value;
    float threshold;
    int level;
    bool is_main_storage;
};

// Real listener: catches Broadcast*Args declared by value, which would never reach it.
class IoAlertRecorder {
public:
    IoAlertRecorder() {
        NoticeCenter::Instance().addListener(this, mediakit::Broadcast::kBroadcastDiskIoAlert,
                                             [this](BroadcastDiskIoAlertArgs) {
            alerts.push_back(IoAlert{mount_point, device, metric, value, threshold, level, is_main_storage});
        });
    }
    ~IoAlertRecorder() { NoticeCenter::Instance().delListener(this, mediakit::Broadcast::kBroadcastDiskIoAlert); }
    std::vector<IoAlert> alerts;
};

struct UsageAlert {
    std::string mount_point;
    std::string device;
    float usage;
    float threshold;
    bool is_critical;
};

class UsageAlertRecorder {
public:
    UsageAlertRecorder() {
        NoticeCenter::Instance().addListener(this, mediakit::Broadcast::kBroadcastDiskUsageAlert,
                                             [this](BroadcastDiskUsageAlertArgs) {
            alerts.push_back(UsageAlert{mount_point, device, usage, threshold, is_critical});
        });
        NoticeCenter::Instance().addListener(this, mediakit::Broadcast::kBroadcastSystemAlert,
                                             [this](BroadcastSystemAlertArgs) { ++system_alerts; });
    }
    ~UsageAlertRecorder() {
        NoticeCenter::Instance().delListener(this, mediakit::Broadcast::kBroadcastDiskUsageAlert);
        NoticeCenter::Instance().delListener(this, mediakit::Broadcast::kBroadcastSystemAlert);
    }
    std::vector<UsageAlert> alerts;
    int system_alerts = 0;
};

struct WriteAlert {
    std::string path;
    std::string mount_point;
    int state;
    int error;
    bool is_main_storage;
};

class WriteAlertRecorder {
public:
    WriteAlertRecorder() {
        NoticeCenter::Instance().addListener(this, mediakit::Broadcast::kBroadcastDiskWriteAlert,
                                             [this](BroadcastDiskWriteAlertArgs) {
            std::lock_guard<std::mutex> lck(mtx);
            alerts.push_back(WriteAlert{path, mount_point, state, error, is_main_storage});
        });
    }
    ~WriteAlertRecorder() { NoticeCenter::Instance().delListener(this, mediakit::Broadcast::kBroadcastDiskWriteAlert); }
    std::vector<WriteAlert> snapshot() {
        std::lock_guard<std::mutex> lck(mtx);
        return alerts;
    }
    std::mutex mtx;
    std::vector<WriteAlert> alerts;
};

// Scripted procfs: each sampleNow() advances 5 s and adds the given per-interval deltas.
class FakeIo {
public:
    // ticks_ms: device busy time in the interval; ios: completed IOs; io_ms: total time they took.
    void step(const std::string &name, uint64_t ticks_ms, uint64_t ios, uint64_t io_ms, uint64_t sectors = 0) {
        auto &c = disks[name];
        c.io_ticks += ticks_ms;
        c.writes += ios;
        c.write_ms += io_ms;
        c.write_sectors += sectors;
        c.weighted_ms += io_ms;
    }
    void stepCpu(uint64_t iowait, uint64_t total) {
        cpu_iowait += iowait;
        cpu_total += total;
    }
    DiskIoMonitor::SampleProvider provider() {
        return [this](DiskIoRawSample &out) {
            at_ms += 5000;
            out.at_ms = at_ms;
            out.disks = disks;
            out.has_cpu = true;
            out.cpu_iowait = cpu_iowait;
            out.cpu_total = cpu_total;
            out.has_psi = has_psi;
            out.psi_full_avg10 = psi_full;
            return true;
        };
    }
    std::unordered_map<std::string, DiskIoCounters> disks;
    int64_t at_ms = 0;
    uint64_t cpu_iowait = 0;
    uint64_t cpu_total = 0;
    bool has_psi = false;
    float psi_full = 0.0f;
};

DiskIoConfig testConfig() {
    DiskIoConfig cfg;
    cfg.sustain_samples = 3;
    cfg.alert_repeat_sec = 0;
    return cfg;
}

DiskIoMonitor::Ptr makeIoMonitor(FakeIo &io, const std::vector<DiskIoTarget> &targets, const DiskIoConfig &cfg,
                                 const DiskIoMonitor::DeviceResolver &resolver = DiskIoMonitor::DeviceResolver()) {
    DiskIoMonitor::Providers providers;
    providers.sample = io.provider();
    providers.targets = [targets]() { return targets; };
    providers.config = [cfg]() { return cfg; };
    providers.resolver = resolver ? resolver : [](const std::string &device) {
        return device.substr(device.rfind('/') + 1);
    };
    return std::make_shared<DiskIoMonitor>(EventPollerPool::Instance().getPoller(), providers);
}

DiskIoTarget target(const std::string &mount, const std::string &device, bool main = false) {
    DiskIoTarget t;
    t.mount_point = mount;
    t.device = device;
    t.is_main_storage = main;
    return t;
}

bool waitFor(const std::function<bool()> &pred, int timeout_ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

} // namespace

TEST(DiskIoMonitorParseTest, ParsesDiskStatsOfOldAndNewKernels) {
    // Kernel 3.10 has 14 fields per line; newer kernels append discard/flush counters.
    const std::string text =
        "   8      16 sdb 100 5 2000 300 400 6 8000 900 2 1500 1200\n"
        " 253       0 dm-0 1 0 8 2 3 0 24 4 0 5 6 7 8 9 10 11 12\n"
        "   7       0 loop0 0 0\n";
    std::unordered_map<std::string, DiskIoCounters> out;
    ASSERT_TRUE(DiskIoMonitor::parseDiskStats(text, out));
    ASSERT_EQ(2U, out.size());
    const auto &sdb = out["sdb"];
    EXPECT_EQ(100U, sdb.reads);
    EXPECT_EQ(2000U, sdb.read_sectors);
    EXPECT_EQ(300U, sdb.read_ms);
    EXPECT_EQ(400U, sdb.writes);
    EXPECT_EQ(8000U, sdb.write_sectors);
    EXPECT_EQ(900U, sdb.write_ms);
    EXPECT_EQ(1500U, sdb.io_ticks);
    EXPECT_EQ(1200U, sdb.weighted_ms);
    EXPECT_EQ(5U, out["dm-0"].io_ticks);
}

TEST(DiskIoMonitorParseTest, ParsesCpuStatAndPressure) {
    uint64_t iowait = 0, total = 0;
    ASSERT_TRUE(DiskIoMonitor::parseCpuStat("cpu  10 20 30 400 50 6 7 8 99 99\ncpu0 1 2 3 4 5 6 7 8\n", iowait, total));
    EXPECT_EQ(50U, iowait);
    EXPECT_EQ(531U, total); // guest columns are already inside user/nice
    EXPECT_FALSE(DiskIoMonitor::parseCpuStat("intr 1 2 3\n", iowait, total));

    float some = 0, full = 0;
    ASSERT_TRUE(DiskIoMonitor::parsePressure(
        "some avg10=12.50 avg60=1.00 avg300=0.00 total=1\nfull avg10=7.25 avg60=0.50 avg300=0.00 total=1\n", some, full));
    EXPECT_FLOAT_EQ(12.5f, some);
    EXPECT_FLOAT_EQ(7.25f, full);
    EXPECT_FALSE(DiskIoMonitor::parsePressure("", some, full));
}

TEST(DiskIoMonitorParseTest, FindsParentDiskOfPartitionNames) {
    EXPECT_EQ("sdb", DiskIoMonitor::parentDiskName("sdb1"));
    EXPECT_EQ("nvme0n1", DiskIoMonitor::parentDiskName("nvme0n1p2"));
    EXPECT_EQ("mmcblk0", DiskIoMonitor::parentDiskName("mmcblk0p1"));
    EXPECT_EQ("", DiskIoMonitor::parentDiskName("sdb"));
    EXPECT_EQ("", DiskIoMonitor::parentDiskName("dm-0"));
    EXPECT_EQ("", DiskIoMonitor::parentDiskName("nvme0n1"));
    EXPECT_EQ("", DiskIoMonitor::parentDiskName("md127"));
    EXPECT_EQ("", DiskIoMonitor::resolveKernelName("server:/export"));
}

TEST(DiskIoMonitorTest, ComputesUtilAwaitThroughputAndJson) {
    FakeIo io;
    io.step("sdb1", 0, 0, 0);
    auto monitor = makeIoMonitor(io, {target("/data", "/dev/sdb1", true)}, testConfig());
    ASSERT_TRUE(monitor->sampleNow());
    auto first = monitor->getSnapshot();
    ASSERT_EQ(1U, first.mounts.size());
    EXPECT_FALSE(first.mounts[0].valid) << "one sample has no delta";

    // 4500 ms busy of 5000 ms; 100 writes took 3000 ms; 10240 sectors = 5 MiB.
    io.step("sdb1", 4500, 100, 3000, 10240);
    ASSERT_TRUE(monitor->sampleNow());
    auto snap = monitor->getSnapshot();
    ASSERT_EQ(1U, snap.mounts.size());
    const auto &m = snap.mounts[0];
    ASSERT_TRUE(m.valid);
    EXPECT_EQ("sdb1", m.kernel_name);
    EXPECT_TRUE(m.is_main_storage);
    EXPECT_FLOAT_EQ(90.0f, m.util_pct);
    EXPECT_FLOAT_EQ(30.0f, m.await_ms);
    EXPECT_FLOAT_EQ(0.6f, m.queue_depth);
    EXPECT_FLOAT_EQ(1.0f, m.write_mbps);
    EXPECT_FLOAT_EQ(0.0f, m.read_mbps);

    Json::Value disk;
    m.appendJson(disk);
    EXPECT_EQ("90.00", disk["io_util_pct"].asString());
    EXPECT_EQ("30.00", disk["await_ms"].asString());
    EXPECT_EQ("normal", disk["io_state"].asString()) << "one congested sample is not sustained";
    EXPECT_TRUE(disk["is_main_storage"].asBool());
}

TEST(DiskIoMonitorTest, AlertsOnlyAfterSustainedLevelAndReportsRecovery) {
    IoAlertRecorder recorder;
    FakeIo io;
    io.step("sdb", 0, 0, 0);
    auto monitor = makeIoMonitor(io, {target("/data", "/dev/sdb", true)}, testConfig());
    monitor->sampleNow();

    // Busy but fast: util 90% >= 80 is a warning; await 10 ms below 50.
    for (int i = 0; i < 2; ++i) {
        io.step("sdb", 4500, 100, 1000);
        monitor->sampleNow();
    }
    EXPECT_TRUE(recorder.alerts.empty()) << "two samples are below sustain_samples=3";
    io.step("sdb", 0, 0, 0); // one quiet sample resets the streak
    monitor->sampleNow();
    for (int i = 0; i < 2; ++i) {
        io.step("sdb", 4500, 100, 1000);
        monitor->sampleNow();
    }
    EXPECT_TRUE(recorder.alerts.empty());
    io.step("sdb", 4500, 100, 1000);
    monitor->sampleNow();
    ASSERT_EQ(1U, recorder.alerts.size());
    EXPECT_EQ(1, recorder.alerts[0].level);
    EXPECT_EQ("util", recorder.alerts[0].metric);
    EXPECT_EQ("/data", recorder.alerts[0].mount_point);
    EXPECT_EQ("/dev/sdb", recorder.alerts[0].device);
    EXPECT_TRUE(recorder.alerts[0].is_main_storage);
    EXPECT_FLOAT_EQ(80.0f, recorder.alerts[0].threshold);
    EXPECT_EQ(DiskIoLevel::WARNING, monitor->getSnapshot().mounts[0].state);

    // Still congested: no repeat when alert_repeat_sec = 0.
    io.step("sdb", 4500, 100, 1000);
    monitor->sampleNow();
    EXPECT_EQ(1U, recorder.alerts.size());

    for (int i = 0; i < 3; ++i) {
        io.step("sdb", 100, 10, 20);
        monitor->sampleNow();
    }
    ASSERT_EQ(2U, recorder.alerts.size());
    EXPECT_EQ(0, recorder.alerts[1].level) << "recovery event";
    EXPECT_EQ("util", recorder.alerts[1].metric);
}

TEST(DiskIoMonitorTest, CriticalNeedsUtilAndAwaitTogether) {
    IoAlertRecorder recorder;
    FakeIo io;
    io.step("nvme0n1", 0, 0, 0);
    auto monitor = makeIoMonitor(io, {target("/rec", "/dev/nvme0n1")}, testConfig());
    monitor->sampleNow();

    // util 100% but each IO takes 1 ms: parallel queues, not congestion -> warning only.
    for (int i = 0; i < 3; ++i) {
        io.step("nvme0n1", 5000, 1000, 1000);
        monitor->sampleNow();
    }
    ASSERT_EQ(1U, recorder.alerts.size());
    EXPECT_EQ(1, recorder.alerts[0].level);

    // util 100% and 300 ms per IO -> critical, reported by await.
    for (int i = 0; i < 3; ++i) {
        io.step("nvme0n1", 5000, 100, 30000);
        monitor->sampleNow();
    }
    ASSERT_EQ(2U, recorder.alerts.size());
    EXPECT_EQ(2, recorder.alerts[1].level);
    EXPECT_EQ("await", recorder.alerts[1].metric);
    EXPECT_FLOAT_EQ(300.0f, recorder.alerts[1].value);
    EXPECT_FLOAT_EQ(200.0f, recorder.alerts[1].threshold);
}

TEST(DiskIoMonitorTest, RepeatsWhileCongested) {
    IoAlertRecorder recorder;
    FakeIo io;
    io.step("sdb", 0, 0, 0);
    auto cfg = testConfig();
    cfg.sustain_samples = 1;
    cfg.alert_repeat_sec = 10; // two 5 s samples
    auto monitor = makeIoMonitor(io, {target("/data", "/dev/sdb")}, cfg);
    monitor->sampleNow();
    for (int i = 0; i < 5; ++i) {
        io.step("sdb", 4500, 100, 1000);
        monitor->sampleNow();
    }
    // Samples at 10,15,20,25,30 s: first alert at 10, repeats at 20 and 30.
    EXPECT_EQ(3U, recorder.alerts.size());
}

TEST(DiskIoMonitorTest, MapsDeviceMapperAndFallsBackToWholeDisk) {
    FakeIo io;
    io.step("dm-0", 0, 0, 0);
    io.step("sdc", 0, 0, 0); // kernel without per-partition rows
    auto resolver = [](const std::string &device) -> std::string {
        if (device == "/dev/mapper/vg-data") return "dm-0";
        return device.substr(device.rfind('/') + 1);
    };
    auto monitor = makeIoMonitor(io, {target("/data", "/dev/mapper/vg-data"), target("/backup", "/dev/sdc1"),
                                      target("/gone", "/dev/sdz1")},
                                 testConfig(), resolver);
    monitor->sampleNow();
    io.step("dm-0", 2500, 10, 10);
    io.step("sdc", 1000, 10, 10);
    monitor->sampleNow();
    auto snap = monitor->getSnapshot();
    ASSERT_EQ(3U, snap.mounts.size());
    EXPECT_EQ("dm-0", snap.findMount("/data")->kernel_name);
    EXPECT_FLOAT_EQ(50.0f, snap.findMount("/data")->util_pct);
    EXPECT_EQ("sdc", snap.findMount("/backup")->kernel_name);
    EXPECT_FLOAT_EQ(20.0f, snap.findMount("/backup")->util_pct);
    EXPECT_FALSE(snap.findMount("/gone")->valid);
}

TEST(DiskIoMonitorTest, CounterResetSkipsTheSampleInsteadOfSpiking) {
    IoAlertRecorder recorder;
    FakeIo io;
    io.step("sdb", 1000000, 1000000, 1000000);
    auto cfg = testConfig();
    cfg.sustain_samples = 1;
    auto monitor = makeIoMonitor(io, {target("/data", "/dev/sdb")}, cfg);
    monitor->sampleNow();
    io.disks["sdb"] = DiskIoCounters(); // device re-created: counters back at zero
    monitor->sampleNow();
    EXPECT_FALSE(monitor->getSnapshot().mounts[0].valid);
    EXPECT_TRUE(recorder.alerts.empty());
}

TEST(DiskIoMonitorTest, HostLevelUsesIowaitWithoutPsiAndPsiWhenPresent) {
    IoAlertRecorder recorder;
    FakeIo io;
    auto cfg = testConfig();
    cfg.sustain_samples = 1;
    auto monitor = makeIoMonitor(io, {}, cfg);
    monitor->sampleNow();
    io.stepCpu(45, 100); // 45% iowait >= 40 critical
    monitor->sampleNow();
    ASSERT_EQ(1U, recorder.alerts.size());
    EXPECT_EQ("iowait", recorder.alerts[0].metric);
    EXPECT_EQ("", recorder.alerts[0].mount_point);
    EXPECT_EQ(2, recorder.alerts[0].level);
    auto json = monitor->getSnapshot().systemJson();
    EXPECT_EQ("45.00", json["iowait_pct"].asString());
    EXPECT_TRUE(json["psi_io_full_pct"].isNull());
    EXPECT_EQ("critical", json["state"].asString());

    // PSI takes over: full avg10 5% is normal even though iowait stays high.
    io.has_psi = true;
    io.psi_full = 5.0f;
    io.stepCpu(45, 100);
    monitor->sampleNow();
    ASSERT_EQ(2U, recorder.alerts.size());
    EXPECT_EQ(0, recorder.alerts[1].level);
    EXPECT_EQ("psi", recorder.alerts[1].metric);
}

TEST(HddMonitorTest, CapacityAlertFiresOnLevelChangeNotEveryRefresh) {
    UsageAlertRecorder recorder;
    float usage = 95.0f;
    auto provider = [&usage](std::vector<DiskPartition> &out) {
        DiskPartition p;
        p.device = "/dev/sdb1";
        p.mount_point = "/data";
        p.usage_pct = usage;
        out.push_back(p);
        DiskPartition os;
        os.device = "/dev/sda1";
        os.mount_point = "/";
        os.usage_pct = 10.0f;
        out.push_back(os);
        return true;
    };
    auto monitor = std::make_shared<HddMonitor>(EventPollerPool::Instance().getPoller(), 10.0f, provider);
    monitor->setThreshold(80.0f, 90.0f);
    monitor->setAlertRepeatSec(0);

    ASSERT_TRUE(monitor->refreshNow());
    ASSERT_EQ(1U, recorder.alerts.size());
    EXPECT_EQ("/data", recorder.alerts[0].mount_point);
    EXPECT_EQ("/dev/sdb1", recorder.alerts[0].device);
    EXPECT_TRUE(recorder.alerts[0].is_critical);
    EXPECT_FLOAT_EQ(90.0f, recorder.alerts[0].threshold);

    ASSERT_TRUE(monitor->refreshNow());
    EXPECT_EQ(1U, recorder.alerts.size()) << "same level must not re-alert every refresh";

    usage = 85.0f;
    ASSERT_TRUE(monitor->refreshNow());
    ASSERT_EQ(2U, recorder.alerts.size());
    EXPECT_FALSE(recorder.alerts[1].is_critical);

    usage = 50.0f;
    ASSERT_TRUE(monitor->refreshNow());
    usage = 85.0f;
    ASSERT_TRUE(monitor->refreshNow());
    EXPECT_EQ(3U, recorder.alerts.size()) << "back above the threshold after normal alerts again";
    EXPECT_EQ(0, recorder.system_alerts) << "capacity no longer goes through the mount-less system alert";
}

TEST(StorageWriteMonitorTest, AlertsOnReadOnlyAndWriteFailureAndRecovery) {
    WriteAlertRecorder recorder;
    int next_error = 0;
    int probes = 0;
    auto probe = [&](const std::string &) {
        ++probes;
        return next_error;
    };
    auto monitor = std::make_shared<StorageWriteMonitor>(
        EventPollerPool::Instance().getPoller(), 10.0f,
        []() { return std::vector<WriteCheckTarget>(); }, probe, nullptr, []() { return 0; });
    WriteCheckTarget t;
    t.path = "/data/record";
    t.mount_point = "/data";
    t.device = "/dev/sdb1";
    t.is_main_storage = true;

    monitor->checkTarget(t);
    EXPECT_TRUE(recorder.snapshot().empty()) << "writable at start is not an event";

    next_error = EROFS;
    monitor->checkTarget(t);
    monitor->checkTarget(t);
    auto alerts = recorder.snapshot();
    ASSERT_EQ(1U, alerts.size());
    EXPECT_EQ(1, alerts[0].state);
    EXPECT_EQ(EROFS, alerts[0].error);
    EXPECT_EQ("/data/record", alerts[0].path);
    EXPECT_EQ("/data", alerts[0].mount_point);
    EXPECT_TRUE(alerts[0].is_main_storage);

    next_error = EIO;
    monitor->checkTarget(t);
    alerts = recorder.snapshot();
    ASSERT_EQ(2U, alerts.size());
    EXPECT_EQ(2, alerts[1].state);

    next_error = 0;
    monitor->checkTarget(t);
    alerts = recorder.snapshot();
    ASSERT_EQ(3U, alerts.size());
    EXPECT_EQ(0, alerts[2].state);

    // A ro mount is reported without writing to it.
    const int before = probes;
    t.read_only = true;
    monitor->checkTarget(t);
    EXPECT_EQ(before, probes);
    alerts = recorder.snapshot();
    ASSERT_EQ(4U, alerts.size());
    EXPECT_EQ(1, alerts[3].state);

    auto status = monitor->getStatus();
    ASSERT_EQ(1U, status.size());
    EXPECT_EQ("read_only", status[0].toJson()["state"].asString());
}

TEST(StorageWriteMonitorTest, BrokenAtStartIsReported) {
    WriteAlertRecorder recorder;
    auto monitor = std::make_shared<StorageWriteMonitor>(
        EventPollerPool::Instance().getPoller(), 10.0f,
        []() { return std::vector<WriteCheckTarget>(); },
        [](const std::string &) { return EACCES; }, nullptr, []() { return 0; });
    WriteCheckTarget t;
    t.path = "/db";
    monitor->checkTarget(t);
    auto alerts = recorder.snapshot();
    ASSERT_EQ(1U, alerts.size());
    EXPECT_EQ(2, alerts[0].state);
    EXPECT_EQ(EACCES, alerts[0].error);
}

TEST(StorageWriteMonitorTest, ProbeWritesIntoNearestExistingDirAndLeavesNothing) {
    char tmpl[] = "/tmp/s3mk_write_probe_XXXXXX";
    char *dir = mkdtemp(tmpl);
    ASSERT_NE(nullptr, dir);
    const std::string base = dir;
    EXPECT_EQ(0, StorageWriteMonitor::probeWritable(base));
    // A record directory not created yet is tested through its parent and is not created.
    EXPECT_EQ(0, StorageWriteMonitor::probeWritable(base + "/record/live"));
    struct stat st{};
    EXPECT_NE(0, ::stat((base + "/record").c_str(), &st));
    EXPECT_NE(0, ::stat((base + "/.s3mk_write_probe").c_str(), &st));
    ::rmdir(base.c_str());
}

TEST(StorageWriteMonitorTest, HungProbeHoldsOneThreadAndNeverThePoller) {
    // Shared, because probes queued on the executor may still run after the test body returns.
    struct ProbeGate {
        std::mutex mtx;
        std::condition_variable cv;
        bool release = false;
        std::atomic<int> probes{0};
    };
    auto gate = std::make_shared<ProbeGate>();
    auto probe = [gate](const std::string &) {
        ++gate->probes;
        std::unique_lock<std::mutex> lck(gate->mtx);
        gate->cv.wait(lck, [gate]() { return gate->release; });
        return 0;
    };
    auto poller = EventPollerPool::Instance().getPoller();
    // Several threads like BlockingExecutors::api(): a duplicate probe would run, not just queue.
    auto executor = std::make_shared<ThreadPool>(4, ThreadPool::PRIORITY_NORMAL, true, false, "probe test");
    WriteCheckTarget t;
    t.path = "/nas/pool";
    auto monitor = std::make_shared<StorageWriteMonitor>(
        poller, 0.05f, [t]() { return std::vector<WriteCheckTarget>{t}; }, probe, executor, []() { return 0; });
    monitor->start();

    ASSERT_TRUE(waitFor([gate]() { return gate->probes.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // several timer ticks
    EXPECT_EQ(1, gate->probes.load()) << "a probe still running must not be dispatched again";

    auto done = std::make_shared<std::atomic<bool>>(false);
    poller->async([done]() { *done = true; }, false);
    EXPECT_TRUE(waitFor([done]() { return done->load(); }, 500)) << "timer poller blocked by the probe";

    {
        std::lock_guard<std::mutex> lck(gate->mtx);
        gate->release = true;
    }
    gate->cv.notify_all();
    EXPECT_TRUE(waitFor([gate]() { return gate->probes.load() >= 2; })) << "probing resumes after the hung one returns";
    monitor.reset();
}
