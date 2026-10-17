#ifndef S3MEDIAKIT_SERVERCONFIGLOADER_H
#define S3MEDIAKIT_SERVERCONFIGLOADER_H

#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include "Common/SerialTaskQueue.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "json/json.h"

// One apply in flight and one latest response waiting. Assign revision when starting the HTTP
// request: a delayed older response must never overwrite a newer config or decommission decision.
class ServerConfigLoader : public std::enable_shared_from_this<ServerConfigLoader> {
public:
    using Apply = std::function<bool(const Json::Value &)>;
    using Qualify = std::function<bool(bool)>;
    using Clock = std::function<uint64_t()>;

    ServerConfigLoader(toolkit::TaskExecutor::Ptr executor, Apply apply, Qualify qualify,
                       Clock clock = []() { return toolkit::getCurrentMillisecond(true); })
        : _queue(std::make_shared<managerkit::SerialTaskQueue>(std::move(executor), "server config"))
        , _apply(std::move(apply)), _qualify(std::move(qualify)), _clock(std::move(clock)) {}

    void submit(Json::Value config, bool qualified, uint64_t revision, uint64_t reapply_ms) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (revision <= _revision) return;
            _revision = revision;
            _next.reset(new Update {std::move(config), qualified, reapply_ms});
            if (_running) return;
            _running = true;
        }
        schedule();
    }

private:
    struct Update {
        Json::Value config;
        bool qualified;
        uint64_t reapply_ms;
    };

    static Json::Value relevantConfig(const Json::Value &config) {
        Json::Value result(Json::objectValue);
        for (const auto key : {"devices", "speakers", "media_server", "list_media_server",
                               "watermark_configs", "list_audio_file"}) {
            if (config.isMember(key)) result[key] = config[key];
        }
        return result;
    }

    void schedule() {
        auto self = shared_from_this();
        _queue->post([self]() { self->runOne(); });
    }

    void runOne() {
        std::unique_ptr<Update> update;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            update = std::move(_next);
        }
        try {
            // Reconcile qualification even when config is unchanged (rejoin and ESC catch-up).
            if (!_qualify(update->qualified)) {
                _has_applied = false;
            } else {
                auto config = relevantConfig(update->config);
                const auto now = _clock();
                if (_has_applied && config == _applied && update->reapply_ms &&
                    now - _applied_at < update->reapply_ms) {
                    TraceL << "Server configuration unchanged, skip apply";
                } else {
                    _has_applied = false;
                    if (_apply(update->config)) {
                        _applied = std::move(config);
                        _applied_at = _clock();
                        _has_applied = true;
                    }
                }
            }
        } catch (const std::exception &ex) {
            _has_applied = false;
            WarnL << "Apply server configuration failed: " << ex.what();
        } catch (...) {
            _has_applied = false;
            WarnL << "Apply server configuration failed";
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_next) {
                _running = false;
                return;
            }
        }
        // Re-post to yield the shared pool between applies.
        schedule();
    }

    managerkit::SerialTaskQueue::Ptr _queue;
    Apply _apply;
    Qualify _qualify;
    Clock _clock;
    std::mutex _mutex; // only submission state; never held across apply/qualification
    uint64_t _revision = 0;
    bool _running = false;
    std::unique_ptr<Update> _next;
    bool _has_applied = false; // worker-only state below
    uint64_t _applied_at = 0;
    Json::Value _applied;
};

#endif
