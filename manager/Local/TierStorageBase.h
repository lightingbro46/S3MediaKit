#ifndef LOCAL_TIER_STORAGE_BASE_H
#define LOCAL_TIER_STORAGE_BASE_H

#ifdef ENABLE_TIER_STORAGE

#include <string>

#include "Storage/StoragePool.h"

namespace managerkit {

class TierStorageBase {
public:
    static StoragePool poolFromJson(const Json::Value &body);

    static void applyPoolPatch(StoragePool &pool, const Json::Value &body);

    static bool validatePoolByType(const StoragePool &pool, std::string &error);

private:
    TierStorageBase();
};

} // namespace managerkit

#endif // ENABLE_TIER_STORAGE

#endif // LOCAL_TIER_STORAGE_BASE_H
