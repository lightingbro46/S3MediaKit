#ifndef LOCAL_TIERSTORAGEMANAGER_H
#define LOCAL_TIERSTORAGEMANAGER_H

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace managerkit {

typedef enum {
    HotTier = 0,
    WarmTier = 1,
    ColdTier = 2,
    TierMax
} TierType;

std::string getTierTypeString(int type);

} // namespace managerkit

#endif // LOCAL_TIERSTORAGEMANAGER_H                                                                                                                                                                                        