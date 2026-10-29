#ifndef LOCAL_DEVICESTATISTIC_H
#define LOCAL_DEVICESTATISTIC_H

#include <functional>
#include <memory>
#include <string>
#include "json/json.h"

namespace managerkit {

// Highest info.txt "schemaVersion" this build reads. A file written with a newer version is kept
// but not loaded.
static constexpr int kDeviceStatisticSchemaVersion = 1;

// Common part of every statistic saved as <record dir>/<device id>/info.txt.
class DeviceStatisticBase {
public:
    using Ptr = std::shared_ptr<DeviceStatisticBase>;

    virtual ~DeviceStatisticBase() = default;

    // Fixed at construction, safe to call without any lock.
    virtual std::string schema() = 0;

    virtual std::string deviceId() = 0;

    virtual void setOnRemove(const std::function<void(const std::string &)> &cb) = 0;
};

// The manager that receives the saved devices at startup.
class ISavedDeviceLoader {
public:
    virtual ~ISavedDeviceLoader() = default;

    virtual void addSaved(const DeviceStatisticBase::Ptr &stats) = 0;

    // StatisticRecorder::loadAll() finished, also when no file of this type was found.
    virtual void onSavedLoaded() = 0;
};

// One device type: the data format (create) is separate from the manager owning it (loader),
// several formats may share a manager and the reverse.
struct DeviceStatisticType {
    std::string schema;
    std::function<DeviceStatisticBase::Ptr(const std::string &device_dir)> create;
    std::weak_ptr<ISavedDeviceLoader> loader;
};

struct DeviceStatisticHeader {
    std::string schema; // a file without "schema" is a generic_rtsp_camera
    int schema_version = 1;
    std::string id;
};

// false when the JSON is broken.
bool parseDeviceStatisticHeader(const std::string &json_str, DeviceStatisticHeader &header);

// Same, on an already parsed info.txt.
bool parseDeviceStatisticJson(const Json::Value &root, DeviceStatisticHeader &header);

// Writes "schema" and "schemaVersion".
void writeDeviceStatisticHeader(const std::string &schema, Json::Value &root);

} // namespace managerkit

#endif // LOCAL_DEVICESTATISTIC_H
