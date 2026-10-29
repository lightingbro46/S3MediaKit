#ifndef CAMERA_RECORDINGSTATISTIC_H
#define CAMERA_RECORDINGSTATISTIC_H

#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include "json/json.h"
#include "Local/DeviceStatistic.h"
#include "Common/DeviceOption.h"
#include "StreamSource.h"

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
    uint64_t last_change_status = 0;
    std::string status;
    int byte_speed = 0;
    bool has_video = false;
    std::string vcodec;
    // Camera codec before ingest transcoding (e.g. JPEG while vcodec is H264).
    std::string source_vcodec;
    bool transcoded = false;
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

struct MotionStorageStats {
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct TierStorageStats {
    uint64_t archiveStartTime = 0;
    uint64_t archiveEndTime = 0;
};

struct TransportStats {
    std::vector<std::string> liveTransports;
    std::vector<std::string> replayTransports;
};

// Statistics of every recorded device. The option lives in the device's own statistic
// (CameraOption, PushCameraOption), both derive RecordingOption.
struct RecordingStatistic {
    std::string schema;
    DeviceTuple tuple;
    std::unordered_map<int, StreamTuple> stream_map;
    BookmarkStats bm;
    std::unordered_map<std::string, StreamStorageStats> storage_map;
    std::unordered_map<int, StreamStatistic> sinfo_map;
    MotionStorageStats motion_stats;
    TransportStats transport_stats;
    std::unordered_map<int, TierStorageStats> tier_storage_map;
    uint64_t created_at = 0;
    uint64_t updated_at = 0;
};

struct RecordingParams : public RecordingStatistic {
    RecordingOption option;
};

// info.txt keys shared by every recorded device; the keys are those of the camera info.txt, so files
// written before the split are read unchanged.
// Reads the tuple, the RecordingOption keys, streamUrls, created_at/updated_at and the recording
// entries of addParams.
void readRecordingJson(const Json::Value &root, RecordingStatistic &stats, RecordingOption &option);

// Writes what readRecordingJson() reads; the addParams entries are appended to `params`.
void writeRecordingJson(const RecordingStatistic &stats, const RecordingOption &option, Json::Value &root, Json::Value &params);

Json::Value makeRecordingStreamTupleJson(const std::unordered_map<int, StreamTuple> &stream_map, int stream_type);

StreamTuple readRecordingStreamTuple(const Json::Value &data, const DeviceTuple &tuple, int stream_type);

// Common part of every recorded device's statistic: archive size, bookmarks, stream statistics and
// retention thresholds, updated by device id from StatisticRecorder.
class RecordingStatisticImp : public DeviceStatisticBase {
public:
    using Ptr = std::shared_ptr<RecordingStatisticImp>;

    std::string getStreamID(const int &stream_type);

    void addArchiveSize(std::string stream_id, size_t count, size_t size, uint64_t archived_start_time, uint64_t archived_end_time, bool add = true);

    void addBookmarkCount(uint64_t bm_created_at, size_t size, bool add = true);

    virtual void addStreamStatistic(int stream_type, bool live, std::string status, const mediakit::TranslationInfo *info = nullptr);

    void addMotionKeepThreshold(bool start, uint64_t threshold);

    void addTierKeepThreshold(int tier_type, bool start, uint64_t threshold);

    RecordingParams getRecordingParams();

protected:
    // The derived statistic's data. Caller holds _mtx.
    virtual RecordingStatistic &recordingData() = 0;

    virtual RecordingOption &recordingOption() = 0;

    // Persist after a statistic changed. Caller holds _mtx.
    virtual void saveRecording() = 0;

    // Sets stream_map and creates the storage_map/sinfo_map entries of new streams. Caller holds _mtx.
    void applyStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map);

    // Updates sinfo_map[stream_type]. Returns false when the device has no such stream; `changed` is
    // false when only byte_speed (a measurement, not state to restore) changed. Caller holds _mtx.
    bool applyStreamStatistic(int stream_type, bool live, const std::string &status, const mediakit::TranslationInfo *info, bool &changed);

protected:
    mutable std::mutex _mtx;
};

} // namespace managerkit

#endif // CAMERA_RECORDINGSTATISTIC_H
