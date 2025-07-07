#ifndef S3MEDIAKIT_TIMERECORDER_H_
#define S3MEDIAKIT_TIMERECORDER_H_

#include <mutex>
#include "TimeMuxer.h"
#include "TimeMaker.h"

namespace mediakit {

class TimeRecorder final {
public:
    using Ptr = std::shared_ptr<TimeRecorder>;

    static TimeRecorder& Instance();

    ~TimeRecorder();

    /**
     * Input block
     */
    bool inputBlock(const TimeBlock &block);

private:
    TimeRecorder(const std::string &path = "");

    /**
     * Write block list to file
     */
    void flush();

    /**
     * Create file
     */
    void createFile();

    /**
     * Close file
     */
    void closeFile();

    /**
     * Asynchronous close file
     */
    void asyncClose();

private:
    std::recursive_mutex _mutex;
    std::string _folder_path;
    std::string _full_path;
    TimeMuxer::Ptr _muxer;
};

} // namespace mediakit

#endif // S3MEDIAKIT_TIMERECORDER_H_