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
     * Input block, write block to file if it is an SD playback block
     */
    bool inputSDBlock(const TimeBlock &block);

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

    /**
     * Start capturing blocks written to the current active file for rebuild.
     */
    bool beginRebuildCapture(const std::string &file, uint64_t &snapshot_size);

    /**
     * Drain captured blocks and mirror future input blocks to the temporary recorder.
     */
    bool drainRebuildCaptureAndStartMirror(const std::string &file,
                                           const Ptr &tmp_recorder,
                                           const std::function<void(const std::string &buf)> &on_data);

    /**
     * Stop rebuild mirroring and close the active file before the caller renames tmp over it.
     */
    void finishRebuildSwap(const std::string &file, const Ptr &tmp_recorder, const std::function<void()> &on_ready);

    /**
     * Close muxers synchronously.
     */
    void closeNow();

private:

    /**
     * Create file
     */
    void createFile();

    /**
     * Create file for SD card playback
     */
    void createSDFile(uint64_t date_time = 0);

    /**
     * Close file
     */
    void closeFile();

    void closeFileDirect();

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
    Ptr _rebuild_mirror_recorder;
    uint64_t _next_open_time = 0;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMERECORDER_H_
