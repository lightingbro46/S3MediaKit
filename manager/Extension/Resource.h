#ifndef COMMON_RESOURCE_H
#define COMMON_RESOURCE_H

#include <string>
#include <mutex>
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsResourceType.h"
#include "Storage/VmsKvPair.h"
#include "Storage/VmsResourceAssignment.h"
#include "Storage/LocalResource.h"

namespace managerkit {

#define RESOURCE_TYPE_USER "User"
#define RESOURCE_TYPE_SERVER "Server"
#define RESOURCE_TYPE_CAMERA "Camera"
#define RESOURCE_TYPE_SPEAKER "Speaker"

class ResourceTypeManager {
public:
    static ResourceTypeManager &Instance();

    std::string getResourceTypeGuid(const std::string &name);

private:
    ResourceTypeManager();

    void load();

private:
    std::unordered_map<std::string, std::string> _resource_type_map;
};

enum class ResourceStatus : uint8_t {
    UNKNOWN = 0,
    OFFLINE = 1,
    ONLINE = 2,
    UNAUTHORIZED = 3,
};

std::string getResourceStatusString(ResourceStatus status);

template<typename T>
struct ResourceAdapter {
    virtual ~ResourceAdapter() = default;

    std::string getXtypeId() {
        throw std::runtime_error("getXtypeId not implemented for this type" + std::string(typeid(T).name()));
    }

    VmsResource toVmsResource(const T &data) {
        throw std::runtime_error("toVmsResource not implemented for this type" + std::string(typeid(T).name()));
    }

    std::vector<VmsKvPair> toKvPairs(const T &data) {
        throw std::runtime_error("toKvPairs not implemented for this type" + std::string(typeid(T).name()));
    }

    std::vector<LocalResource> toLocalProps(const T &data) {
        throw std::runtime_error("toLocalProps not implemented for this type" + std::string(typeid(T).name()));
    }

    T fromVmsResource(const VmsResource &data, const std::vector<VmsKvPair> &kvs, const std::vector<LocalResource> &props) {
        throw std::runtime_error("fromVmsResource not implemented for this type" + std::string(typeid(T).name()));
    }
};

class ResourceManager {
public:
    static ResourceManager &Instance();
    ~ResourceManager() = default;

    // Upsert: VmsResource + VmsKvPair (synced) + LocalResourceProperty (local)
    // Thread-safe: serializes multi-table writes for the same resource guid.
    template <typename T>
    void addResource(T data, bool append_log = true) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto ret = ResourceAdapter<T>::toVmsResource(data);
        _resource_imp->add(ret, append_log);

        auto kvs = ResourceAdapter<T>::toKvPairs(data);
        for (auto &kv : kvs) {
            kv.resource_guid = ret.guid;   // ensure guid set
        }
        _kvpair_imp->addBatch(kvs, append_log);

        for (auto prop : ResourceAdapter<T>::toLocalProps(data)) {
            prop.resource_id = ret.guid;
            _local_imp->add(prop);
        }
        DebugL << "Upsert resource with guid " << ret.guid;
    }

    // Remove: VmsResource + VmsKvPair (synced) + LocalResourceProperty (local)
    // Thread-safe: serializes multi-table deletes for the same resource guid.
    void removeResource(const std::string &guid) {
        std::lock_guard<std::mutex> lk(_mtx);
        _local_imp->remove(guid); // local-only first
        _rstatus_imp->remove(guid);
        _kvpair_imp->remove(guid);
        _assign_imp->remove(guid);
        _resource_imp->remove(guid);
        DebugL << "Removed resource with guid " << guid;
    }

    // Get: VmsResource + VmsKvPair (synced) + LocalResourceProperty (local).
    // Locked to ensure a consistent multi-table snapshot per resource guid
    // (prevents reading a half-written resource from a concurrent addResource).
    template<typename T>
    bool getResource(const std::string &guid, T &out) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto ret = _resource_imp->findByGuid(guid);
        if (ret.empty()) return false;
        auto kvs   = _kvpair_imp->findAllKeyValue(guid);
        auto props = _local_imp->findAllProperty(guid);
        out = ResourceAdapter<T>::fromVmsResource(ret[0], kvs, props);
        return true;
    }

    // Lấy tất cả resource theo xtype_guid (phân biệt loại thiết bị)
    template<typename T>
    void getAllResource(const std::string peer_id, std::vector<T> &out) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto xtype_id = ResourceAdapter<T>::getXtypeId();
        auto ret = _resource_imp->findByParentGuidAndXType(peer_id, xtype_id);
        for (const auto &res : ret) {
            auto kvs   = _kvpair_imp->findAllKeyValue(res.guid);
            auto props = _local_imp->findAllProperty(res.guid);
            out.push_back(ResourceAdapter<T>::fromVmsData(res, kvs, props));
        }
    }

    void setResourceStatus(const std::string &guid, ResourceStatus status) {
        std::lock_guard<std::mutex> lk(_mtx);
        VmsResourceStatus r_status;
        r_status.guid = guid;
        r_status.status = static_cast<int>(status);
        _rstatus_imp->add(r_status);
        DebugL << "Set resource status to " << getResourceStatusString(status) << " for resource with guid " << guid;
    }

    ResourceStatus getResourceStatus(const std::string &guid) {
        return static_cast<ResourceStatus>(_rstatus_imp->findStatus(guid));
    }

    // Thread-safe: the mutex prevents two concurrent assignResource calls for
    // the same resource_guid from both seeing empty → both inserting a new row.
    void assignResource(const std::string &resource_guid, const std::string &peer_id, const std::string &db_guid, ResourceAssignType type) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto current = _assign_imp->findCurrentAssignment(resource_guid);
        if (!current.empty()) {
            auto assign = current[0];
            if (assign.owner_peer_id == peer_id) {
                InfoL << "Resource " << resource_guid << " is already assigned to the same peer " << peer_id;
                return; // already assigned to the same peer, no-op
            }
            WarnL << "Resource " << resource_guid << " is already assigned to peer " << assign.owner_peer_id << ", release it before re-assigning";
            // release current assignment
            assign.released_at = static_cast<int64_t>(time(nullptr));
            _assign_imp->update(assign);
        }

        VmsResourceAssignment assign;
        assign.resource_guid  = resource_guid;
        assign.owner_peer_id  = peer_id;
        assign.owner_db_guid  = db_guid;
        assign.assigned_at    = static_cast<int64_t>(time(nullptr));
        assign.released_at    = 0;
        assign.assign_type    = static_cast<int>(type);
        _assign_imp->add(assign);
        InfoL << "Assigning resource " << resource_guid << " to peer " << peer_id;
    }

    void releaseResource(const std::string &resource_guid, const std::string &peer_id) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto current = _assign_imp->findCurrentAssignment(resource_guid);
        if (!current.empty()) {
            auto assign = current[0];
            if (assign.owner_peer_id != peer_id) {
                WarnL << "Resource " << resource_guid << " is assigned to peer " << assign.owner_peer_id << ", cannot be released by peer " << peer_id;
                return; // assigned to a different peer, cannot release
            }
            assign.released_at = static_cast<int64_t>(time(nullptr));
            _assign_imp->update(assign);
            InfoL << "Resource " << resource_guid << " is already released from peer " << assign.owner_peer_id;
            return;
        }
        WarnL << "Resource " << resource_guid << " is not currently assigned, cannot be released";
    }

    bool getCurrentResourceAssignment(const std::string &resource_guid, VmsResourceAssignment &out) {
        std::lock_guard<std::mutex> lk(_mtx);
        auto current = _assign_imp->findCurrentAssignment(resource_guid);
        if (!current.empty()) {
            out = current[0];
            return true;
        }
        return false;
    }

    const std::string getSelfNodeId() const { return _self_node_id; }

    const std::string getSelfDbGuid() const { return _self_db_guid; }

private:
    ResourceManager();

    void load();

private:
    // Serializes all multi-table write and logical-read operations.
    // Per-table static mutexes in each ImpClass remain in place for callers
    // that bypass ResourceManager (e.g. SyncManager direct table writes).
    mutable std::mutex _mtx;

    VmsResourceImp::Ptr _resource_imp;
    VmsResourceStatusImp::Ptr _rstatus_imp;
    VmsKvPairImp::Ptr _kvpair_imp;
    LocalResourceImp::Ptr _local_imp;
    VmsResourceAssignmentImp::Ptr _assign_imp;

    std::string _self_node_id;
    std::string _self_db_guid;
};

} // namespace managerkit

#endif // COMMON_RESOURCE_H