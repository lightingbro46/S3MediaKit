#ifndef LOCAL_STATISTICRECORDER_H
#define LOCAL_STATISTICRECORDER_H

#include "Camera/CameraStatistic.h"
#include "Speaker/SpeakerStatistic.h"
#include "Local/DeviceStatistic.h"

namespace managerkit {

class StatisticRecorder : public std::enable_shared_from_this<StatisticRecorder> {
public:
    using Ptr = std::shared_ptr<StatisticRecorder>;

    static StatisticRecorder &Instance();

    // Public for tests that need their own record directory; production uses Instance().
    StatisticRecorder(const std::string &record_path = "");

    ~StatisticRecorder();

    // Called before loadAll().
    void registerType(const DeviceStatisticType &type);

    // Loads every <record dir>/<id>/info.txt into the loader of its schema, then calls
    // onSavedLoaded() of every registered loader. Blocking (disk).
    void loadAll();

    template <typename T>
    std::shared_ptr<T> get(const std::string &device_id) {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        auto it = _stats_map.find(device_id);
        return it == _stats_map.end() ? nullptr : std::dynamic_pointer_cast<T>(it->second);
    }

    // nullptr when create_if_not_exist and the id already belongs to a device of another schema.
    CameraStatisticImp::Ptr getRecorder(const std::string &device_id, bool create_if_not_exist = true);

    bool removeRecorder(const std::string &device_id);

    SpeakerStatisticImp::Ptr getSpeakerRecorder(const std::string &device_id, bool create_if_not_exist = true);

    bool removeSpeakerRecorder(const std::string &device_id);

    void addArchiveSize(const std::string &device_id, const std::string &stream_id, size_t count, size_t size, uint64_t archive_start_time, uint64_t archive_end_time, bool add = true);

    void addBookmarkCount(const std::string &device_id, uint64_t created_at, bool add = true);

    void addDeviceCapabilities(const std::string &device_id, bool connected, const std::string &status, const DeviceCapabilities *device_caps = nullptr);

    void addStreamStatistic(const std::string &device_id, int stream_type, bool live, const std::string &status, const mediakit::TranslationInfo *info = nullptr);

    void addMotionKeepThreshold(const std::string &device_id, bool start, uint64_t threshold);

    void addTierKeepThreshold(const std::string &device_id, int tier_type, bool start, uint64_t threshold);

private:
    // Returns the statistic of device_id, creating it under the record dir if absent. nullptr when
    // the id is already used (in memory or by info.txt on disk) by a device of another schema:
    // loading would fail and the first save() would overwrite that device's file.
    template <typename T>
    std::shared_ptr<T> create(const std::string &schema, const std::string &device_id) {
        {
            std::lock_guard<std::mutex> lock(_mtx_stats);
            auto it = _stats_map.find(device_id);
            if (it != _stats_map.end()) {
                return castExisting<T>(it->second, schema, device_id);
            }
        }
        auto device_dir = _record_path + "/" + device_id;
        if (!savedSchemaMatches(device_dir, schema)) {
            return nullptr;
        }
        auto imp = std::make_shared<T>(device_dir);
        setOnRemove(imp);
        std::lock_guard<std::mutex> lock(_mtx_stats);
        auto ret = _stats_map.emplace(device_id, imp);
        return ret.second ? imp : castExisting<T>(ret.first->second, schema, device_id);
    }

    template <typename T>
    std::shared_ptr<T> castExisting(const DeviceStatisticBase::Ptr &stats, const std::string &schema, const std::string &device_id) {
        auto ret = std::dynamic_pointer_cast<T>(stats);
        if (!ret) {
            ErrorL << "Device id " << device_id << " is already used by a device of another schema, cannot create " << schema;
        }
        return ret;
    }

    template <typename T>
    bool remove(const std::string &device_id) {
        std::lock_guard<std::mutex> lock(_mtx_stats);
        auto it = _stats_map.find(device_id);
        if (it == _stats_map.end() || !std::dynamic_pointer_cast<T>(it->second)) {
            return false;
        }
        _stats_map.erase(it);
        return true;
    }

    bool savedSchemaMatches(const std::string &device_dir, const std::string &schema);

    void setOnRemove(const DeviceStatisticBase::Ptr &stats);

private:
    std::mutex _mtx_stats;
    std::string _record_path;
    std::unordered_map<std::string, DeviceStatisticType> _types;
    std::unordered_map<std::string, DeviceStatisticBase::Ptr> _stats_map;
};

} // namespace managerkit

#endif // LOCAL_STATISTICRECORDER_H