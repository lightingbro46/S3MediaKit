#ifndef COMMON_RESOURCE_H
#define COMMON_RESOURCE_H

#include <string>
#include "Storage/VmsResource.h"
#include "Storage/VmsResourceStatus.h"
#include "Storage/VmsResourceType.h"
#include "Storage/VmsKvPair.h"

namespace managerkit {

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
    UNAUTHORIZED = 3
};

class ResourceManager {
public:
    static ResourceManager &Instance();
    ~ResourceManager() = default;

    template <typename Type, typename Helper>
    void addResourceProperty(Type resource);

    template <typename Type, typename Helper>
    void getResourceProperty(const std::string &guid);

    template <typename Type, typename Helper>
    void getAllResource(const std::string type);

    void setResourceStatus(const std::string &guid, ResourceStatus state);

    ResourceStatus getResourceStatus(const std::string &guid);

private:
    ResourceManager();

private:
    std::recursive_mutex _mtx;
    VmsResourceImp::Ptr _resource_imp;
    VmsResourceStatusImp::Ptr _rstatus_imp;
    VmsKvPairImp::Ptr _kvpair_imp;
};

} // namespace managerkit

#endif // COMMON_RESOURCE_H