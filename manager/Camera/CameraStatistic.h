#ifndef CAMERA_CAMERESTATISTIC_H
#define CAMERA_CAMERESTATISTIC_H

#include "Util/TimeTicker.h"

namespace managerkit {
    
class StreamStorage {
public:
    StreamStorage(size_t bytes = 0, uint64_t oldestTimeBlock = 0) : _bytes(bytes), _oldestTimeBlock(oldestTimeBlock) {}
    ~StreamStorage() = default;

    /**
     * Add statistical bytes
     */
    StreamStorage &operator+=(size_t bytes) {
        _bytes += bytes;
        return *this;
    }

    /**
     * Subtract statistical bytes
     */
    StreamStorage &operator-=(size_t bytes) {
        _bytes -= bytes;
        return *this;
    }

    /**
     * Get speed, unit bytes/s
     */
    int getSize() {
        return _bytes;
    }


    /**
     * Updates the internal record of the oldest time block with the specified time value.
     * @param time The timestamp (in uint64_t) to set as the oldest time block.
     */
    void setOldestTimeBlock (uint64_t time) {
        _oldestTimeBlock = time;
    }

    /**
     * Get oldeset time block, unit second
     */
    uint64_t getOldestTimeBlock() { return _oldestTimeBlock; }

private:
    size_t _bytes;
    uint64_t _oldestTimeBlock;
    toolkit::Ticker _ticker;
};

} // namespace managerkit

#endif // CAMERA_CAMERESTATISTIC_H