#include "Resource.h"
#include "Util/util.h"

namespace managerkit {

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
}

template<typename Type, typename Helper>
void ResourceManager::addResourceProperty(Type resource) {

}

template<typename Type, typename Helper>
void ResourceManager::getResourceProperty(const std::string &guid) {

}

template<typename Type, typename Helper>
void ResourceManager::getAllResource(const std::string type) {
    
}

void ResourceManager::setResourceStatus(const std::string &guid, ResourceStatus status) {
    VmsResourceStatus r_status;
    r_status.guid = guid;
    r_status.status = (int)status;
    _rstatus_imp->add(r_status);
}

ResourceStatus ResourceManager::getResourceStatus(const std::string &guid) {
    auto ret = _rstatus_imp->findStatus(guid);
    return static_cast<ResourceStatus>(ret);
}

} // namespace managerkit
