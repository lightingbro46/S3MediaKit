#ifndef MANAGER_RESOURCETYPE_H
#define MANAGER_RESOURCETYPE_H

#include <string>
#include <memory>
#include <unordered_map>

struct ResourceType {
    using Ptr = std::shared_ptr<ResourceType>;
    std::string guid;
    std::string name;
    std::string manufacture_id;
    std::string description;
    std::string parent_guid;

    ~ResourceType() {};
};

class ResourceTypeImp final {
public:
    using Ptr = std::shared_ptr<ResourceTypeImp>;

    //todo: add sqlite connection
    ResourceTypeImp();

    void addType(const ResourceType::Ptr &type) {
        auto key = type->guid;
        _xTypeMap[key] = type;
    }

    ResourceType::Ptr find(std::string &key) {
        if (_all_type_ready) {
            auto it = _xTypeMap.find(key);
            if (it != _xTypeMap.end()) {
                return it->second;
            }
        }
        return nullptr;
    }

    void onAllTypeReady() {
        _all_type_ready = true;
    }

private:
    bool _all_type_ready = false;
    std::unordered_map<std::string, ResourceType::Ptr> _xTypeMap;
};

#endif // MANAGER_RESOURCETYPE_H