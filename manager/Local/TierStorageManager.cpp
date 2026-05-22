#include "TierStorageManager.h"
#include "Common/config.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

std::string getTierTypeString(int type) {
    switch (type) {
        case HotTier:
            return "HotTier";
        case WarmTier:
            return "WarmTier";
        case ColdTier:
            return "ColdTier";
        default:
            return "UnknownTier";
    }
}

} // namespace managerkit