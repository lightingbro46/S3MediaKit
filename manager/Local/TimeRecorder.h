#ifndef S3MEDIAKIT_TIMERECORDER_H_
#define S3MEDIAKIT_TIMERECORDER_H_

#include <mutex>
#include "TimeMuxer.h"

namespace managerkit {

class TimeRecorder final : public std::enable_shared_from_this<TimeRecorder> {
public:
    using Ptr = std::shared_ptr<TimeRecorder>;

    TimeRecorder(const std::string &path = "");

    ~TimeRecorder();

    /**
     * Input block, write block to file
     */
    bool inputBlock(const TimeBlock &block);

    /**
     * Get close time of current file
     */
    uint64_t getNextOpenTime() { return _next_open_time; }

    /**
     * Flush buffer in muxer and trigger record both file and memory
     */
    bool enableMemoryMuxer();

    /**
     * Get all block list on memory and close current muxer. TimeRecorder automatic open avaiable time file to record next time block
     */
    void getMemoryBlockAndRefresh(const std::function<void(const std::string &buf)> &on_data, const std::function<void()> &on_close);

private:

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
    std::mutex _mutex;
    std::string _path;
    std::string _full_path;
    TimeMuxer::Ptr _muxer;
    TimeMuxerMemory::Ptr _mem_muxer;
    uint64_t _next_open_time = 0;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMERECORDER_H_