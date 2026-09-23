#ifndef ZLMEDIAKIT_VIDEOCONFIGRULEJOB_H
#define ZLMEDIAKIT_VIDEOCONFIGRULEJOB_H

#include "DbStorage.h"
#include "Camera/GenericRtspCamera.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

enum class RuleJobStatus {
    ACTIVE = 0,   // Currently applied to the camera
    QUEUED = 1    // Waiting for the active job to expire
};

struct VideoConfigRuleJob {
    std::string id;
    std::string logId;
    std::string deviceId;
    std::string profileToken;
    int status = 0;
    std::string vcodec;
    int width = 0;
    int height = 0;
    float fps = 0;
    int bitrate = 0;
    int64_t receivedAt = 0;
    int64_t appliedAt = 0;
    int64_t deadlineAt = 0;
    int64_t updatedAt = 0;

    static std::string makeId(const std::string &deviceId, const std::string &profileToken, RuleJobStatus status) {
        return deviceId + "|" + profileToken + "|" + (status == RuleJobStatus::ACTIVE ? "active" : "queued");
    }

    VideoEncoderConfig toVideoEncoderConfig() const {
        VideoEncoderConfig config;
        config.vcodec = vcodec;
        config.width = width;
        config.height = height;
        config.fps = fps;
        config.bitrate = bitrate;
        return config;
    }

    void fromVideoEncoderConfig(const VideoEncoderConfig &config) {
        vcodec = config.vcodec;
        width = config.width;
        height = config.height;
        fps = config.fps;
        bitrate = config.bitrate;
    }
};

DECLARE_ENTITY(VideoConfigRuleJob, "video_config_rule_job",
    { "id" },
    &VideoConfigRuleJob::id, "id",
    &VideoConfigRuleJob::logId, "logId",
    &VideoConfigRuleJob::deviceId, "deviceId",
    &VideoConfigRuleJob::profileToken, "profileToken",
    &VideoConfigRuleJob::status, "status",
    &VideoConfigRuleJob::vcodec, "vcodec",
    &VideoConfigRuleJob::width, "width",
    &VideoConfigRuleJob::height, "height",
    &VideoConfigRuleJob::fps, "fps",
    &VideoConfigRuleJob::bitrate, "bitrate",
    &VideoConfigRuleJob::receivedAt, "receivedAt",
    &VideoConfigRuleJob::appliedAt, "appliedAt",
    &VideoConfigRuleJob::deadlineAt, "deadlineAt",
    &VideoConfigRuleJob::updatedAt, "updatedAt"
)

class VideoConfigRuleJobRepository : public SqliteRepository<VideoConfigRuleJob> {
public:
    VideoConfigRuleJobRepository() : SqliteRepository<VideoConfigRuleJob>(Database::kMediaServerDb) {}

    std::vector<VideoConfigRuleJob> findAll() {
        auto rows = _executor->executeRaw(
            toolkit::QueryBuilder()
                .select(EntityTraits<VideoConfigRuleJob>::getColumns())
                .from(EntityTraits<VideoConfigRuleJob>::tableName()));
        std::vector<VideoConfigRuleJob> ret;
        for (const auto &row : rows) {
            ret.push_back(EntityTraits<VideoConfigRuleJob>::fromRow(row));
        }
        return ret;
    }
};

class VideoConfigRuleJobImp : public VideoConfigRuleJobRepository {
public:
    using Ptr = std::shared_ptr<VideoConfigRuleJobImp>;

    VideoConfigRuleJobImp() : VideoConfigRuleJobRepository() {}

    // Writes `job` unless the stored row carries a newer updatedAt (a later transition already
    // won). Returns false when skipped. Read and write share one BEGIN IMMEDIATE transaction, so two
    // writers of the same id cannot both see "absent" and collide on the primary key.
    bool addOrUpdate(const VideoConfigRuleJob &job) {
        auto txn = _executor->beginWriteTxn();
        try {
            auto rows = findByIdWithTxn(job, txn);
            if (!rows.empty() && rows[0].updatedAt > job.updatedAt) {
                InfoL << "Skipping write of row " << job.id << " because it was superseded by a newer transition"
                      << " (updatedAt=" << rows[0].updatedAt << " > " << job.updatedAt << ")";
                txn->commit();
                return false;
            }
            bool ok = rows.empty() ? saveWithTxn(job, txn, true) : updateByIdWithTxn(job, txn);
            txn->commit();
            return ok;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    // Deletes row `id` unless it was updated after `ts`. Returns false when skipped.
    bool removeIfNotNewer(const std::string &id, int64_t ts) {
        VideoConfigRuleJob key;
        key.id = id;
        auto txn = _executor->beginWriteTxn();
        try {
            auto rows = findByIdWithTxn(key, txn);
            if (rows.empty()) {
                txn->commit();
                return true;
            }
            if (rows[0].updatedAt > ts) {
                InfoL << "Skipping deletion of row " << id << " because it was superseded by a newer transition"
                      << " (updatedAt=" << rows[0].updatedAt << " > ts=" << ts << ")";
                txn->commit();
                return false;
            }
            removeByIdWithTxn(key, txn);
            txn->commit();
            return true;
        } catch (...) {
            txn->rollback();
            throw;
        }
    }

    void removeByIdStr(const std::string &id) {
        VideoConfigRuleJob job;
        job.id = id;
        removeById(job);
    }

    std::vector<VideoConfigRuleJob> findByIdStr(const std::string &id) {
        VideoConfigRuleJob job;
        job.id = id;
        return VideoConfigRuleJobRepository::findById(job);
    }

    std::vector<VideoConfigRuleJob> findDeviceJobs(const std::string &deviceId, const std::string &profileToken) {
        std::vector<VideoConfigRuleJob> ret;
        for (auto status : { RuleJobStatus::ACTIVE, RuleJobStatus::QUEUED }) {
            auto rows = findByIdStr(VideoConfigRuleJob::makeId(deviceId, profileToken, status));
            for (const auto &row : rows) {
                ret.push_back(row);
            }
        }
        return ret;
    }
};

} // namespace managerkit

#endif // ZLMEDIAKIT_VIDEOCONFIGRULEJOB_H