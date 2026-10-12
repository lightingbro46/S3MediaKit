#include <algorithm>
#include <future>
#include <random>
#include "Util/logger.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Util/util.h"
#include "Poller/EventPoller.h"
#include "Common/config.h"
#include "Common/BlockingExecutors.h"
#include "Common/EventBus.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace KafkaCfg {
#define KAFKA_FIELD "kafka."
const string kCompression = KAFKA_FIELD "compression";
const string kLingerMs = KAFKA_FIELD "lingerMs";
const string kQueueMaxKBytes = KAFKA_FIELD "queueMaxKBytes";
const string kMessageTimeoutMs = KAFKA_FIELD "messageTimeoutMs";
const string kSpoolAfterSec = KAFKA_FIELD "spoolAfterSec";
const string kSpoolDir = KAFKA_FIELD "spoolDir";
const string kSpoolMaxMB = KAFKA_FIELD "spoolMaxMB";
const string kSpoolMaxAgeHours = KAFKA_FIELD "spoolMaxAgeHours";
const string kSpoolMinFreeMB = KAFKA_FIELD "spoolMinFreeMB";
const string kSpoolSegmentMB = KAFKA_FIELD "spoolSegmentMB";
const string kSpoolFsyncMs = KAFKA_FIELD "spoolFsyncMs";
const string kSpoolMemQueue = KAFKA_FIELD "spoolMemQueue";
const string kSpoolReplayInFlight = KAFKA_FIELD "spoolReplayInFlight";
const string kSpoolReplayRate = KAFKA_FIELD "spoolReplayRate";
const string kSpoolHighPercent = KAFKA_FIELD "spoolHighPercent";
const string kFlushTimeoutMs = KAFKA_FIELD "flushTimeoutMs";
const string kMaxEventsPerCameraPerMin = KAFKA_FIELD "maxEventsPerCameraPerMin";
const string kAlertRepeatSec = KAFKA_FIELD "alertRepeatSec";
const string kStatsIntervalSec = KAFKA_FIELD "statsIntervalSec";

static onceToken token([]() {
    mINI::Instance()[kCompression] = "lz4";
    mINI::Instance()[kLingerMs] = 50;
    mINI::Instance()[kQueueMaxKBytes] = 16384;
    // A record stuck in librdkafka this long fails and goes to the spool.
    mINI::Instance()[kMessageTimeoutMs] = 60000;
    mINI::Instance()[kSpoolAfterSec] = 15;
    // Next to ./db, not on a recording disk.
    mINI::Instance()[kSpoolDir] = "./eventbus_spool";
    mINI::Instance()[kSpoolMaxMB] = 1024;
    mINI::Instance()[kSpoolMaxAgeHours] = 72;
    mINI::Instance()[kSpoolMinFreeMB] = 512;
    mINI::Instance()[kSpoolSegmentMB] = 16;
    mINI::Instance()[kSpoolFsyncMs] = 1000;
    mINI::Instance()[kSpoolMemQueue] = 20000;
    mINI::Instance()[kSpoolReplayInFlight] = 1000;
    // After a long outage, do not flood the consumer.
    mINI::Instance()[kSpoolReplayRate] = 2000;
    mINI::Instance()[kSpoolHighPercent] = 80;
    mINI::Instance()[kFlushTimeoutMs] = 3000;
    mINI::Instance()[kMaxEventsPerCameraPerMin] = 600;
    mINI::Instance()[kAlertRepeatSec] = 1800;
    mINI::Instance()[kStatsIntervalSec] = 60;
});
} // namespace KafkaCfg

namespace managerkit {

static string randomBootId() {
    std::random_device rd;
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rd()));
    return buf;
}

EventBus &EventBus::Instance() {
    static Ptr *s_bus = new Ptr(create());
    return **s_bus;
}

EventBus::Ptr EventBus::create(int tick_ms) {
    Ptr ret(new EventBus(tick_ms));
    ret->start();
    return ret;
}

EventBus::EventBus(int tick_ms) : _tick_ms(tick_ms > 0 ? tick_ms : 100), _boot_id(randomBootId()) {}

const char *EventBus::modeName(Mode mode) {
    switch (mode) {
        case Mode::Live: return "LIVE";
        case Mode::Spool: return "SPOOL";
        case Mode::Drain: return "DRAIN";
        default: return "OFF";
    }
}

void EventBus::noteInitialized(bool resynced) {
    if (resynced) {
        ++_resynced;
    } else {
        ++_skipped_initialized;
    }
}

#if !defined(ENABLE_KAFKA)

void EventBus::start() {}
void EventBus::apply(bool enable, const KafkaSettings &) {
    if (enable) {
        static once_flag s_once;
        call_once(s_once, []() { WarnL << "EventBus: built without ENABLE_KAFKA, device events are not sent"; });
    }
}
bool EventBus::publish(const EventRecord &, const string &) { return false; }
bool EventBus::enabled() const { return false; }
EventBus::Mode EventBus::mode() const { return Mode::Off; }
EventBus::Stats EventBus::stats() const {
    Stats ret;
    ret.skippedInitialized = _skipped_initialized;
    ret.resynced = _resynced;
    return ret;
}
void EventBus::shutdown() {}
void EventBus::waitIdle() {}

#else

static uint64_t nowMs() {
    return getCurrentMillisecond();
}

void EventBus::start() {
    _queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "event bus");
    _poller = EventPollerPool::Instance().getPoller();
    std::weak_ptr<EventBus> weak_self = shared_from_this();
    const uint64_t tick = static_cast<uint64_t>(_tick_ms);
    _timer = _poller->doDelayTask(tick, [weak_self, tick]() -> uint64_t {
        auto self = weak_self.lock();
        if (!self) {
            return 0;
        }
        self->onTick();
        return tick;
    });
}

void EventBus::post(const function<void(EventBus &)> &task) {
    std::weak_ptr<EventBus> weak_self = shared_from_this();
    _queue->post([weak_self, task]() {
        if (auto self = weak_self.lock()) {
            task(*self);
        }
    });
}

void EventBus::waitIdle() {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    _queue->post([done]() { done->set_value(); });
    fut.wait_for(std::chrono::seconds(30));
}

bool EventBus::enabled() const {
    lock_guard<mutex> lck(_mtx);
    return _enabled;
}

EventBus::Mode EventBus::mode() const {
    lock_guard<mutex> lck(_mtx);
    return _mode;
}

EventBus::Stats EventBus::stats() const {
    Stats ret;
    KafkaProducer::Ptr producer;
    {
        lock_guard<mutex> lck(_mtx);
        ret.mode = _mode;
        producer = _producer;
    }
    if (producer) {
        ret.brokersUp = producer->brokersUp();
        ret.brokersKnown = producer->brokersKnown();
        ret.queued = producer->queued();
    }
    ret.produced = _produced;
    ret.delivered = _delivered;
    ret.spooled = _spooled;
    ret.replayed = _replayed;
    ret.spoolPending = _spool_pending;
    ret.spoolBytes = _spool_bytes;
    ret.droppedRate = _dropped_rate;
    ret.droppedSpool = _dropped_spool;
    ret.droppedMem = _dropped_mem;
    ret.skippedInitialized = _skipped_initialized;
    ret.resynced = _resynced;
    return ret;
}

void EventBus::apply(bool enable, const KafkaSettings &settings) {
    post([enable, settings](EventBus &bus) { bus.applyTask(enable, settings); });
}

bool EventBus::openSpool() {
    if (_spool) {
        return true;
    }
    GET_CONFIG(string, dir, KafkaCfg::kSpoolDir);
    GET_CONFIG(uint64_t, max_mb, KafkaCfg::kSpoolMaxMB);
    GET_CONFIG(uint64_t, segment_mb, KafkaCfg::kSpoolSegmentMB);
    GET_CONFIG(uint64_t, max_age_h, KafkaCfg::kSpoolMaxAgeHours);
    GET_CONFIG(uint64_t, min_free_mb, KafkaCfg::kSpoolMinFreeMB);
    EventSpool::Options opts;
    opts.dir = dir;
    opts.maxBytes = max_mb << 20;
    opts.segmentBytes = segment_mb << 20;
    opts.maxAgeSec = max_age_h * 3600;
    opts.minFreeBytes = min_free_mb << 20;
    unique_ptr<EventSpool> spool(new EventSpool(opts));
    string err;
    if (!spool->open(err)) {
        ErrorL << "EventBus: cannot open the spool, events are dropped while Kafka is unreachable: " << err;
        lock_guard<mutex> lck(_mtx);
        _spool_open_failed = true;
        return false;
    }
    // Synchronous, on the spool queue (append / dropExpired).
    EventBus *self = this;
    spool->setDropCallback([self](uint64_t records, EventSpool::DropReason reason) {
        self->_dropped_spool += records;
        self->spoolDropped(records, reason == EventSpool::DropReason::MaxSize ? "max_size" : "max_age");
    });
    _spool = std::move(spool);
    _spool_ready = true;
    _spool_pending = _spool->stats().pending;
    _spool_bytes = _spool->stats().bytes;
    return true;
}

void EventBus::applyTask(bool enable, const KafkaSettings &settings) {
    const bool valid = enable && !settings.brokers.empty() && !settings.topic.empty();
    {
        lock_guard<mutex> lck(_mtx);
        if (_stopping) {
            return;
        }
        // Unchanged; a fatal producer is replaced even with the same settings.
        if (!_recreate_scheduled && _enabled == valid && (!valid || (_settings == settings && _producer))) {
            return;
        }
    }
    if (enable && !valid) {
        WarnL << "EventBus: Kafka enabled without brokers or topic, device events are not sent";
    }

    KafkaProducer::Ptr old;
    if (!valid) {
        lock_guard<mutex> lck(_mtx);
        old = std::move(_producer);
        _enabled = false;
        _mode = Mode::Off;
        _settings = settings;
        _replay.clear();
        _recreate_scheduled = false;
    } else {
        openSpool();
        string err;
        auto producer = KafkaProducer::create(settings, err);
        if (producer) {
            std::weak_ptr<EventBus> weak_self = shared_from_this();
            producer->setDeliveryCallback([weak_self](uint64_t id, int error, KafkaProducer::ErrorKind kind,
                                                      const string &reason, const EventRecord &record) {
                if (auto self = weak_self.lock()) {
                    self->onDelivery(id, error, kind, reason, record);
                }
            });
            producer->setErrorCallback([weak_self](int error, KafkaProducer::ErrorKind kind, bool fatal, const string &reason) {
                if (auto self = weak_self.lock()) {
                    self->onProducerError(error, kind, fatal, reason);
                }
            });
        } else {
            ErrorL << "EventBus: Kafka producer not created: " << err;
        }
        InfoL << "EventBus: Kafka brokers=" << settings.brokers << " topic=" << settings.topic
              << " protocol=" << settings.securityProtocol << " mechanism=" << settings.saslMechanism
              << " user=" << settings.saslUsername << (producer ? "" : " (rejected)");

        lock_guard<mutex> lck(_mtx);
        old = std::move(_producer);
        _producer = producer;
        _enabled = true;
        _settings = settings;
        _recreate_scheduled = false;
        _no_broker_since_ms = nowMs();
        const bool backlog = _spool_pending > 0 || !_write_queue.empty();
        if (!producer) {
            // Rejected settings: keep the events until a new configuration arrives.
            if (_mode == Mode::Off) {
                _mode = Mode::Live;
            }
            enterSpoolLocked(KafkaProducer::ErrorKind::Other, err);
        } else if (_mode == Mode::Off || _mode == Mode::Live) {
            if (backlog) {
                // Records of an earlier run or outage go first.
                _mode = Mode::Spool;
                _next_retry_ms = 0;
                _replay_reset = true;
                if (!_spool_since_ms) {
                    _spool_since_ms = nowMs();
                    _spool_reason = KafkaProducer::ErrorKind::BrokerDown;
                }
            } else {
                _mode = Mode::Live;
            }
        } else {
            _mode = Mode::Spool;
            _replay.clear();
            _replay_reset = true;
            _next_retry_ms = 0; // new settings: try at once
        }
    }
    if (old) {
        // Its queued records come back through the delivery callback and go to the spool.
        GET_CONFIG(int, flush_ms, KafkaCfg::kFlushTimeoutMs);
        old->purgeQueued();
        old->flush(flush_ms);
        old.reset(); // rd_kafka_destroy: blocking, on this pool
    }
}

bool EventBus::publish(const EventRecord &record, const string &rate_key) {
    GET_CONFIG(int, per_min, KafkaCfg::kMaxEventsPerCameraPerMin);
    KafkaProducer::Ptr producer;
    {
        lock_guard<mutex> lck(_mtx);
        if (!_enabled) {
            return false;
        }
        if (per_min > 0) {
            const uint64_t minute = nowMs() / 60000;
            auto &bucket = _rate[rate_key];
            if (bucket.minute != minute) {
                bucket.minute = minute;
                bucket.count = 0;
            }
            if (bucket.count >= per_min) {
                ++_dropped_rate;
                return false;
            }
            ++bucket.count;
        }
        if (_stopping || _mode != Mode::Live || !_producer) {
            return queueForSpoolLocked(record);
        }
        producer = _producer;
    }
    string err;
    auto rc = producer->produce(record, 0, false, &err);
    if (rc == KafkaProducer::ProduceResult::Ok) {
        ++_produced;
        return true;
    }
    lock_guard<mutex> lck(_mtx);
    enterSpoolLocked(rc == KafkaProducer::ProduceResult::QueueFull ? KafkaProducer::ErrorKind::QueueFull
                                                                   : KafkaProducer::ErrorKind::Other, err);
    return queueForSpoolLocked(record);
}

bool EventBus::queueForSpoolLocked(const EventRecord &record) {
    GET_CONFIG(size_t, mem_max, KafkaCfg::kSpoolMemQueue);
    if (_spool_open_failed) {
        ++_dropped_spool;
        return false;
    }
    if (_write_queue.size() >= std::max<size_t>(mem_max, 1)) {
        // Disk slower than the events: never block the camera poller.
        ++_dropped_mem;
        if (!_spool_full_alerted) {
            _spool_full_alerted = true;
            std::weak_ptr<EventBus> weak_self = shared_from_this();
            _poller->async([weak_self]() {
                if (auto self = weak_self.lock()) {
                    self->emitAlert("EventBusSpoolFull", "memory", "", 0, 0, static_cast<int64_t>(self->_dropped_mem.load()), 0, 0);
                }
            }, false);
        }
        return false;
    }
    _write_queue.push_back(record);
    scheduleWriteLocked();
    return true;
}

void EventBus::scheduleWriteLocked() {
    if (_write_scheduled) {
        return;
    }
    _write_scheduled = true;
    post([](EventBus &bus) { bus.writeQueued(); });
}

void EventBus::enterSpoolLocked(KafkaProducer::ErrorKind kind, const string &error) {
    if (!error.empty()) {
        _last_error = error;
    }
    if (_mode == Mode::Spool || _mode == Mode::Off) {
        return;
    }
    const uint64_t now = nowMs();
    if (_mode == Mode::Live || !_spool_since_ms) {
        _spool_since_ms = now;
        _spool_reason = kind;
    }
    _mode = Mode::Spool;
    _replay.clear();
    _replay_reset = true;
    // First replay as soon as a broker is up; replays failing again back off 5 s .. 5 min.
    _next_retry_ms = now + _retry_backoff_ms;
    _retry_backoff_ms = _retry_backoff_ms ? std::min<uint64_t>(_retry_backoff_ms * 2, 300000) : 5000;
    if (_producer) {
        // Records not sent yet come back through the delivery callback, into the spool.
        _producer->purgeQueued();
    }
    WarnL << "EventBus: Kafka unavailable (" << KafkaProducer::kindName(kind) << "), spooling events"
          << (_last_error.empty() ? "" : ": ") << _last_error;
}

void EventBus::onDelivery(uint64_t id, int error, KafkaProducer::ErrorKind kind, const string &reason,
                          const EventRecord &record) {
    lock_guard<mutex> lck(_mtx);
    if (!id) {
        if (!error) {
            ++_delivered;
            return;
        }
        // Purged too (settings change): once a record is in the spool, later ones go behind it.
        enterSpoolLocked(kind, reason);
        queueForSpoolLocked(record);
        return;
    }
    // Replayed record: still in the spool until committed. Ids grow, so the window is sorted.
    auto it = std::lower_bound(_replay.begin(), _replay.end(), id,
                               [](const ReplayEntry &e, uint64_t v) { return e.id < v; });
    if (it == _replay.end() || it->id != id) {
        return; // window reset since (spool episode, new settings): sent again later
    }
    if (error) {
        if (kind != KafkaProducer::ErrorKind::Purged) {
            enterSpoolLocked(kind, reason);
        }
        return;
    }
    it->done = true;
    ++_delivered;
    ++_replayed;
    ++_replayed_episode;
    if (!_commit_scheduled) {
        _commit_scheduled = true;
        post([](EventBus &bus) { bus.commitReplayed(); });
    }
}

void EventBus::onProducerError(int error, KafkaProducer::ErrorKind kind, bool fatal, const string &reason) {
    lock_guard<mutex> lck(_mtx);
    _last_error = reason;
    if (fatal) {
        enterSpoolLocked(KafkaProducer::ErrorKind::Fatal, reason);
    } else if (kind == KafkaProducer::ErrorKind::Auth || kind == KafkaProducer::ErrorKind::Topic) {
        // Retrying cannot help until the configuration changes; spool now instead of after the
        // message timeout.
        enterSpoolLocked(kind, reason);
    }
}

void EventBus::writeQueued() {
    deque<EventRecord> batch;
    {
        lock_guard<mutex> lck(_mtx);
        batch.swap(_write_queue);
        _write_scheduled = false;
    }
    if (batch.empty()) {
        return;
    }
    if (!_spool && !openSpool()) {
        _dropped_spool += batch.size();
        return;
    }
    vector<EventRecord> records(std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    string err;
    auto rc = _spool->append(records, &err);
    if (rc == EventSpool::AppendResult::Ok) {
        _spooled += records.size();
    } else {
        _dropped_spool += records.size();
        if (rc == EventSpool::AppendResult::IoError) {
            ErrorL << "EventBus: spool write failed, " << records.size() << " events lost: " << err;
        }
        spoolDropped(records.size(), "disk");
    }
    _spool_pending = _spool->stats().pending;
    _spool_bytes = _spool->stats().bytes;
}

void EventBus::spoolDropped(uint64_t records, const char *reason) {
    {
        lock_guard<mutex> lck(_mtx);
        if (_spool_full_alerted) {
            return;
        }
        _spool_full_alerted = true;
    }
    WarnL << "EventBus: spool full (" << reason << "), " << records << " events lost";
    emitAlert("EventBusSpoolFull", reason, "", static_cast<int64_t>(_spool_bytes >> 20), 0,
              static_cast<int64_t>(_dropped_spool.load() + _dropped_mem.load()), 0, 0);
}

void EventBus::drainStep() {
    writeQueued();
    GET_CONFIG(size_t, in_flight_max, KafkaCfg::kSpoolReplayInFlight);
    GET_CONFIG(uint64_t, rate, KafkaCfg::kSpoolReplayRate);
    KafkaProducer::Ptr producer;
    size_t room = 0;
    {
        lock_guard<mutex> lck(_mtx);
        _drain_scheduled = false;
        if (_mode != Mode::Drain || !_producer || !_spool) {
            return;
        }
        producer = _producer;
        if (_replay_reset) {
            _replay.clear();
            _replay_pos = _spool->checkpoint();
            _replay_reset = false;
        }
        const uint64_t sec = nowMs() / 1000;
        if (sec != _replay_budget_sec) {
            _replay_budget_sec = sec;
            _replay_budget = rate ? rate : UINT64_MAX;
        }
        const size_t window = std::max<size_t>(in_flight_max, 1);
        room = _replay.size() >= window ? 0 : window - _replay.size();
        room = static_cast<size_t>(std::min<uint64_t>(room, _replay_budget));
    }
    if (!room) {
        return; // the timer comes back
    }
    vector<pair<EventSpool::Position, EventRecord>> out;
    EventSpool::Position end;
    _spool->read(_replay_pos, room, out, end);

    for (size_t i = 0; i < out.size(); ++i) {
        const EventSpool::Position next = i + 1 < out.size() ? out[i + 1].first : end;
        uint64_t id;
        {
            lock_guard<mutex> lck(_mtx);
            if (_mode != Mode::Drain || _replay_reset) {
                return;
            }
            id = ++_next_replay_id;
            _replay.push_back(ReplayEntry { id, next, false });
            --_replay_budget;
        }
        string err;
        auto rc = producer->produce(out[i].second, id, true, &err);
        lock_guard<mutex> lck(_mtx);
        if (rc != KafkaProducer::ProduceResult::Ok) {
            if (!_replay.empty() && _replay.back().id == id) {
                _replay.pop_back();
            }
            if (rc == KafkaProducer::ProduceResult::QueueFull) {
                _replay_pos = out[i].first; // again at the next step
            } else {
                enterSpoolLocked(KafkaProducer::ErrorKind::Other, err);
            }
            return;
        }
        _replay_pos = next;
    }

    if (out.empty()) {
        lock_guard<mutex> lck(_mtx);
        if (_mode == Mode::Drain && _replay.empty() && _write_queue.empty() && _spool->empty()) {
            const uint64_t now = nowMs();
            InfoL << "EventBus: spool replayed (" << _replayed_episode << " events in "
                  << (now - _spool_since_ms) / 1000 << " s), sending live";
            _mode = Mode::Live;
            if (_unavailable_alerted) {
                _recovered_pending = true;
            } else {
                _replayed_episode = 0;
                _spool_since_ms = 0;
            }
            _retry_backoff_ms = 0;
            _spool_full_alerted = false;
            _spool_high_alerted = false;
        }
    }
}

void EventBus::commitReplayed() {
    EventSpool::Position next;
    uint64_t count = 0;
    {
        lock_guard<mutex> lck(_mtx);
        _commit_scheduled = false;
        while (!_replay.empty() && _replay.front().done) {
            next = _replay.front().next;
            _replay.pop_front();
            ++count;
        }
    }
    if (count && _spool) {
        _spool->commit(next, count);
        _spool_pending = _spool->stats().pending;
        _spool_bytes = _spool->stats().bytes;
    }
}

void EventBus::maintainSpool() {
    writeQueued();
    if (!_spool) {
        return;
    }
    _spool->dropExpired(time(nullptr));
    _spool->sync();
    _spool->flushCheckpoint();
    _spool_pending = _spool->stats().pending;
    _spool_bytes = _spool->stats().bytes;
}

void EventBus::onTick() {
    KafkaProducer::Ptr producer;
    {
        lock_guard<mutex> lck(_mtx);
        if (_stopping) {
            return;
        }
        producer = _producer;
    }
    if (producer) {
        producer->poll(0);
    }

    GET_CONFIG(uint64_t, spool_after_sec, KafkaCfg::kSpoolAfterSec);
    GET_CONFIG(uint64_t, fsync_ms, KafkaCfg::kSpoolFsyncMs);
    GET_CONFIG(uint64_t, repeat_sec, KafkaCfg::kAlertRepeatSec);
    GET_CONFIG(uint64_t, high_pct, KafkaCfg::kSpoolHighPercent);
    GET_CONFIG(uint64_t, max_mb, KafkaCfg::kSpoolMaxMB);
    GET_CONFIG(uint64_t, stats_sec, KafkaCfg::kStatsIntervalSec);
    const uint64_t now = nowMs();

    bool alert_unavailable = false, alert_recovered = false, alert_high = false;
    string reason, last_error;
    int64_t duration = 0, replayed = 0;
    bool spool_work = false;
    {
        lock_guard<mutex> lck(_mtx);
        if (_stopping || !_enabled) {
            return;
        }
        const int up = producer ? producer->brokersUp() : -1;
        if (up > 0) {
            _no_broker_since_ms = 0;
        } else if (!_no_broker_since_ms) {
            _no_broker_since_ms = now;
        }
        const bool fatal = producer && producer->fatal();
        if (fatal && !_recreate_scheduled) {
            enterSpoolLocked(KafkaProducer::ErrorKind::Fatal, _last_error);
            _recreate_scheduled = true;
            post([](EventBus &bus) {
                bool enable;
                KafkaSettings settings;
                {
                    lock_guard<mutex> lck(bus._mtx);
                    enable = bus._enabled;
                    settings = bus._settings;
                }
                bus.applyTask(enable, settings);
            });
        }
        if (_mode == Mode::Live) {
            if (_no_broker_since_ms && now - _no_broker_since_ms > spool_after_sec * 1000) {
                enterSpoolLocked(KafkaProducer::ErrorKind::BrokerDown,
                                 "no broker up for " + to_string((now - _no_broker_since_ms) / 1000) + " s");
            }
        } else if (_mode == Mode::Spool) {
            if (producer && !fatal && up > 0 && now >= _next_retry_ms && _spool_ready) {
                _mode = Mode::Drain;
                _replay_reset = true;
                InfoL << "EventBus: broker up, replaying " << _spool_pending << " spooled events";
            }
        }
        if (_mode == Mode::Drain && !_drain_scheduled) {
            _drain_scheduled = true;
            post([](EventBus &bus) { bus.drainStep(); });
        }

        // Not for a short spool (settings change, a burst filling the queue): after spoolAfterSec.
        if ((_mode == Mode::Spool || _mode == Mode::Drain) && _spool_since_ms &&
            now - _spool_since_ms >= spool_after_sec * 1000) {
            if (!_unavailable_alerted || (repeat_sec && now - _last_unavailable_alert_ms >= repeat_sec * 1000)) {
                alert_unavailable = true;
                _unavailable_alerted = true;
                _last_unavailable_alert_ms = now;
                reason = KafkaProducer::kindName(_spool_reason);
                last_error = _last_error;
            }
        }
        if (_recovered_pending) {
            _recovered_pending = false;
            alert_recovered = true;
            _unavailable_alerted = false;
            replayed = static_cast<int64_t>(_replayed_episode);
            duration = static_cast<int64_t>((now - _spool_since_ms) / 1000);
            _replayed_episode = 0;
            _spool_since_ms = 0;
        }
        const uint64_t high_bytes = (max_mb << 20) / 100 * high_pct;
        if (high_pct && _spool_bytes >= high_bytes && !_spool_high_alerted) {
            _spool_high_alerted = true;
            alert_high = true;
        } else if (_spool_high_alerted && _spool_bytes < high_bytes / 2) {
            _spool_high_alerted = false;
        }
        if (now - _last_maintain_ms >= std::max<uint64_t>(fsync_ms, 100)) {
            _last_maintain_ms = now;
            spool_work = _spool_ready || !_write_queue.empty();
        }
    }
    if (spool_work) {
        post([](EventBus &bus) { bus.maintainSpool(); });
    }
    if (alert_unavailable) {
        emitAlert("EventBusUnavailable", reason, last_error, static_cast<int64_t>(_spool_pending.load()), 0, 0, 0, 0);
    }
    if (alert_high) {
        emitAlert("EventBusSpoolHigh", "", "", static_cast<int64_t>(_spool_bytes >> 20),
                  static_cast<int64_t>(max_mb * high_pct / 100), 0, 0, 0);
    }
    if (alert_recovered) {
        emitAlert("EventBusRecovered", "", "", 0, 0, 0, replayed, duration);
    }
    if (stats_sec && now - _last_stats_ms >= stats_sec * 1000) {
        if (_last_stats_ms) {
            logStats();
        }
        _last_stats_ms = now;
    }
}

void EventBus::emitAlert(const string &code, const string &reason, const string &last_error, int64_t current,
                         int64_t threshold, int64_t dropped, int64_t replayed, int64_t duration_sec) {
    const int64_t pending = static_cast<int64_t>(_spool_pending.load());
    const int64_t bytes = static_cast<int64_t>(_spool_bytes.load());
    NOTICE_EMIT(BroadcastEventBusAlertArgs, Broadcast::kBroadcastEventBusAlert, code, reason, last_error, current,
                threshold, pending, bytes, dropped, replayed, duration_sec);
}

void EventBus::logStats() {
    auto s = stats();
    InfoL << "EventBus kafka: mode=" << modeName(s.mode) << " brokers_up=" << s.brokersUp << "/" << s.brokersKnown
          << " queued=" << s.queued << " produced=" << s.produced << " delivered=" << s.delivered
          << " spooled=" << s.spooled << " replayed=" << s.replayed << " spool_pending=" << s.spoolPending
          << " spool_mb=" << (s.spoolBytes >> 20) << " dropped{rate=" << s.droppedRate << " spool=" << s.droppedSpool
          << " mem=" << s.droppedMem << "} skipped_initialized=" << s.skippedInitialized << " resynced=" << s.resynced;
}

void EventBus::shutdown() {
    KafkaProducer::Ptr producer;
    {
        lock_guard<mutex> lck(_mtx);
        if (_stopping) {
            return;
        }
        _stopping = true;
        producer = _producer;
    }
    if (_timer) {
        _timer->cancel();
    }
    GET_CONFIG(int, flush_ms, KafkaCfg::kFlushTimeoutMs);
    if (producer) {
        // Callbacks on the timer's poller, as during normal running.
        _poller->sync([&]() {
            if (producer->flush(flush_ms) > 0) {
                producer->purgeQueued();
                producer->poll(0);
            }
        });
    }
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    post([done](EventBus &bus) {
        bus.commitReplayed();
        bus.writeQueued();
        if (bus._spool) {
            bus._spool->sync();
            bus._spool->flushCheckpoint();
            bus._spool_pending = bus._spool->stats().pending;
        }
        done->set_value();
    });
    if (fut.wait_for(std::chrono::milliseconds(flush_ms + 10000)) != std::future_status::ready) {
        WarnL << "EventBus: spool not written in time at exit";
    }
    {
        lock_guard<mutex> lck(_mtx);
        _producer.reset();
    }
    InfoL << "EventBus stopped: delivered=" << _delivered << " spooled=" << _spooled << " spool_pending=" << _spool_pending;
    producer.reset();
}

#endif // ENABLE_KAFKA

} // namespace managerkit
