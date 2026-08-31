#ifndef S3MEDIAKIT_TIMEFILEDIRTYTRACKER_H_
#define S3MEDIAKIT_TIMEFILEDIRTYTRACKER_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace managerkit {

/**
 * TimeFileDirtyTracker keeps, per device, the set of time files (.s3db) that have
 * been written since the last successful rebuild. The rebuild flow uses this set to
 * rebuild only the files that actually changed instead of scanning every file.
 *
 * A monotonically increasing version is kept per file so that blocks written while a
 * rebuild is in progress are not lost: the entry is only cleared when its version is
 * unchanged relative to the snapshot taken before the rebuild started.
 *
 * The dirty set is persisted to "<device_path>/.dirty" so it survives a restart.
 */
class TimeFileDirtyTracker {
public:
    struct DirtyEntry {
        std::string file_path;
        uint64_t version;
    };

    static TimeFileDirtyTracker &Instance();

    /**
     * Mark a time file dirty because a block was written to it.
     */
    void markDirty(const std::string &device_path, const std::string &file_path);

    /**
     * Snapshot the current dirty files of a device without clearing them.
     */
    std::vector<DirtyEntry> snapshot(const std::string &device_path);

    /**
     * Clear a file from the dirty set only when no new block was written since the
     * snapshot version was taken.
     */
    void clearIfUnchanged(const std::string &device_path, const std::string &file_path, uint64_t version);

    /**
     * Current dirty version of a file, or 0 when the file is not dirty. Used by the
     * rebuild simple path to detect concurrent writes before the destructive swap.
     */
    uint64_t currentVersion(const std::string &device_path, const std::string &file_path);

    /**
     * Load persisted dirty sets for every device directory under record_path.
     */
    void loadFromDisk(const std::string &record_path);

private:
    TimeFileDirtyTracker() = default;

    static std::string dirtyFilePath(const std::string &device_path);

    // caller must hold _mutex; persists the current dirty set of a device to disk
    void flushDeviceLocked(const std::string &device_path);

private:
    struct DeviceState {
        std::unordered_map<std::string /*file_path*/, uint64_t /*version*/> files;
        uint64_t next_version = 1;
    };

    std::mutex _mutex;
    std::unordered_map<std::string /*device_path*/, DeviceState> _devices;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMEFILEDIRTYTRACKER_H_
