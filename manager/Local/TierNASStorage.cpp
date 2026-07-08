#include "TierNASStorage.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

bool TierNASStorage::validatePoolPath(const std::string &base_path,
                                      std::string &out_message, int &out_latency_ms) const {
    // NAS currently requires the network path to be mounted and writable.
    // Keep this override as the NAS-specific hook for stale-mount detection,
    // timeout/retry, or protocol-aware probes.
    return TierFileStorageBase::validatePoolPath(base_path, out_message, out_latency_ms);
}

} // namespace managerkit
