#ifndef CAMERA_VIDEOCONFIGRULEDECISION_H
#define CAMERA_VIDEOCONFIGRULEDECISION_H

#include "Control/OnvifDeviceControl.h"

#include <algorithm>
#include <cstdint>

namespace managerkit {

// What VideoConfigJobManager does with a new rule request for one camera profile. Kept free of
// camera and timer state so the whole table can be unit tested.
enum class RuleRequestAction {
    START,           // no job yet: apply the config now
    ALREADY_APPLIED, // no job and the request asks for the baseline: nothing to change
    EXTEND_ACTIVE,   // same config as the running job: keep it applied until the later deadline
    QUEUE,           // different config: run it after the running job
};

struct RuleRequestDecision {
    RuleRequestAction action = RuleRequestAction::START;
    // The waiting job is dropped (reported RULE_SUPERSEDED): the newest request is what the caller
    // wants now, whether it extends the running job or replaces the waiting one.
    bool supersedeQueued = false;
    int64_t deadlineAt = 0; // deadline of the job the request ends up in
};

inline RuleRequestDecision decideRuleRequest(bool hasActive, const VideoEncoderConfig &active, int64_t activeDeadlineAt,
                                             bool hasQueued, const VideoEncoderConfig &baseline,
                                             const VideoEncoderConfig &target, int64_t requestDeadlineAt) {
    RuleRequestDecision decision;
    decision.deadlineAt = requestDeadlineAt;
    if (!hasActive) {
        decision.action = target == baseline ? RuleRequestAction::ALREADY_APPLIED : RuleRequestAction::START;
        return decision;
    }
    decision.supersedeQueued = hasQueued;
    if (target == active) {
        // An extension never shortens the job: an earlier request may still need the config.
        decision.action = RuleRequestAction::EXTEND_ACTIVE;
        decision.deadlineAt = std::max(activeDeadlineAt, requestDeadlineAt);
        return decision;
    }
    // Includes a request for the baseline itself: it runs after the job so the rollback still
    // happens on time.
    decision.action = RuleRequestAction::QUEUE;
    return decision;
}

} // namespace managerkit

#endif // CAMERA_VIDEOCONFIGRULEDECISION_H
