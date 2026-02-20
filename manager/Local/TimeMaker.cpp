#include <iomanip>
#include "TimeMaker.h"
#include "Common/config.h"
#include "Util/util.h"
#include "Common/StrUtil.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

////////////////////// TimeMaker ////////////////////////

bool TimeMaker::inputData(uint64_t &block_time, size_t &block_size) {
    auto last_minute = StampUtils::getStartOfMinute(block_time);
    if (last_minute > getLastStamp()) {
        BlockListIndexEntry entry;
        entry.start_time = last_minute;
        entry.offset = _last_offset;
        
        inputEntry(entry);
        setLastStamp(last_minute);
    }
    _last_offset += block_size;
    return true;
}

uint64_t TimeMaker::getStampOfEntry(BlockListIndexEntry &entry) {
    return entry.start_time;
}

} // namespace managerkit