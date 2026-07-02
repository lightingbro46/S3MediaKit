#ifndef LOCAL_TIER_NAS_STORAGE_H
#define LOCAL_TIER_NAS_STORAGE_H

#include "TierFileStorageBase.h"

namespace managerkit {

/**
 * TierNASStorage
 *
 * Network-path storage helper for NAS pools. A NAS pool is expected to expose
 * a usable filesystem path through network_path or mount_path. Protocol
 * mounting/authentication is handled outside this class.
 */
class TierNASStorage : public TierFileStorageBase {
public:
    TierNASStorage() = default;
    ~TierNASStorage() override = default;

    static std::string makeNASKey(const std::string &camera_id,
                                  const std::string &stream_id,
                                  const std::string &segment_path) {
        return makeStorageKey(camera_id, stream_id, segment_path);
    }

protected:
    const char *storageName() const override {
        return "TierNASStorage";
    }

    bool validatePoolPath(const std::string &base_path,
                          std::string &out_message) const override;
};

} // namespace managerkit

#endif // LOCAL_TIER_NAS_STORAGE_H
