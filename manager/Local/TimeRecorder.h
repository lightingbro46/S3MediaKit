#ifndef S3MEDIAKIT_TIMERECORDER_H_
#define S3MEDIAKIT_TIMERECORDER_H_

#include <map>
#include <mutex>
#include <string>
#include "TimeMuxer.h"

namespace managerkit {

/**
 * TimeRecorder writes TimeBlocks belonging to a single device.
 *
 * A device receives two interleaved kinds of blocks:
 *   - Live blocks: produced in real time, always land in today's date file.
 *   - Import blocks: produced by the SD-card reader, may target any past date file.
 *
 * To avoid thrashing a single muxer between today's file and arbitrary past-date
 * files, the recorder keeps one open muxer per date file. Today's file is pinned
 * open; the past-date (import) files are kept in a small LRU cache and closed once
 * the cache is full.
 *
 * When constructed with a concrete .s3db file path (not a directory) the recorder
 * degrades to a single-file writer, used by the rebuild flow for temporary files.
 */
class TimeRecorder final : public std::enable_shared_from_this<TimeRecorder> {
public:
    using Ptr = std::shared_ptr<TimeRecorder>;

    TimeRecorder(const std::string &path = "");

    ~TimeRecorder();

    /**
     * Input block, write block to the proper date file (live or import).
     */
    bool inputBlock(const TimeBlock &block);

    /**
     * Start capturing blocks written to the given active file for rebuild.
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
     * Close all muxers synchronously.
     */
    void closeNow();

private:
    struct FileWriter {
        using Ptr = std::shared_ptr<FileWriter>;
        std::string full_path;
        TimeMuxer::Ptr muxer;
        uint64_t last_access = 0;
        // per-file rebuild capture state
        std::shared_ptr<TimeMuxerMemory> mem_muxer;
        TimeRecorder::Ptr mirror_recorder;
    };

    // caller must hold _mutex
    std::string resolveTargetPath(uint64_t block_time) const;
    bool isLivePath(const std::string &file_path) const;
    FileWriter::Ptr getOrCreateWriter(const std::string &file_path);
    void evictIdleImportWritersLocked();
    void closeAllLocked(bool async);

private:
    std::mutex _mutex;
    std::string _path;         // device directory, or a single .s3db file path
    bool _is_dir = false;
    // open writers keyed by full file path; today's file is pinned, imports are LRU
    std::map<std::string, FileWriter::Ptr> _writers;
    size_t _max_import_writers = 2;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMERECORDER_H_
