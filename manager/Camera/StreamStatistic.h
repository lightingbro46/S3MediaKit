#ifndef CAMERA_CAMERESTATISTIC_H
#define CAMERA_CAMERESTATISTIC_H

#include <mutex>
#include "Util/TimeTicker.h"

namespace managerkit {
    
class StreamStorage {
public:
    StreamStorage(size_t bytes = 0, uint64_t oldestTimeBlock = 0, uint64_t lastestTimeBlock = 0) 
                : _bytes(bytes), _oldestTimeBlock(oldestTimeBlock), _latestTimeBlock(lastestTimeBlock) {}
    ~StreamStorage() = default;

    /**
     * Add statistical bytes
     */
    StreamStorage &operator+=(size_t bytes) {
        std::lock_guard<std::mutex> lck(_mtx);
        _bytes += bytes;
        return *this;
    }

    /**
     * Subtract statistical bytes
     */
    StreamStorage &operator-=(size_t bytes) {
        std::lock_guard<std::mutex> lck(_mtx);
        _bytes -= bytes;
        return *this;
    }

    /**
     * Get speed, unit bytes/s
     */
    int getSize() {
        std::lock_guard<std::mutex> lck(_mtx);
        return _bytes;
    }


    /**
     * Updates the internal record of the oldest time block with the specified time value.
     * @param time The timestamp (in uint64_t) to set as the oldest time block.
     */
    void setOldestTimeBlock (uint64_t time) {
        std::lock_guard<std::mutex> lck(_mtx);
        _oldestTimeBlock = time;
    }

    /**
     * Get oldeset time block, unit second
     */
    uint64_t getOldestTimeBlock() { 
        std::lock_guard<std::mutex> lck(_mtx);
        return _oldestTimeBlock; 
    }

    /**
     * Updates the internal record of the latest time block with the specified time value.
     * @param time The timestamp (in uint64_t) to set as the latest time block.
     */
    void setLastestTimeBlock (uint64_t time) {
        std::lock_guard<std::mutex> lck(_mtx);
        _latestTimeBlock = time;
    }

    /**
     * Get latest time block, unit second
     */
    uint64_t getLastestTimeBlock() { 
        std::lock_guard<std::mutex> lck(_mtx);
        return _latestTimeBlock; 
    }

private:
    std::mutex _mtx;
    size_t _bytes;
    uint64_t _oldestTimeBlock;
    uint64_t _latestTimeBlock;
    toolkit::Ticker _ticker;
};

} // namespace managerkit

#endif // CAMERA_CAMERESTATISTIC_H