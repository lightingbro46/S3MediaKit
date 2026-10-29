#include "DeviceStatistic.h"
#include "Common/DeviceSource.h"
#include "Common/StrUtil.h"

using namespace std;

namespace managerkit {

bool parseDeviceStatisticHeader(const string &json_str, DeviceStatisticHeader &header) {
    Json::Value root;
    if (!StrJsonUtils::readJsonString(json_str, root)) {
        return false;
    }
    return parseDeviceStatisticJson(root, header);
}

bool parseDeviceStatisticJson(const Json::Value &root, DeviceStatisticHeader &header) {
    if (!root.isObject()) {
        return false;
    }
    const Json::Value &schema = root["schema"];
    header.schema = schema.isString() && !schema.asString().empty() ? schema.asString() : GENERIC_RTSP_CAMERA_SCHEMA;
    const Json::Value &version = root["schemaVersion"];
    header.schema_version = version.isIntegral() ? version.asInt() : 1;
    const Json::Value &id = root["id"];
    header.id = id.isString() ? id.asString() : "";
    return true;
}

void writeDeviceStatisticHeader(const string &schema, Json::Value &root) {
    root["schema"] = schema;
    root["schemaVersion"] = kDeviceStatisticSchemaVersion;
}

} // namespace managerkit
