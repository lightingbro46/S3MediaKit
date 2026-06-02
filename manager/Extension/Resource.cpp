#include "Resource.h"
#include "Util/util.h"
#include "Common/config.h"
#include "Storage/MiscData.h"

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

} // namespace managerkit
