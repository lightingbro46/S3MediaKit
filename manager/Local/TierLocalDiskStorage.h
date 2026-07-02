#ifndef LOCAL_TIER_LOCAL_DISK_STORAGE_H
#define LOCAL_TIER_LOCAL_DISK_STORAGE_H

#include "TierFileStorageBase.h"

namespace managerkit {

class TierLocalDiskStorage : public TierFileStorageBase {
public:
    TierLocalDiskStorage() = default;
    ~TierLocalDiskStorage() override = default;

    static std::string makeLocalKey(const std::string &camera_id,
                                    const std::string &stream_id,
                                    const std::string &segment_path) {
        return makeStorageKey(camera_id, stream_id, segment_path);
    }

protected:
    const char *storageName() const override {
        return "TierLocalDiskStorage";
    }
};

} // namespace managerkit

#endif // LOCAL_TIER_LOCAL_DISK_STORAGE_H
