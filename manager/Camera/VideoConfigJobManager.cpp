#include "VideoConfigJobManager.h"
#include "VideoConfigRuleDecision.h"
#include "Common/BlockingExecutors.h"
#include "Common/SerialTaskQueue.h"
#include "server/WebApiErrCode.h"
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Util/onceToken.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

namespace VideoConfigRuleConfig {
#define VIDEO_CONFIG_RULE_FIELD "video_config_rule."
const string kResultKeepHours = VIDEO_CONFIG_RULE_FIELD "result_keep_hours";

static onceToken token([]() {
    mINI::Instance()[kResultKeepHours] = 168;
});
} // namespace VideoConfigRuleConfig

INSTANCE_IMP(VideoConfigJobManager)

VideoConfigJobManager::VideoConfigJobManager() {}

void VideoConfigJobManager::reportFailure(const string &logId, const string &deviceId, const string &errCode, const string &errMsg) {
    DebugL << "[RuleJob] " << errCode << ", logId=" << logId << ", deviceId=" << deviceId << ", msg=" << errMsg;
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, Broadcast::kBroadcastVideoConfigRuleResult, logId, deviceId, false, errCode, errMsg);
}

void VideoConfigJobManager::reportSuccess(const string &logId, const string &deviceId) {
    DebugL << "[RuleJob] already applied, logId=" << logId << ", deviceId=" << deviceId;
    // NOTICE_EMIT std::forward-s each argument as its declared const std::string &: a "" literal
    // would have to become a temporary string there, which std::forward refuses.
    const string none;
    NOTICE_EMIT(BroadcastVideoConfigRuleResultArgs, Broadcast::kBroadcastVideoConfigRuleResult, logId, deviceId, true, none, none);
}

VideoConfigJobManager::JobSlotPtr VideoConfigJobManager::findSlot(const string &key) const {
    lock_guard<mutex> lk(_mtx);
    auto it = _slots.find(key);
    return it == _slots.end() ? nullptr : it->second;
}

void VideoConfigJobManager::insertSlot(const string &key, const JobSlotPtr &slot) {
    lock_guard<mutex> lk(_mtx);
    _slots[key] = slot;
}

void VideoConfigJobManager::eraseSlot(const string &key) {
    lock_guard<mutex> lk(_mtx);
    _slots.erase(key);
}

bool VideoConfigJobManager::markRecoverStarted(const string &key) {
    lock_guard<mutex> lk(_mtx);
    return _recovered.insert(key).second;
}

void VideoConfigJobManager::unmarkRecoverStarted(const string &key) {
    lock_guard<mutex> lk(_mtx);
    _recovered.erase(key);
}

void VideoConfigJobManager::deferRelease(const EventPoller::Ptr &poller, std::shared_ptr<void> obj) {
    if (!poller || !obj) {
        return;
    }
    poller->async([obj]() {}, false);
}

// True while the current thread runs a task of the DB strand.
static thread_local bool t_onDbStrand = false;

void VideoConfigJobManager::postDbTask(std::function<void()> task) {
    // SQLite work stays off the pollers (WorkThreadPool threads run timers and ordered queues), and
    // goes through one strand so rows are written in the order the owner pollers decided them: on the
    // plain api() pool two writes of the same row could run concurrently or swap.
    static SerialTaskQueue::Ptr queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "video-config-rule-db");
    queue->post([task]() {
        t_onDbStrand = true;
        onceToken reset(nullptr, []() { t_onDbStrand = false; });
        task();
    });
}

void VideoConfigJobManager::asyncDbTask(std::function<void(VideoConfigRuleJobImp &)> task) {
    postDbTask([task]() {
        VideoConfigRuleJobImp repo;
        task(repo);
    });
}

void VideoConfigJobManager::start() {
    NoticeCenter::Instance().addListener(this, Broadcast::kBroadcastVideoConfigRuleResult, [](BroadcastVideoConfigRuleResultArgs) {
        int64_t now = nowMs();
        if (t_onDbStrand) {
            // Emitted by a DB task (e.g. removeDevice): write in place, before any read queued meanwhile.
            VideoConfigRuleResultImp repo;
            repo.recordResult(log_id, device_id, success, err_code, err_msg, now);
            return;
        }
        // Emitted on a camera poller: only queue the write.
        string logId = log_id, deviceId = device_id, errCode = err_code, errMsg = err_msg;
        bool ok = success;
        postDbTask([logId, deviceId, ok, errCode, errMsg, now]() {
            VideoConfigRuleResultImp repo;
            repo.recordResult(logId, deviceId, ok, errCode, errMsg, now);
        });
    });
    pruneResultsIfDue(nowMs());
}

void VideoConfigJobManager::stop() {
    NoticeCenter::Instance().delListener(this, Broadcast::kBroadcastVideoConfigRuleResult);
}

void VideoConfigJobManager::recordAccepted(const string &logId, const vector<string> &deviceIds) {
    if (deviceIds.empty()) {
        return;
    }
    int64_t now = nowMs();
    postDbTask([logId, deviceIds, now]() {
        VideoConfigRuleResultImp repo;
        for (const auto &deviceId : deviceIds) {
            repo.markPending(logId, deviceId, now);
        }
    });
    pruneResultsIfDue(now);
}

void VideoConfigJobManager::queryResults(const string &logId, const ResultsCallback &cb) {
    postDbTask([logId, cb]() {
        vector<VideoConfigRuleResult> results;
        string error;
        try {
            VideoConfigRuleResultImp repo;
            results = repo.findByLogId(logId);
        } catch (const std::exception &ex) {
            error = ex.what();
            WarnL << "[RuleJob] reading results failed, logId=" << logId << ": " << error;
        }
        cb(results, error);
    });
}

void VideoConfigJobManager::pruneResultsIfDue(int64_t now) {
    static const int64_t kPruneIntervalMs = 3600 * 1000;
    int64_t last = _lastResultPruneMs.load();
    if (now - last < kPruneIntervalMs || !_lastResultPruneMs.compare_exchange_strong(last, now)) {
        return;
    }
    GET_CONFIG(int, keepHours, VideoConfigRuleConfig::kResultKeepHours);
    if (keepHours <= 0) {
        return;
    }
    int64_t cutoff = now - (int64_t)keepHours * 3600 * 1000;
    postDbTask([cutoff]() {
        VideoConfigRuleResultImp repo;
        repo.removeUpdatedBefore(cutoff);
    });
}

void VideoConfigJobManager::persistJob(const VideoConfigRuleJob &job) {
    asyncDbTask([job](VideoConfigRuleJobImp &repo) {
        repo.addOrUpdate(job);
    });
}

void VideoConfigJobManager::persistPromote(const VideoConfigRuleJob &newActive, int64_t ts) {
    string queuedId = VideoConfigRuleJob::makeId(newActive.deviceId, newActive.profileToken, RuleJobStatus::QUEUED);
    asyncDbTask([newActive, queuedId, ts](VideoConfigRuleJobImp &repo) {
        repo.removeIfNotNewer(queuedId, ts);
        repo.addOrUpdate(newActive);
    });
}

void VideoConfigJobManager::persistClear(const string &deviceId, const string &profileToken, int64_t ts) {
    string activeId = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::ACTIVE);
    string queuedId = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::QUEUED);
    asyncDbTask([activeId, queuedId, ts](VideoConfigRuleJobImp &repo) {
        repo.removeIfNotNewer(activeId, ts);
        repo.removeIfNotNewer(queuedId, ts);
    });
}

void VideoConfigJobManager::persistRemoveQueued(const string &deviceId, const string &profileToken, int64_t ts) {
    string queuedId = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::QUEUED);
    asyncDbTask([queuedId, ts](VideoConfigRuleJobImp &repo) {
        repo.removeIfNotNewer(queuedId, ts);
    });
}

void VideoConfigJobManager::handleRuleRequest(const GenericRtspCameraImp::Ptr &device,
                                                   const string &logId, const string &deviceId,
                                                   const string &profileToken,
                                                   int width, int height, float fps, int bitrate,
                                                   int applyDurationSec) {
    CHECK(device->getOwnerPoller(DeviceSource::NullDeviceSource())->isCurrentThread(), "handleRuleRequest can only be called in it's owner poller");

    int64_t receivedAt = nowMs();
    int64_t deadlineAt = receivedAt + (int64_t)applyDurationSec * 1000;

    string key = makeKey(deviceId, profileToken);
    weak_ptr<GenericRtspCameraImp> weak_device = device;

    device->ensureBaselineVideoConfig(profileToken,
        [weak_device, key, logId, deviceId, profileToken, width, height, fps, bitrate, receivedAt, deadlineAt]
        (const SockException &ex, const VideoEncoderConfig &baseline) mutable {
            auto strong_device = weak_device.lock();
            if (!strong_device) {
                return;
            }
            auto &self = VideoConfigJobManager::Instance();
            if (ex) {
                self.reportFailure(logId, deviceId, "GET_CONFIG_FAILED", ex.what());
                return;
            }
            self.onBaselineReady(strong_device, key, logId, deviceId, profileToken, baseline,
                             width, height, fps, bitrate, receivedAt, deadlineAt);
        });
}

void VideoConfigJobManager::onBaselineReady(const GenericRtspCameraImp::Ptr &device,
                                                 const string &key, const string &logId, const string &deviceId,
                                                 const string &profileToken, const VideoEncoderConfig &baseline,
                                                 int width, int height, float fps, int bitrate,
                                                 int64_t receivedAt, int64_t deadlineAt) {
    int64_t now = nowMs();
    if (now >= deadlineAt) {
        reportFailure(logId, deviceId, "RULE_EXPIRED", "Rule expired while loading baseline config");
        return;
    }

    auto slot = findSlot(key);
    VideoEncoderConfig target = baseline;
    target.width = width;
    target.height = height;
    target.fps = fps;
    target.bitrate = bitrate;

    VideoEncoderConfig active = slot ? slot->active.toVideoEncoderConfig() : baseline;
    auto decision = decideRuleRequest(slot != nullptr, active, slot ? slot->active.deadlineAt : 0,
                                      slot && slot->hasQueued, baseline, target, deadlineAt);

    if (decision.action == RuleRequestAction::ALREADY_APPLIED) {
        reportSuccess(logId, deviceId);
        return;
    }

    if (decision.supersedeQueued) {
        reportFailure(slot->queued.logId, deviceId, "RULE_SUPERSEDED", "Replaced by a newer rule request before it could start");
        slot->hasQueued = false;
    }

    if (decision.action == RuleRequestAction::EXTEND_ACTIVE) {
        if (decision.supersedeQueued) {
            // Only here: a new QUEUED row overwrites the old one, and deleting it too would race
            // with that write.
            persistRemoveQueued(deviceId, profileToken, now);
        }
        extendJob(device, key, slot, logId, receivedAt, decision.deadlineAt, now);
        return;
    }

    VideoConfigRuleJob job;
    job.logId = logId;
    job.deviceId = deviceId;
    job.profileToken = profileToken;
    job.fromVideoEncoderConfig(target);
    job.receivedAt = receivedAt;
    job.deadlineAt = deadlineAt;
    job.updatedAt = now;

    if (decision.action == RuleRequestAction::START) {
        slot = std::make_shared<JobSlot>();
        insertSlot(key, slot);

        job.status = (int)RuleJobStatus::ACTIVE;
        job.appliedAt = now;
        job.id = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::ACTIVE);
        startJob(device, key, slot, job, true);
        return;
    }

    job.status = (int)RuleJobStatus::QUEUED;
    job.appliedAt = 0;
    job.id = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::QUEUED);
    slot->queued = job;
    slot->hasQueued = true;
    persistJob(job);
    InfoL << "Job queued, logId=" << logId << ", deviceId=" << deviceId
        << ", current job expires at " << slot->active.deadlineAt
        << ", queued job expires at " << job.deadlineAt;
}

void VideoConfigJobManager::extendJob(const GenericRtspCameraImp::Ptr &device, const string &key, const JobSlotPtr &slot,
                                      const string &logId, int64_t receivedAt, int64_t deadlineAt, int64_t now) {
    const bool deadlineChanged = deadlineAt != slot->active.deadlineAt;
    slot->active.logId = logId;
    slot->active.receivedAt = receivedAt;
    slot->active.deadlineAt = deadlineAt;
    slot->active.updatedAt = now;

    if (!device->attachRuleLogId(slot->active.profileToken, logId)) {
        // The camera never took the config (or the controller lost the rule): apply it again for
        // the extending request, which also re-arms the deadline.
        InfoL << "Extending job re-applies its config, logId=" << logId << ", deviceId=" << slot->active.deviceId;
        startJob(device, key, slot, slot->active, true);
        return;
    }

    persistJob(slot->active);
    if (deadlineChanged) {
        armDeadlineTimer(device, key, slot);
    }
    InfoL << "Extended job, logId=" << logId << ", deviceId=" << slot->active.deviceId
        << ", token=" << slot->active.profileToken << ", expires at " << slot->active.deadlineAt;
}

void VideoConfigJobManager::startJob(const GenericRtspCameraImp::Ptr &device, const string &key,
                                          const JobSlotPtr &slot, const VideoConfigRuleJob &job, bool persist) {
    slot->active = job;
    slot->activeReported = false;
    if (persist) {
        persistJob(job);
    }

    VideoEncoderConfig config = job.toVideoEncoderConfig();
    device->applyRuleVideoConfig(job.logId, job.profileToken, config, [key](const SockException &ex) {
        if (ex) {
            WarnL << "Apply not yet successful, waiting for syncMediaProfile retry, key=" << key << ", err=" << ex.what();
        }
    });

    float delaySec = armDeadlineTimer(device, key, slot);

    InfoL << "Started job, logId=" << job.logId << ", deviceId=" << job.deviceId
        << ", token=" << job.profileToken
        << ", " << job.width << "x" << job.height << "@" << job.fps
        << "fps/" << job.bitrate
        << ", remaining " << (int)delaySec << "s";
}

float VideoConfigJobManager::armDeadlineTimer(const GenericRtspCameraImp::Ptr &device, const string &key, const JobSlotPtr &slot) {
    auto poller = device->getOwnerPoller(DeviceSource::NullDeviceSource());
    int64_t remaining = slot->active.deadlineAt - nowMs();
    float delaySec = remaining > 0 ? (float)remaining / 1000.0f : 0.1f;

    auto oldTimer = slot->deadlineTimer;
    weak_ptr<GenericRtspCameraImp> weak_device = device;
    slot->deadlineTimer = std::make_shared<Timer>(delaySec, [key, weak_device]() -> bool {
        if (auto strong_device = weak_device.lock()) {
            VideoConfigJobManager::Instance().onDeadline(strong_device, key);
        }
        return false;
    }, poller);
    if (oldTimer) {
        deferRelease(poller, oldTimer);
    }
    return delaySec;
}

void VideoConfigJobManager::onDeadline(const GenericRtspCameraImp::Ptr &device, const string &key) {
    auto slot = findSlot(key);
    if (!slot) {
        return;
    }
    promoteOrRollback(device, key, slot, nowMs());
}

void VideoConfigJobManager::promoteOrRollback(const GenericRtspCameraImp::Ptr &device, const string &key, const JobSlotPtr &slot, int64_t now) {
    string deviceId = slot->active.deviceId;
    string profileToken = slot->active.profileToken;

    if (!slot->hasQueued) {
        finishJob(device, key, slot, now);
        return;
    }

    VideoConfigRuleJob next = slot->queued;
    if (now >= next.deadlineAt) {
        reportFailure(next.logId, deviceId, "RULE_EXPIRED", "Queued rule expired while waiting for the running job to finish");
        finishJob(device, key, slot, now);
        return;
    }

    VideoEncoderConfig baseline;
    bool hasBaseline = device->getBaselineVideoConfig(profileToken, baseline);
    VideoEncoderConfig nextConfig = next.toVideoEncoderConfig();
    if (hasBaseline && nextConfig == baseline) {
        // The queued request asks for what the rollback restores: it is satisfied by it.
        reportSuccess(next.logId, deviceId);
        finishJob(device, key, slot, now);
        return;
    }
    if (next.vcodec.empty() && hasBaseline) {
        next.vcodec = baseline.vcodec;
    }

    next.status = (int)RuleJobStatus::ACTIVE;
    next.appliedAt = now;
    next.updatedAt = now;
    next.id = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::ACTIVE);

    slot->hasQueued = false;
    persistPromote(next, now);
    startJob(device, key, slot, next, false);
}

void VideoConfigJobManager::finishJob(const GenericRtspCameraImp::Ptr &device, const string &key, const JobSlotPtr &slot, int64_t now) {
    string logId = slot->active.logId;
    string deviceId = slot->active.deviceId;
    string profileToken = slot->active.profileToken;

    if (!slot->activeReported && !device->isRuleResultReported(profileToken)) {
        string errCode = "CONTROLLER_OFFLINE";
        string errMsg = "Onvif controller was not reachable when the apply duration ended";
        if (device->isControllerReady()) {
            errCode = "CONFIG_NOT_CONFIRMED";
            errMsg = "Camera config could not be confirmed before the apply duration ended";
        }
        // Every request folded into the job gets the failure; without a controller only the
        // job's latest request is known.
        if (!device->reportRuleFailureIfPending(profileToken, errCode, errMsg)) {
            reportFailure(logId, deviceId, errCode, errMsg);
        }
    }

    persistClear(deviceId, profileToken, now);
    eraseSlot(key);
    deferRelease(device->getOwnerPoller(DeviceSource::NullDeviceSource()), slot);
    rollbackToBaseline(device, profileToken);
}

void VideoConfigJobManager::rollbackToBaseline(const GenericRtspCameraImp::Ptr &device, const string &profileToken) {
    VideoEncoderConfig baseline;
    if (!device->getBaselineVideoConfig(profileToken, baseline)) {
        WarnL << "Baseline not found for rollback, token=" << profileToken;
        device->clearRuleVideoConfig(profileToken);
        return;
    }

    device->rollbackRuleVideoConfig(profileToken, baseline, [profileToken](const SockException &ex) {
        if (ex) {
            WarnL << "Rollback not completed, waiting for syncMediaProfile to retry, token=" << profileToken << ", err=" << ex.what();
        } else {
            InfoL << "Rolled back to baseline, token=" << profileToken;
        }
    });
}

void VideoConfigJobManager::recoverDeviceJobs(const GenericRtspCameraImp::Ptr &device, const string &deviceId, const string &profileToken) {
    if (profileToken.empty()) {
        return;
    }
    string key = makeKey(deviceId, profileToken);
    if (!markRecoverStarted(key)) {
        return;
    }

    auto poller = device->getOwnerPoller(DeviceSource::NullDeviceSource());
    weak_ptr<GenericRtspCameraImp> weak_device = device;

    // Same strand as the writes, so the read sees every transition persisted before it.
    asyncDbTask([deviceId, profileToken, key, poller, weak_device](VideoConfigRuleJobImp &repo) {
        auto rows = repo.findDeviceJobs(deviceId, profileToken);
        if (rows.empty()) {
            return;
        }
        // Results reported before the restart must not be reported again.
        unordered_map<string, bool> reported;
        VideoConfigRuleResultImp results;
        for (const auto &row : rows) {
            bool success = false;
            if (results.isReported(row.logId, deviceId, success)) {
                reported[row.logId] = success;
            }
        }
        poller->async([rows, reported, deviceId, profileToken, key, weak_device]() {
            auto strong_device = weak_device.lock();
            if (!strong_device) {
                return;
            }
            VideoConfigJobManager::Instance().applyRecoveredRows(strong_device, key, deviceId, profileToken, rows, reported);
        });
    });
}

void VideoConfigJobManager::applyRecoveredRows(const GenericRtspCameraImp::Ptr &device, const string &key,
                                                    const string &deviceId, const string &profileToken,
                                                    const vector<VideoConfigRuleJob> &rows,
                                                    const unordered_map<string, bool> &reported) {
    if (findSlot(key)) {
        return;
    }

    VideoConfigRuleJob activeRow, queuedRow;
    bool hasActive = false, hasQueued = false;
    for (const auto &row : rows) {
        if (row.status == (int)RuleJobStatus::ACTIVE) {
            activeRow = row;
            hasActive = true;
        } else {
            queuedRow = row;
            hasQueued = true;
        }
    }

    // Only a QUEUED row (the process stopped between removing it and writing it as ACTIVE): it becomes
    // the active job, and its QUEUED row must go once the job is resumed.
    bool fromQueuedRow = false;
    if (!hasActive) {
        if (!hasQueued) {
            return;
        }
        activeRow = queuedRow;
        hasQueued = false;
        fromQueuedRow = true;
    }

    auto reportedIt = reported.find(activeRow.logId);
    bool activeReported = reportedIt != reported.end();
    bool activeSucceeded = activeReported && reportedIt->second;

    weak_ptr<GenericRtspCameraImp> weak_device = device;
    device->ensureBaselineVideoConfig(profileToken,
        [weak_device, key, deviceId, profileToken, activeRow, queuedRow, hasQueued, fromQueuedRow, activeReported, activeSucceeded]
        (const SockException &ex, const VideoEncoderConfig &baseline) mutable {
            auto strong_device = weak_device.lock();
            if (!strong_device) {
                return;
            }
            if (ex) {
                // Nothing restored: let the next controller connection try again.
                WarnL << "Recovering rule job failed, baseline unavailable, key=" << key << ", err=" << ex.what();
                VideoConfigJobManager::Instance().unmarkRecoverStarted(key);
                return;
            }
            auto &self = VideoConfigJobManager::Instance();
            if (self.findSlot(key)) {
                return;
            }

            auto slot = std::make_shared<JobSlot>();
            slot->active = activeRow;
            slot->hasQueued = hasQueued;
            if (hasQueued) {
                slot->queued = queuedRow;
            }
            self.insertSlot(key, slot);

            int64_t now = nowMs();
            if (now >= activeRow.deadlineAt) {
                InfoL << "Job expired while server was down, completing immediately, key=" << key;
                // No controller state survives a restart: without this, finishJob would blame the
                // controller (CONTROLLER_OFFLINE / CONFIG_NOT_CONFIRMED) for the downtime.
                if (!activeReported) {
                    reportFailure(activeRow.logId, deviceId, "RULE_EXPIRED", "Rule expired while the media server was not running");
                }
                slot->activeReported = true;
                self.promoteOrRollback(strong_device, key, slot, now);
                return;
            }

            VideoEncoderConfig ruleConfig = activeRow.toVideoEncoderConfig();
            if (ruleConfig.vcodec.empty()) {
                ruleConfig.vcodec = baseline.vcodec;
            }
            if (ruleConfig == baseline) {
                // The camera already runs the requested config.
                if (!activeReported) {
                    reportSuccess(activeRow.logId, deviceId);
                }
                slot->activeReported = true;
                self.promoteOrRollback(strong_device, key, slot, now);
                return;
            }

            VideoConfigRuleJob resumed = activeRow;
            resumed.fromVideoEncoderConfig(ruleConfig);
            resumed.status = (int)RuleJobStatus::ACTIVE;
            resumed.appliedAt = now;
            resumed.updatedAt = now;
            resumed.id = VideoConfigRuleJob::makeId(deviceId, profileToken, RuleJobStatus::ACTIVE);

            InfoL << "Resumed job after restart, logId=" << resumed.logId << ", deviceId=" << deviceId << ", remaining " << (resumed.deadlineAt - now) / 1000 << "s";
            self.startJob(strong_device, key, slot, resumed, true);
            if (activeReported) {
                strong_device->markRuleResultReported(profileToken, activeSucceeded);
            }
            if (fromQueuedRow) {
                persistRemoveQueued(deviceId, profileToken, now);
            }
        });
}

void VideoConfigJobManager::releaseDevice(const string &deviceId) {
    string prefix = deviceId + "#";
    vector<JobSlotPtr> released;
    {
        lock_guard<mutex> lk(_mtx);
        for (auto it = _slots.begin(); it != _slots.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) {
                released.push_back(it->second);
                it = _slots.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = _recovered.begin(); it != _recovered.end();) {
            if (it->compare(0, prefix.size(), prefix) == 0) {
                it = _recovered.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!released.empty()) {
        InfoL << "Removed " << released.size() << " job in-memory for deviceId=" << deviceId;
    }
}

void VideoConfigJobManager::removeDevice(const string &deviceId) {
    releaseDevice(deviceId);
    // From the result table, not the in-memory jobs: a removed camera was stopped before, so its
    // jobs are already released. This also covers every request folded into a job and queued ones.
    postDbTask([deviceId]() {
        VideoConfigRuleResultImp results;
        for (const auto &row : results.findPendingByDeviceId(deviceId)) {
            reportFailure(row.logId, deviceId, "DEVICE_REMOVED", "Camera was removed from this media server");
        }
        VideoConfigRuleJobImp jobs;
        jobs.removeByDeviceId(deviceId);
        InfoL << "[RuleJob] removed rule jobs of deleted camera, deviceId=" << deviceId;
    });
}

static void *s_tag;

static onceToken g_token(
[]() {
    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastCameraControllerState,
        [](BroadcastCameraControllerStateArgs) {
            if (!connect) {
                return;
            }

            auto strong_listener = sender.getListener().lock();
            if (!strong_listener) {
                return;
            }
            auto cameraImp = std::dynamic_pointer_cast<GenericRtspCameraImp>(strong_listener);
            if (!cameraImp) {
                return;
            }
            auto stats_imp = cameraImp->getCameraStatisticImp();
            if (!stats_imp) {
                return;
            }
            auto token = stats_imp->getPrimaryProfileToken();
            if (token.empty()) {
                return;
            }
            VideoConfigJobManager::Instance().recoverDeviceJobs(cameraImp, stats_imp->getParams().tuple.device_id, token);
        });

    NoticeCenter::Instance().addListener(&s_tag, Broadcast::kBroadcastCameraStopped,
        [](BroadcastCameraStoppedArgs) {
            VideoConfigJobManager::Instance().releaseDevice(device_id);
        });
},
[]() {
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastCameraControllerState);
    NoticeCenter::Instance().delListener(&s_tag, Broadcast::kBroadcastCameraStopped);
});

} // namespace managerkit