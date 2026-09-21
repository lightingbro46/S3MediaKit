#ifndef S3MEDIAKIT_TIMERECORDERMANAGER_H_
#define S3MEDIAKIT_TIMERECORDERMANAGER_H_

#include "TimeRecorder.h"

namespace managerkit {

/**
 * TimeRecorderManager is responsible for managing multiple TimeRecorder instances.
 */
class TimeRecorderManager {
public:
    using Ptr = std::shared_ptr<TimeRecorderManager>;

    static TimeRecorderManager &Instance();

    ~TimeRecorderManager();

    bool addBlock(const TimeBlock &block);

    TimeRecorder::Ptr getRecorder(const std::string &device_id);

    /**
     * Close every timefile writer idle for longer than `record.timefileIdleCloseSec`, across all
     * devices; returns how many were closed. Blocking (it closes files), so it belongs on
     * BlockingExecutors::storage() — StorageManager's maintenance cycle calls it.
     *
     * A recorder is created per device and never removed while the process lives, so without this
     * every device the node has ever recorded keeps its `.s3db` + `.idx` handles open. That is most
     * visible after a failover: a camera moved back to its own node stays in this node's device list
     * (by design, until retention has cleared its data) and used to keep those two handles with it.
     */
    size_t closeIdleWriters();

private:
    TimeRecorderManager();

    TimeRecorder::Ptr addRecorder(const std::string &device_id);

    bool removeRecorder(const std::string &device_id);

private:
    std::mutex _mutex;
    std::unordered_map<std::string, TimeRecorder::Ptr> _recorders;
};

} // namespace managerkit

#endif //S3MEDIAKIT_TIMERECORDERMANAGER_H_
