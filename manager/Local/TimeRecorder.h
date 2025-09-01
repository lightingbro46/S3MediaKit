#ifndef S3MEDIAKIT_TIMERECORDER_H_
#define S3MEDIAKIT_TIMERECORDER_H_

#include <mutex>
#include "TimeMuxer.h"
#include "TimeMaker.h"

namespace mediakit {

class TimeRecorder final : public std::enable_shared_from_this<TimeRecorder> {
public:
    friend class TimeRebuilder;
    using Ptr = std::shared_ptr<TimeRecorder>;

    static TimeRecorder& Instance();

    TimeRecorder(const std::string &path = "");

    ~TimeRecorder();

    /**
     * Input block, write block to file
     */
    bool inputBlock(const TimeBlock &block);

    /**
     * Get current file path to store timeline
     */
    std::string getFilePath() { return _full_path; }

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
    std::recursive_mutex _mutex;
    std::string _path;
    std::string _full_path;
    uint32_t _file_index = 0;
    TimeMuxer::Ptr _muxer;
    TimeMuxerMemory::Ptr _mem_muxer;
};

class TimeRebuilder {
public:
    using Ptr = std::shared_ptr<TimeRebuilder>;
    using KeepTimeMap = std::unordered_map<std::string /*camera_id/stream_id*/, uint64_t /*keep_time*/>;

    TimeRebuilder(TimeRecorder::Ptr &recorder);

    ~TimeRebuilder() = default;

    /**
     * Recreate time file with callback
     */
    size_t rebuildTimeLine(KeepTimeMap &map, size_t space_reclaim);

private:
    /**
     * Create template file 
     */
    void createTempFile();

    /**
     * Close time file and rename 
     */
    void closeTempFile();

    /**
     * Rename template file to offical name
     */
    void commitTempFile();

private:
    TimeRecorder::Ptr _writer;
    std::weak_ptr<TimeRecorder> _weak_recorder;
    std::string _src_path;
    std::string _full_path_tmp;
    std::string _full_path;
};

} // namespace mediakit

#endif // S3MEDIAKIT_TIMERECORDER_H_