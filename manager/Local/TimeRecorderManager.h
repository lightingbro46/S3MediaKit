#ifndef S3MEDIAKIT_TIMERECORDERMANAGER_H_
#define S3MEDIAKIT_TIMERECORDERMANAGER_H_

#include "TimeRecorder.h"

using namespace mediakit;

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

private:
    TimeRecorderManager();

    TimeRecorder::Ptr addRecorder(const std::string &device_id);

    bool removeRecorder(const std::string &device_id);

private:
    std::mutex _mutex;
    std::unordered_map<std::string, mediakit::TimeRecorder::Ptr> _recorders;
};

} // namespace managerkit

#endif //S3MEDIAKIT_TIMERECORDERMANAGER_H_