#ifndef CAMERA_CAMERASTATISTIC_H
#define CAMERA_CAMERASTATISTIC_H

#include <mutex>
#include "Util/TimeTicker.h"
#include "Local/FileRecorder.h"
#include "StreamSource.h"
#include "GenericRtspCamera.h"
#include "proto/timeblock.pb.h"

namespace managerkit {

struct BookmarkStats {
    size_t recordAverageSizeB = 0;
    size_t recordCount = 0;
};

struct StreamStorageStats {
    size_t archiveIndexRecordCount = 0;
    size_t archiveSizeB = 0;
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct StreamStatistic {
    bool live = false;
    std::string status;
    int byte_speed = 0;
    bool has_video = false;
    std::string vcodec;
    int width = 0;
    int height = 0;
    int bitrate = 0;
    float fps = 0.0;
    bool has_audio = false;
    std::string acodec;
    int sample_rate = 0;
    int channel_no = 0;
    int sample_bit = 0;
};

struct CameraStatistic;
class CameraStatisticHelper {
public:
    static bool getParams(const std::string &json_str, CameraStatistic &stats);

    static std::string getParamsString(const CameraStatistic &stats);
};

struct CameraStatistic {
    CameraInfo info;
    CameraOption option;
    std::unordered_map<int, StreamTuple> stream_map;
    BookmarkStats bm;
    std::unordered_map<int, StreamStorageStats> storage_map;
    std::unordered_map<int, StreamStatistic> sinfo_map;
    uint64_t created_at;
    uint64_t updated_at;

    CameraStatistic() {
        storage_map[PrimaryStream];
        storage_map[SecondaryStream];
        sinfo_map[PrimaryStream];
        sinfo_map[SecondaryStream];
        created_at = 0;
        updated_at = 0;
    }
};

class CameraStatisticImp : public CameraStatistic {
public:
    using Ptr = std::shared_ptr<CameraStatisticImp>;

    CameraStatisticImp(const CameraInfo &info, const std::unordered_map<int, StreamTuple> &stream_map);
    ~CameraStatisticImp();

    void setCameraOption(const CameraOption &option);

    const CameraOption &getCameraOption();

    void addArchiveSize(std::string stream_id, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add = true);
    
    void addBookmarkCount(uint64_t bm_created_at,  size_t size, bool add = true);

    void addStreamStatistic(int stream_type, bool live, std::string status, const mediakit::TranslationInfo *info = nullptr);

    CameraStatistic getParams();

    static void addCameraArchiveSize(const TimeBlock &block, bool add = true);

    static void addCameraBookmarkCount(const std::string &camera_id, uint64_t created_at, bool add = true);

private:
    void setup();

    void load();

    void save() ;

    virtual void onSetCameraOption(const CameraOption &option) {}

private:
    std::recursive_mutex _mtx_stats;
    FileRecorder<CameraStatistic, CameraStatisticHelper>::Ptr _file;
};

} // namespace managerkit

#endif // CAMERA_CAMERASTATISTIC_H