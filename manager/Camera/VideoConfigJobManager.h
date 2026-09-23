#ifndef CAMERA_VIDEOCONFIGRULEMANAGER_H
#define CAMERA_VIDEOCONFIGRULEMANAGER_H

#include "GenericRtspCameraImp.h"
#include "manager/Storage/VideoConfigRuleJob.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace managerkit {

class VideoConfigJobManager {
public:
    static VideoConfigJobManager& Instance();
    ~VideoConfigJobManager() = default;

    void handleRuleRequest(const GenericRtspCameraImp::Ptr &device,
                            const std::string &logId,
                            const std::string &deviceId,
                            const std::string &profileToken,
                            int width, int height, float fps, int bitrate,
                            int applyDurationSec);

    void recoverDeviceJobs(const GenericRtspCameraImp::Ptr &device,
                            const std::string &deviceId,
                            const std::string &profileToken);

    void releaseDevice(const std::string &deviceId);

private:
    VideoConfigJobManager();

    struct JobSlot {
        VideoConfigRuleJob active;
        bool hasQueued = false;
        VideoConfigRuleJob queued;
        toolkit::Timer::Ptr deadlineTimer;
    };
    using JobSlotPtr = std::shared_ptr<JobSlot>;

    static std::string makeKey(const std::string &deviceId, const std::string &profileToken) {
        return deviceId + "#" + profileToken;
    }

    static int64_t nowMs() { return (int64_t)toolkit::getCurrentMillisecond(true); }

    JobSlotPtr findSlot(const std::string &key) const;
    void insertSlot(const std::string &key, const JobSlotPtr &slot);
    void eraseSlot(const std::string &key);
    bool markRecoverStarted(const std::string &key);

    void onBaselineReady(const GenericRtspCameraImp::Ptr &device,
                          const std::string &key, const std::string &logId, const std::string &deviceId,
                          const std::string &profileToken, const VideoEncoderConfig &baseline,
                          int width, int height, float fps, int bitrate,
                          int64_t receivedAt, int64_t deadlineAt);

    void startJob(const GenericRtspCameraImp::Ptr &device, const std::string &key,
                   const JobSlotPtr &slot, const VideoConfigRuleJob &job, bool persist);

    // Same config as the running job: the job takes the request's logId and the later deadline.
    void extendJob(const GenericRtspCameraImp::Ptr &device, const std::string &key, const JobSlotPtr &slot,
                   const std::string &logId, int64_t receivedAt, int64_t deadlineAt, int64_t now);

    // (Re)arms the deadline timer of `slot` from slot->active.deadlineAt; returns the delay in seconds.
    float armDeadlineTimer(const GenericRtspCameraImp::Ptr &device, const std::string &key, const JobSlotPtr &slot);

    void onDeadline(const GenericRtspCameraImp::Ptr &device, const std::string &key);

    void promoteOrRollback(const GenericRtspCameraImp::Ptr &device, const std::string &key, const JobSlotPtr &slot, int64_t now);

    void finishJob(const GenericRtspCameraImp::Ptr &device, const std::string &key, const JobSlotPtr &slot, int64_t now);

    void rollbackToBaseline(const GenericRtspCameraImp::Ptr &device, const std::string &profileToken);

    void applyRecoveredRows(const GenericRtspCameraImp::Ptr &device, const std::string &key,
                             const std::string &deviceId, const std::string &profileToken,
                             const std::vector<VideoConfigRuleJob> &rows);

    static void reportFailure(const std::string &logId, const std::string &deviceId,
                               const std::string &errCode, const std::string &errMsg);
    static void reportSuccess(const std::string &logId, const std::string &deviceId);

    static void deferRelease(const toolkit::EventPoller::Ptr &poller, std::shared_ptr<void> obj);

    static void asyncDbTask(std::function<void(VideoConfigRuleJobImp &)> task);
    static void persistJob(const VideoConfigRuleJob &job);
    static void persistPromote(const VideoConfigRuleJob &newActive, int64_t ts);
    static void persistClear(const std::string &deviceId, const std::string &profileToken, int64_t ts);
    static void persistRemoveQueued(const std::string &deviceId, const std::string &profileToken, int64_t ts);
    static void removeIfNotNewer(VideoConfigRuleJobImp &repo, const std::string &id, int64_t ts);

private:
    mutable std::mutex _mtx;
    std::unordered_map<std::string, JobSlotPtr> _slots;
    std::unordered_set<std::string> _recovered;
};

} // namespace managerkit

#endif // CAMERA_VIDEOCONFIGRULEMANAGER_H