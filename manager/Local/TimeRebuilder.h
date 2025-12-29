#ifndef LOCAL_TIMEREBUILDER_H_
#define LOCAL_TIMEREBUILDER_H_

#include "TimeRecorderManager.h"

namespace managerkit {

class TimeRebuilder final {
public:
    using Ptr = std::shared_ptr<TimeRebuilder>;
    using KeepTimeMap = std::unordered_map<std::string /*camera_id/stream_id*/, uint64_t /*keep_time*/>;

    TimeRebuilder(const std::string &src_path);

    ~TimeRebuilder() = default;

    /**
     * Recreate time file with callback
     */
    size_t rebuildTimeLine(const KeepTimeMap &map);

private:
    /**
     * Create template file 
     */
    void createTempFile();

    /**
     * Close time file and rename template file to offical name
     */
    void closeTempFile();

private:
    TimeRecorder::Ptr _writer;
    TimeRecorder::Ptr _recorder;
    std::string _src_path;
    std::string _full_path_tmp;
    std::string _full_path;
    bool _in_use = false;
};

class MultiTimeRebuilder {
public:
    using Ptr = std::shared_ptr<MultiTimeRebuilder>;
    using KeepTimeMap = TimeRebuilder::KeepTimeMap;

    MultiTimeRebuilder(const std::string &src_path);

    ~MultiTimeRebuilder() = default;

    /**
     * Recreate time file according to keep time map
     */
    size_t rebuildTimeLine(const KeepTimeMap &map);

private:
    /**
     * Open all time files under source path
     */    
    void openTimeFiles(const std::string &src_path);

private:
    std::string _src_path;
    std::map<uint64_t, std::string> _timefiles_map;
};

} // namespace managerkit

#endif //LOCAL_TIMEREBUILDER_H_