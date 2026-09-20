#include "StoragePolicy.h"

#include <algorithm>

#include "Common/Parser.h"
#include "Common/StrUtil.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/util.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

uint64_t oldestSegmentDate(const string &record_path, const string &key) {
    string stream_path = record_path + "/" + key;
    uint64_t oldest = 0;
    File::scanDir(stream_path, [&oldest, stream_path](const string date_path, bool isDir) {
        if (!isDir) {
            return true;
        }
        string date_string = findSubString(date_path.data() + stream_path.size(), "/", nullptr);
        auto date_time = StrTimeUtils::getTsFromDateStr(date_string);
        if (date_time != 0 && (oldest == 0 || date_time < oldest)) {
            oldest = date_time;
        }
        return true;
    });
    return oldest;
}

uint64_t keepTimeThreshold(uint64_t min_value, uint64_t max_value, int keep_percent) {
    uint64_t newer = std::max(min_value, max_value);
    uint64_t older = std::min(min_value, max_value);
    keep_percent = std::min(100, std::max(0, keep_percent));
    return newer - (newer - older) * static_cast<uint64_t>(keep_percent) / 100;
}

uint64_t clampMaxToDisk(const string &record_path, const string &key, uint64_t max_value, bool is_auto) {
    if (!is_auto) {
        return max_value;
    }
    uint64_t floor_value = oldestSegmentDate(record_path, key);
    if (floor_value != 0 && floor_value < max_value) {
        DebugL << "Stream " << key << " clamps Auto archive floor from "
               << getTimeStr("%Y-%m-%d", max_value) << " down to " << getTimeStr("%Y-%m-%d", floor_value);
        return floor_value;
    }
    return max_value;
}

} // namespace managerkit
