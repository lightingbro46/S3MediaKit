#ifndef LOCAL_TIMEMAKER_H
#define LOCAL_TIMEMAKER_H

#include <memory>
#include "FileMaker.h"

namespace managerkit {

struct BlockListIndexEntry {
    uint64_t start_time;
    uint64_t offset;
} __attribute__((packed));

class TimeMaker : public FileMaker<BlockListIndexEntry> {
public:
    using Ptr = std::shared_ptr<TimeMaker>;

    bool inputData(uint64_t &block_time, size_t &block_size);

    void setLastOffset(uint32_t offset) { _last_offset = offset; }

private:
    uint64_t getStampOfEntry(BlockListIndexEntry &entry) override;

private:
    uint32_t _last_offset = 0;
};

} // namespace managerkit

#endif // LOCAL_TIMEMAKER_H