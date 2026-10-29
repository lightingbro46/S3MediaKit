#include "PushCameraStatistic.h"
#include <cstdint>
#include "Common/StrUtil.h"
#include "Common/BlockingExecutors.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

// ################### PushCameraStatisticHelper ###########################

// Inverse of getOriginTypeString(); unknown for a name this build does not know.
static DeviceOriginType originTypeFromString(const string &name) {
    for (int i = 0; i <= UINT8_MAX; ++i) {
        auto type = static_cast<DeviceOriginType>(i);
        if (getOriginTypeString(type) == name) {
            return type;
        }
    }
    return DeviceOriginType::unknown;
}

bool PushCameraStatisticHelper::getParams(const string &json_str, PushCameraStatistic &stats) {
    Json::Value ret;
    DeviceStatisticHeader header;
    if (!StrJsonUtils::readJsonString(json_str, ret) || !parseDeviceStatisticJson(ret, header)) {
        WarnL << "Parse json string failed";
        return false;
    }
    if (header.schema != GENERIC_PUSH_CAMERA_SCHEMA) {
        WarnL << "Not a push camera statistic: " << header.id << " has schema " << header.schema;
        return false;
    }
    stats.schema = header.schema;
    readRecordingJson(ret, stats, stats.option);
    stats.option.originType = originTypeFromString(ret["originType"].asString());
    stats.option.enableActive = ret["enableActive"].asBool();
    return true;
}

string PushCameraStatisticHelper::getParamsString(const PushCameraStatistic &stats) {
    Json::Value root;
    Json::Value params = Json::arrayValue;
    writeRecordingJson(stats, stats.option, root, params);
    root["originType"] = getOriginTypeString(stats.option.originType);
    root["enableActive"] = stats.option.enableActive;
    root["addParams"] = params;
    return root.toStyledString();
}

// ################### PushCameraStatisticImp ###########################

PushCameraStatisticImp::PushCameraStatisticImp(const std::string &src_path) {
    CHECK(!src_path.empty(), "Source path cannot be empty");
    _file_queue = std::make_shared<SerialTaskQueue>(BlockingExecutors::api(), "push camera " + src_path);
    setup(src_path);
}

PushCameraStatisticImp::~PushCameraStatisticImp() {
    // A coalesced write still queued would be dropped (its task only holds a weak_ptr).
    if (_file_save_queued) {
        try {
            writeFile(static_cast<const PushCameraStatistic &>(*this), _file_seq);
        } catch (const std::exception &e) {
            WarnL << "Failed to save push camera statistic " << tuple.shortUrl() << ": " << e.what();
        }
    }
}

void PushCameraStatisticImp::setup(const string &src_path) {
    auto file_path = src_path;
    if (!end_with(file_path, "/info.txt")) {
        file_path += "/info.txt";
    }
    _file = std::make_shared<FileRecorder<PushCameraStatistic, PushCameraStatisticHelper>>(file_path);
    if (!_file->empty()) {
        load();
    }
}

void PushCameraStatisticImp::load() {
    PushCameraStatistic saved_stats;
    if (_file->load(saved_stats)) {
        static_cast<PushCameraStatistic &>(*this) = saved_stats;
    }
}

void PushCameraStatisticImp::save(bool sync) {
    if (created_at == 0) {
        created_at = time(nullptr);
    }
    updated_at = time(nullptr);
    if (sync) {
        writeFile(static_cast<const PushCameraStatistic &>(*this), ++_file_seq);
        return;
    }
    ++_file_seq;
    if (!_file_save_queued) {
        _file_save_queued = true;
        std::weak_ptr<PushCameraStatisticImp> weak_self = shared_from_this();
        _file_queue->post([weak_self]() {
            auto self = weak_self.lock();
            if (self) {
                self->flushQueuedSave();
            }
        });
    }
}

void PushCameraStatisticImp::writeFile(const PushCameraStatistic &snapshot, uint64_t seq) {
    std::lock_guard<std::mutex> lck(_file_mtx);
    if (_file_removed || seq <= _file_written_seq) {
        return;
    }
    _file->save(snapshot);
    _file_written_seq = seq;
}

void PushCameraStatisticImp::flushQueuedSave() {
    PushCameraStatistic snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        if (!_file_save_queued) {
            return;
        }
        _file_save_queued = false;
        snapshot = static_cast<const PushCameraStatistic &>(*this);
        seq = _file_seq;
    }
    try {
        writeFile(snapshot, seq);
    } catch (const std::exception &e) {
        WarnL << "Failed to save push camera statistic " << snapshot.tuple.shortUrl() << ": " << e.what();
    }
}

std::string PushCameraStatisticImp::deviceId() {
    std::lock_guard<std::mutex> lck(_mtx);
    return tuple.device_id;
}

void PushCameraStatisticImp::setDeviceTuple(const DeviceTuple &input_tuple) {
    std::lock_guard<std::mutex> lck(_mtx);
    tuple = input_tuple;
    save(true);
}

void PushCameraStatisticImp::setStreamTuples(const std::unordered_map<int, StreamTuple> &input_stream_map) {
    std::lock_guard<std::mutex> lck(_mtx);
    applyStreamTuples(input_stream_map);
    save(true);
}

void PushCameraStatisticImp::setPushCameraOption(const PushCameraOption &input_option) {
    std::lock_guard<std::mutex> lck(_mtx);
    option = input_option;
    save(true);
}

PushCameraStatistic PushCameraStatisticImp::getParams() {
    std::lock_guard<std::mutex> lck(_mtx);
    return static_cast<const PushCameraStatistic &>(*this);
}

void PushCameraStatisticImp::remove() {
    std::string device_id;
    {
        std::lock_guard<std::mutex> lck(_mtx);
        std::lock_guard<std::mutex> file_lck(_file_mtx);
        _file_removed = true; // a write already queued must not recreate the file
        _file->remove();
        device_id = tuple.device_id;
        DebugL << "Removed file recorder success: " << tuple.shortUrl();
    }
    if (_on_remove) {
        _on_remove(device_id);
    }
}

} // namespace managerkit
