#ifndef LOCAL_MOTIONBLOCKMANAGER_H
#define LOCAL_MOTIONBLOCKMANAGER_H

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace managerkit {

class MotionBlockManager {
public:
    using Ptr = std::shared_ptr<MotionBlockManager>;

    static MotionBlockManager &Instance();

    ~MotionBlockManager() = default;

    /**
     * Delete expired motion block files according to threshold map, key is device_id, value is timestamp of last access or creation
     */
    void removeExpiredMotionBlocks(const std::unordered_map<std::string, uint64_t> &blocks_map);

    /**
     * Delete one device's daily motion pair. Missing files are success so a durable cleanup job can retry safely after a crash.
     */
    void removeMotionBlockDays(const std::string &device_id, const std::vector<uint64_t> &day_starts);

private:
    MotionBlockManager(const std::string &file_path = "", int max_hour_retention = 4320);

    /**
     * Delete expired motion block files according to threshold
     */
    void removeExpiredMotionBlocks(const std::string &key, uint64_t &value);

    /**
     * Emit event to get motion block threshold for a device, and update threshold map with returned value
     */
    void emitEvent(const std::string &key, bool start, uint64_t &threshold);

    /**
     * Get the motion block file path for a device and day. The day is the start of the day in seconds since epoch.
     */
    std::string getMotionBlockPath(const std::string &device_id, uint64_t day_start) const;

    /**
     * Refresh the motion block threshold for a device by scanning its motion block directory and finding the earliest timestamp. This will emit an event to notify listeners of the new threshold.
     */
    void refreshMotionKeepThreshold(const std::string &device_id);

private:
    std::string _record_path;
    int _max_hour;
};

} // namespace managerkit

#endif // LOCAL_MOTIONBLOCKMANAGER_H