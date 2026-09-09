#ifndef LOCAL_TIER_STORAGE_STATS_H
#define LOCAL_TIER_STORAGE_STATS_H

#include <cstdint>
#include <string>

namespace managerkit {

struct TierPoolStats {
    bool        is_online           = false;
    bool        capacity_available  = false;
    bool        traffic_available   = false;
    uint64_t    total_bytes         = 0;
    uint64_t    used_bytes          = 0;
    uint64_t    free_bytes          = 0;
    uint64_t    rx_bytes            = 0;
    uint64_t    tx_bytes            = 0;
    std::string capacity_status     = "UNAVAILABLE";
    std::string capacity_message;
};

} // namespace managerkit

#endif // LOCAL_TIER_STORAGE_STATS_H
