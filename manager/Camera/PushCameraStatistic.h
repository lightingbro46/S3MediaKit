#ifndef CAMERA_PUSHCAMERASTATISTIC_H
#define CAMERA_PUSHCAMERASTATISTIC_H

#include <mutex>
#include "Local/FileRecorder.h"
#include "Common/SerialTaskQueue.h"
#include "RecordingStatistic.h"
#include "GenericPushCamera.h"

namespace managerkit {

struct PushCameraStatistic;
class PushCameraStatisticHelper {
public:
    static bool getParams(const std::string &json_str, PushCameraStatistic &stats);

    static std::string getParamsString(const PushCameraStatistic &stats);
};

struct PushCameraStatistic : public RecordingStatistic {
    PushCameraOption option;

    PushCameraStatistic() { schema = GENERIC_PUSH_CAMERA_SCHEMA; }
};

// Statistic of a camera pushing its stream (drone, mobile video push), saved as
// <record dir>/<id>/info.txt with schema generic_push_camera. Not synced to ESC.
class PushCameraStatisticImp : private PushCameraStatistic, public RecordingStatisticImp, public std::enable_shared_from_this<PushCameraStatisticImp> {
public:
    using Ptr = std::shared_ptr<PushCameraStatisticImp>;

    PushCameraStatisticImp(const std::string &src_path);

    ~PushCameraStatisticImp();

    std::string schema() override { return PushCameraStatistic::schema; }

    std::string deviceId() override;

    void setOnRemove(const std::function<void(const std::string &)> &cb) override { _on_remove = cb; }

    void setDeviceTuple(const DeviceTuple &input_tuple);

    void setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map);

    void setPushCameraOption(const PushCameraOption &input_option);

    PushCameraStatistic getParams();

    void remove();

private:
    RecordingStatistic &recordingData() override { return *this; }

    RecordingOption &recordingOption() override { return option; }

    void saveRecording() override { save(false); }

    void setup(const std::string &src_path);

    void load();

    // sync: config changes are written before returning; statistics are written later on _file_queue,
    // coalesced, as CameraStatisticImp does. Caller must hold _mtx.
    void save(bool sync);

    // Writes `snapshot` unless the file was removed or a newer snapshot is already on disk.
    void writeFile(const PushCameraStatistic &snapshot, uint64_t seq);

    void flushQueuedSave();

private:
    FileRecorder<PushCameraStatistic, PushCameraStatisticHelper>::Ptr _file;
    uint64_t _file_seq = 0;            // guarded by _mtx
    bool _file_save_queued = false;    // guarded by _mtx
    std::mutex _file_mtx;              // serializes info.txt writes and remove(); taken after _mtx
    uint64_t _file_written_seq = 0;    // guarded by _file_mtx
    bool _file_removed = false;        // guarded by _file_mtx
    SerialTaskQueue::Ptr _file_queue;
    std::function<void(const std::string &)> _on_remove;
};

} // namespace managerkit

#endif // CAMERA_PUSHCAMERASTATISTIC_H
