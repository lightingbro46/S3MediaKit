#ifndef S3MANAGERKIT_VIDEO_CONFIG_RULE_RESULT_H
#define S3MANAGERKIT_VIDEO_CONFIG_RULE_RESULT_H

#include <string>
#include <vector>
#include <json/json.h>

#include "DbStorage.h"

namespace managerkit {

// Values of VideoConfigRuleResult::status. Namespace-scope constexpr (internal linkage): usable by
// reference in C++11 without an out-of-line definition.
namespace VideoConfigRuleStatus {
constexpr const char *kPending = "PENDING";
constexpr const char *kSuccess = "SUCCESS";
constexpr const char *kFailed = "FAILED";
} // namespace VideoConfigRuleStatus

// Outcome of one camera of an onvifApplyVideoConfigRule request, looked up by the calling service
// by logId. The same body is sent to hook.on_video_config_rule_result.
struct VideoConfigRuleResult {
    std::string id;
    std::string logId;
    std::string deviceId;
    std::string status = VideoConfigRuleStatus::kPending;
    std::string errCode;
    std::string errMsg;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;

    static std::string makeId(const std::string &logId, const std::string &deviceId) {
        return logId + "|" + deviceId;
    }

    // Body of hook.on_video_config_rule_result, as sent by the recording-encryption fork:
    // errorCode/errorMessage are null unless FAILED.
    Json::Value toHookJson() const {
        Json::Value value;
        value["logId"] = logId;
        value["deviceId"] = deviceId;
        value["status"] = status;
        if (status == VideoConfigRuleStatus::kFailed) {
            value["errorCode"] = errCode;
            value["errorMessage"] = errMsg;
        } else {
            value["errorCode"] = Json::nullValue;
            value["errorMessage"] = Json::nullValue;
        }
        return value;
    }

    // Item of onvifGetVideoConfigRuleResult: the hook body plus the row's timestamps (ms).
    Json::Value toJson() const {
        Json::Value value = toHookJson();
        value["createdAt"] = (Json::Int64)createdAt;
        value["updatedAt"] = (Json::Int64)updatedAt;
        return value;
    }
};

DECLARE_ENTITY(VideoConfigRuleResult, "video_config_rule_result",
    { "id" },
    &VideoConfigRuleResult::id, "id",
    &VideoConfigRuleResult::logId, "logId",
    &VideoConfigRuleResult::deviceId, "deviceId",
    &VideoConfigRuleResult::status, "status",
    &VideoConfigRuleResult::errCode, "errCode",
    &VideoConfigRuleResult::errMsg, "errMsg",
    &VideoConfigRuleResult::createdAt, "createdAt",
    &VideoConfigRuleResult::updatedAt, "updatedAt"
)

class VideoConfigRuleResultRepository : public SqliteRepository<VideoConfigRuleResult> {
public:
    VideoConfigRuleResultRepository() : SqliteRepository<VideoConfigRuleResult>(Database::kMediaServerDb) {}

    std::vector<VideoConfigRuleResult> findByLogId(const std::string &logId) {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<VideoConfigRuleResult>::getColumns())
                .from(EntityTraits<VideoConfigRuleResult>::tableName())
                .where("logId = ?", { logId })
                .orderBy("deviceId ASC"));
        std::vector<VideoConfigRuleResult> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VideoConfigRuleResult>::fromRow(row));
        }
        return ret;
    }

    bool removeUpdatedBefore(int64_t ts) {
        return _executor->execDML(
            toolkit::QueryBuilder()
                .deleteFrom(EntityTraits<VideoConfigRuleResult>::tableName())
                .where("updatedAt < ?", { std::to_string(ts) }));
    }

protected:
    // Insert or replace row `obj.id` in one write transaction; createdAt of an existing row is kept
    // unless `resetCreatedAt`.
    bool upsert(VideoConfigRuleResult obj, bool resetCreatedAt) {
        auto txn = _executor->beginWriteTxn();
        try {
            auto rows = findByIdWithTxn(obj, txn);
            bool ok;
            if (rows.empty()) {
                ok = saveWithTxn(obj, txn, true);
            } else {
                if (!resetCreatedAt) {
                    obj.createdAt = rows[0].createdAt;
                }
                ok = updateByIdWithTxn(obj, txn);
            }
            txn->commit();
            return ok;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }
};

class VideoConfigRuleResultImp : public VideoConfigRuleResultRepository {
public:
    VideoConfigRuleResultImp() : VideoConfigRuleResultRepository() {}

    // The camera was accepted by the API: PENDING until its result arrives. A request that reuses a
    // logId for the same camera starts over.
    bool markPending(const std::string &logId, const std::string &deviceId, int64_t now) {
        auto row = makeRow(logId, deviceId, now);
        row.createdAt = now;
        return upsert(row, true);
    }

    // Final result of the camera; the latest one wins (writes arrive in emission order).
    bool recordResult(const std::string &logId, const std::string &deviceId, bool success,
                      const std::string &errCode, const std::string &errMsg, int64_t now) {
        auto row = makeRow(logId, deviceId, now);
        row.status = success ? VideoConfigRuleStatus::kSuccess : VideoConfigRuleStatus::kFailed;
        row.errCode = success ? std::string() : errCode;
        row.errMsg = success ? std::string() : errMsg;
        // Result of a camera that was never marked pending (e.g. a job recovered after the pending
        // row was pruned): it is created now.
        row.createdAt = now;
        return upsert(row, false);
    }

private:
    static VideoConfigRuleResult makeRow(const std::string &logId, const std::string &deviceId, int64_t now) {
        VideoConfigRuleResult row;
        row.id = VideoConfigRuleResult::makeId(logId, deviceId);
        row.logId = logId;
        row.deviceId = deviceId;
        row.updatedAt = now;
        return row;
    }
};

} // namespace managerkit

#endif // S3MANAGERKIT_VIDEO_CONFIG_RULE_RESULT_H
