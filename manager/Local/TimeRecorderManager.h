#ifndef S3MEDIAKIT_TIMERECORDERMANAGER_H_
#define S3MEDIAKIT_TIMERECORDERMANAGER_H_

#include "TimeRecorder.h"

namespace managerkit {

enum class RecorderType { LOCAL, LOCAL_SD };

/**
 * TimeRecorderManager is responsible for managing multiple TimeRecorder instances.
 */
class TimeRecorderManager {
public:
    using Ptr = std::shared_ptr<TimeRecorderManager>;

    static TimeRecorderManager &Instance();

    ~TimeRecorderManager();

    bool addBlock(const TimeBlock &block);

    TimeRecorder::Ptr getRecorder(const std::string &device_id, RecorderType type  = RecorderType::LOCAL);

private:
    TimeRecorderManager();

    TimeRecorder::Ptr addRecorder(const std::string &device_id, RecorderType type  = RecorderType::LOCAL);

    bool removeRecorder(const std::string &device_id, RecorderType type  = RecorderType::LOCAL);

private:
    std::mutex _mutex;
    std::unordered_map<std::string, TimeRecorder::Ptr> _recorders;
    std::unordered_map<std::string, TimeRecorder::Ptr> _sd_recorders;
};

} // namespace managerkit

#endif //S3MEDIAKIT_TIMERECORDERMANAGER_H_