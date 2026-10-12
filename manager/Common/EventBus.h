#ifndef COMMON_EVENTBUS_H
#define COMMON_EVENTBUS_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Poller/EventPoller.h"
#include "Common/EventRecord.h"
#include "Common/EventSpool.h"
#include "Common/KafkaProducer.h"
#include "Common/SerialTaskQueue.h"

namespace KafkaCfg {
extern const std::string kCompression;
extern const std::string kLingerMs;
extern const std::string kQueueMaxKBytes;
extern const std::string kMessageTimeoutMs;
extern const std::string kSpoolAfterSec;         // no broker UP this long -> SPOOL
extern const std::string kSpoolDir;
extern const std::string kSpoolMaxMB;
extern const std::string kSpoolMaxAgeHours;
extern const std::string kSpoolMinFreeMB;
extern const std::string kSpoolSegmentMB;
extern const std::string kSpoolFsyncMs;
extern const std::string kSpoolMemQueue;         // records waiting to be written to the spool
extern const std::string kSpoolReplayInFlight;
extern const std::string kSpoolReplayRate;       // records/s
extern const std::string kSpoolHighPercent;
extern const std::string kFlushTimeoutMs;
extern const std::string kMaxEventsPerCameraPerMin;
extern const std::string kAlertRepeatSec;
extern const std::string kStatsIntervalSec;
} // namespace KafkaCfg

namespace managerkit {

/**
 * Sends event records (device events) to Kafka; while the broker cannot take them they go to a
 * disk spool and are replayed in order afterwards.
 *
 *   LIVE   publish() produces straight into librdkafka
 *   SPOOL  publish() appends to the spool; nothing is sent
 *   DRAIN  the spool is replayed (replayed=1); publish() still appends behind it
 *
 * LIVE -> SPOOL: no broker UP for kafka.spoolAfterSec, a full librdkafka queue, a failed delivery
 * (the record and everything still queued go to the spool) or a fatal producer error.
 * SPOOL -> DRAIN: a broker is UP again and the retry backoff has passed. DRAIN -> LIVE: spool read
 * to the end, nothing in flight, nothing waiting to be written (checked under the same lock as
 * publish(), so no record slips between the two paths).
 *
 * Threads: publish() on any thread (camera pollers), never blocks. A 100 ms timer on one poller
 * serves librdkafka callbacks and decides mode changes. Spool IO, producer creation and teardown
 * run in one SerialTaskQueue on BlockingExecutors::api(). Alerts go out as kBroadcastEventBusAlert.
 * Built without ENABLE_KAFKA: enabled() is false and publish() drops.
 */
class EventBus : public std::enable_shared_from_this<EventBus> {
public:
    using Ptr = std::shared_ptr<EventBus>;
    enum class Mode { Off, Live, Spool, Drain };

    struct Stats {
        Mode mode = Mode::Off;
        int brokersUp = -1;
        int brokersKnown = 0;
        int queued = 0;            // in librdkafka
        uint64_t produced = 0;     // live
        uint64_t delivered = 0;    // live + replayed
        uint64_t spooled = 0;      // records written to the spool
        uint64_t replayed = 0;     // delivered from the spool
        uint64_t spoolPending = 0;
        uint64_t spoolBytes = 0;
        uint64_t droppedRate = 0;  // over kafka.maxEventsPerCameraPerMin
        uint64_t droppedSpool = 0; // lost to the spool limits (size, age, disk)
        uint64_t droppedMem = 0;   // spool write queue full
        uint64_t skippedInitialized = 0;
        uint64_t resynced = 0;
    };

    // The node's bus: never destroyed (callbacks may still run while statics go away at exit).
    static EventBus &Instance();
    // A separate bus (tests). `tick_ms` is the poll timer period.
    static Ptr create(int tick_ms = 100);

    // Connection settings, from the Service API. `enable` false or no brokers/topic: no sending
    // and no spooling (the spool on disk is kept). Unchanged settings are ignored. Any thread.
    void apply(bool enable, const KafkaSettings &settings);

    // Any thread, never blocks. False when the record is not taken (bus off, rate limit,
    // spool write queue full); `rate_key` (device id) selects the per-minute limit.
    bool publish(const EventRecord &record, const std::string &rate_key);

    bool enabled() const;
    // Random per process, part of event ids.
    const std::string &bootId() const { return _boot_id; }
    uint64_t nextSeq() { return ++_seq; }
    // OnvifEventHub statistics shown on the bus line.
    void noteInitialized(bool resynced);

    Mode mode() const;
    Stats stats() const;
    static const char *modeName(Mode mode);

    // Sends or spools what is queued, writes the spool and its checkpoint. Blocks up to
    // kafka.flushTimeoutMs plus the disk writes. publish() afterwards only queues in memory.
    void shutdown();

    // Tests: waits until the spool queue has run everything posted so far.
    void waitIdle();

private:
    explicit EventBus(int tick_ms);
    void start();

#if defined(ENABLE_KAFKA)
    struct ReplayEntry {
        uint64_t id;
        EventSpool::Position next; // position after this record
        bool done;
    };

    void onTick();
    void onDelivery(uint64_t id, int error, KafkaProducer::ErrorKind kind, const std::string &reason,
                    const EventRecord &record);
    void onProducerError(int error, KafkaProducer::ErrorKind kind, bool fatal, const std::string &reason);

    // _mtx held.
    void enterSpoolLocked(KafkaProducer::ErrorKind kind, const std::string &error);
    bool queueForSpoolLocked(const EventRecord &record);
    void scheduleWriteLocked();

    // Spool queue tasks.
    void applyTask(bool enable, const KafkaSettings &settings);
    bool openSpool();
    void writeQueued();
    void drainStep();
    void commitReplayed();
    void maintainSpool();
    void post(const std::function<void(EventBus &)> &task);

    void emitAlert(const std::string &code, const std::string &reason, const std::string &last_error,
                   int64_t current, int64_t threshold, int64_t dropped, int64_t replayed, int64_t duration_sec);
    void spoolDropped(uint64_t records, const char *reason);
    void logStats();
#endif

private:
    const int _tick_ms;
    std::string _boot_id;
    std::atomic<uint64_t> _seq { 0 };
    std::atomic<uint64_t> _skipped_initialized { 0 };
    std::atomic<uint64_t> _resynced { 0 };

#if defined(ENABLE_KAFKA)
    mutable std::mutex _mtx;
    Mode _mode = Mode::Off;
    bool _stopping = false;
    bool _enabled = false;
    KafkaSettings _settings;
    KafkaProducer::Ptr _producer;
    std::deque<EventRecord> _write_queue; // waiting for the spool
    bool _write_scheduled = false;

    // Rate limit per camera and minute.
    struct RateBucket {
        uint64_t minute = 0;
        int count = 0;
    };
    std::unordered_map<std::string, RateBucket> _rate;

    // Replay window (DRAIN): in send order.
    std::deque<ReplayEntry> _replay;
    uint64_t _next_replay_id = 0;
    EventSpool::Position _replay_pos;
    bool _replay_reset = true;  // next drain step starts at the checkpoint
    bool _drain_scheduled = false;
    bool _commit_scheduled = false;
    uint64_t _replay_budget_sec = 0;
    uint64_t _replay_budget = 0;

    // Failure / alert bookkeeping (_mtx).
    KafkaProducer::ErrorKind _spool_reason = KafkaProducer::ErrorKind::None;
    std::string _last_error;
    uint64_t _spool_since_ms = 0;
    uint64_t _next_retry_ms = 0;
    uint64_t _retry_backoff_ms = 0;
    uint64_t _no_broker_since_ms = 0;
    uint64_t _last_unavailable_alert_ms = 0;
    bool _unavailable_alerted = false;
    bool _spool_high_alerted = false;
    bool _spool_full_alerted = false;
    uint64_t _replayed_episode = 0;
    bool _recovered_pending = false;
    bool _recreate_scheduled = false;

    std::unique_ptr<EventSpool> _spool; // spool queue only
    bool _spool_open_failed = false;
    std::atomic<bool> _spool_ready { false }; // _spool opened (read by the timer)
    std::atomic<uint64_t> _spool_bytes { 0 };
    std::atomic<uint64_t> _spool_pending { 0 };
    uint64_t _last_maintain_ms = 0;
    uint64_t _last_stats_ms = 0;

    std::atomic<uint64_t> _produced { 0 };
    std::atomic<uint64_t> _delivered { 0 };
    std::atomic<uint64_t> _spooled { 0 };
    std::atomic<uint64_t> _replayed { 0 };
    std::atomic<uint64_t> _dropped_rate { 0 };
    std::atomic<uint64_t> _dropped_spool { 0 };
    std::atomic<uint64_t> _dropped_mem { 0 };

    SerialTaskQueue::Ptr _queue;
    toolkit::EventPoller::Ptr _poller;
    toolkit::EventPoller::DelayTask::Ptr _timer;
#endif
};

} // namespace managerkit

#endif // COMMON_EVENTBUS_H
