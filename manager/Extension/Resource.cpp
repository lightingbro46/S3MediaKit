#include "Resource.h"
#include "Util/util.h"
#include "Common/config.h"
#include "Storage/MiscData.h"
#include "Storage/MediaGapFillSession.h"
#include "Storage/MediaGapFillTask.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

string getResourceStatusString(ResourceStatus status) {
    switch (status) {
        case ResourceStatus::OFFLINE:
            return "OFFLINE";
        case ResourceStatus::ONLINE:
            return "ONLINE";
        case ResourceStatus::UNAUTHORIZED:
            return "UNAUTHORIZED";
        default:
            return "UNKNOWN";
    }
}

/////////////////////ResourceTypeManager/////////////////////
 
INSTANCE_IMP(ResourceTypeManager)

ResourceTypeManager::ResourceTypeManager() {
    load();
}

void ResourceTypeManager::load() {
    auto imp = std::make_shared<VmsResourceTypeImp>();
    auto ret = imp->findAll();
    for (const auto &type : ret) {
        _resource_type_map[type.name] = type.guid;
    }
}
    
std::string ResourceTypeManager::getResourceTypeGuid(const std::string& name) {
    std::string guid;
    if (_resource_type_map.find(name) != _resource_type_map.end()) {
        guid = _resource_type_map[name];
    }
    return guid;
}

/////////////////////ResourceManager/////////////////////

INSTANCE_IMP(ResourceManager)

ResourceManager::ResourceManager() {
    _resource_imp = std::make_shared<VmsResourceImp>();
    _rstatus_imp = std::make_shared<VmsResourceStatusImp>();
    _kvpair_imp = std::make_shared<VmsKvPairImp>();
    _assign_imp = std::make_shared<VmsResourceAssignmentImp>();
    _local_imp = std::make_shared<LocalResourceImp>();

    load();
}

void ResourceManager::load() {
    if (_self_node_id.empty()) {
        GET_CONFIG(std::string, mediaServerId, General::kMediaServerId);
        _self_node_id = mediaServerId;
    }
    if (_self_db_guid.empty()) {
        auto imp = std::make_shared<MiscDataImp>();
        auto ret = imp->findByKey(MISC_DATA_DB_INSTANCE_ID_KEY);
        if (!ret.empty()) {
            _self_db_guid = ret[0].value;
        }
    }
    CHECK(!_self_node_id.empty() && !_self_db_guid.empty());
}

/////////////////////ResourceManager/////////////////////

INSTANCE_IMP(ResourceStatusManager)

namespace {
const string kResourceStatusGapRetentionDays = "database.resource_status_gap_retention_days";
static onceToken s_status_gap_config([]() {
    mINI::Instance()[kResourceStatusGapRetentionDays] = 30;
});
}

ResourceStatusManager::ResourceStatusManager() {
    _gap_imp = std::make_shared<LocalResourceStatusGapImp>();
    GET_CONFIG(int, retention_days, kResourceStatusGapRetentionDays);
    _retention_seconds = retention_days > 0
        ? static_cast<uint64_t>(retention_days) * 24 * 3600
        : 0;
}

ResourceStatusManager::~ResourceStatusManager() = default;

bool ResourceStatusManager::onDisconnected(const std::string &resource_id, const std::string &reason, uint64_t disconnect_time) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (resource_id.empty() || disconnect_time == 0) {
        WarnL << "Invalid parameters for onDisconnected: resource_id=" << resource_id << ", reason=" << reason << ", disconnect_time=" << disconnect_time << ". Ignore";
        return false;
    }

    try {
        auto txn = _gap_imp->getExecutor()->beginWriteTxn();
        LocalResourceStatusGap gap;
        if (_gap_imp->findOpenGapWithTxn(resource_id, gap, txn)) {
            // A reason change is metadata for the same outage, not a new gap.
            if (gap.disconnect_reason != reason) {
                gap.disconnect_reason = reason;
                gap.updated_time = static_cast<uint64_t>(time(nullptr));
                if (!_gap_imp->updateWithTxn(gap, txn)) {
                    throw std::runtime_error("failed to update open status gap");
                }
            }
        } else {
            gap.gap_id = makeUuidStr();
            gap.resource_id = resource_id;
            gap.disconnect_time = disconnect_time;
            gap.disconnect_reason = reason;
            gap.state = static_cast<int>(ResourceStatus::OFFLINE);
            gap.updated_time = static_cast<uint64_t>(time(nullptr));
            if (!_gap_imp->addWithTxn(gap, txn)) {
                throw std::runtime_error("failed to create status gap");
            }
        }
        txn->commit();
        cleanupExpiredGaps(static_cast<uint64_t>(time(nullptr)));
        return true;
    } catch (const std::exception &ex) {
        WarnL << "Failed to record disconnection for resource " << resource_id << ": " << ex.what();
        return false;
    }
}

bool ResourceStatusManager::onReconnected(const std::string &resource_id, uint64_t reconnect_time) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (resource_id.empty() || reconnect_time == 0) {
        WarnL << "Invalid parameters for onReconnected: resource_id=" << resource_id << ", reconnect_time=" << reconnect_time << ". Ignore";
        return false;
    }

    try {
        auto txn = _gap_imp->getExecutor()->beginWriteTxn();
        LocalResourceStatusGap gap;
        if (!_gap_imp->findOpenGapWithTxn(resource_id, gap, txn)) {
            txn->commit();
            cleanupExpiredGaps(static_cast<uint64_t>(time(nullptr)));
            return true;
        }
        if (reconnect_time < gap.disconnect_time) {
            WarnL << "Reconnect time " << reconnect_time << " precedes disconnect time "
                  << gap.disconnect_time << " for resource " << resource_id << ". Ignore";
            return false;
        }
        gap.reconnect_time = reconnect_time;
        gap.updated_time = static_cast<uint64_t>(time(nullptr));
        if (!_gap_imp->updateWithTxn(gap, txn)) {
            throw std::runtime_error("failed to close open status gap");
        }
        txn->commit();
        cleanupExpiredGaps(static_cast<uint64_t>(time(nullptr)));
        return true;
    } catch (const std::exception &ex) {
        WarnL << "Failed to record reconnection for resource " << resource_id << ": " << ex.what();
        return false;
    }
}

std::vector<LocalResourceStatusGap> ResourceStatusManager::getResourceStatusGaps(const std::string &resource_id, uint64_t start_time, uint64_t end_time) {
    return _gap_imp->findByResourceIdAndTimeRange(resource_id, start_time, end_time);
};

std::vector<LocalResourceStatusGap> ResourceStatusManager::getResourceStatusGaps(const std::vector<std::string> &resource_ids, uint64_t start_time, uint64_t end_time) {
    return _gap_imp->findByResourceIdsAndTimeRange(resource_ids, start_time, end_time);
}

bool ResourceStatusManager::removeResourceStatusGaps(const std::string &resource_id, uint64_t threshold_time) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (resource_id.empty() || threshold_time == 0) {
        WarnL << "Invalid parameters for removeResourceStatusGaps: resource_id=" << resource_id << ", threshold_time=" << threshold_time << ". Ignore";
        return false;
    }
    try {
        _gap_imp->removeByThresholdTime(resource_id, threshold_time);
        DebugL << "Removed resource status gaps for resource " << resource_id << " with threshold time " << threshold_time;
        return true;
    } catch (const std::exception &ex) {
        WarnL << "Failed to remove resource status gaps for " << resource_id << ": " << ex.what();
        return false;
    }
}

void ResourceStatusManager::cleanupExpiredGaps(uint64_t now) {
    static const uint64_t kCleanupInterval = 3600;
    if (!_retention_seconds || now <= _retention_seconds ||
        (_last_cleanup_time && now - _last_cleanup_time < kCleanupInterval)) {
        return;
    }
    try {
        static const size_t kCleanupBatchSize = 100;
        const uint64_t threshold = now - _retention_seconds;
        MediaGapFillTaskImpl task_repository;
        MediaGapFillSessionImpl session_repository;
        size_t removed = 0;
        while (true) {
            auto gaps = _gap_imp->findClosedBefore(threshold, kCleanupBatchSize);
            if (gaps.empty()) break;
            auto transaction = _gap_imp->getExecutor()->beginWriteTxn();
            for (const auto &gap : gaps) {
                // Missing child rows are valid (for short or non-media gaps), so
                // only deletion of the owning status gap is mandatory.
                task_repository.removeByDeviceAndGapWithTxn(
                    gap.resource_id, gap.gap_id, transaction);
                session_repository.removeByDeviceAndGapWithTxn(
                    gap.resource_id, gap.gap_id, transaction);
                if (!_gap_imp->removeWithTxn(gap, transaction))
                    throw std::runtime_error("failed to delete expired status gap");
            }
            transaction->commit();
            removed += gaps.size();
            if (gaps.size() < kCleanupBatchSize) break;
        }
        _last_cleanup_time = now;
        if (removed != 0)
            InfoL << "Cleaned " << removed << " expired media status gaps and children";
    } catch (const std::exception &ex) {
        WarnL << "Failed to clean expired resource status gaps: " << ex.what();
    }
}

} // namespace managerkit
