#include "StorageManager.h"
#include "Util/util.h"
#include "Util/File.h"
#include "Common/config.h"
#include <sstream>

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

INSTANCE_IMP(StorageManager)

StorageManager::~StorageManager() {
    _timer.reset();
}

StorageManager::StorageManager(const EventPoller::Ptr &poller) {
    _poller = poller ? std::move(poller) : EventPollerPool::Instance().getPoller();

    cleanupTemporaryFiles();
}

static void cleanupFolder(const string folder) {
    File::scanDir(folder, [](const string &path, bool isDir) {
        File::delete_file(path);
        return true;
    });
}

std::string formatDuration(int64_t milliseconds) {
    int64_t ms = milliseconds % 1000;
    int64_t total_seconds = milliseconds / 1000;
    int64_t seconds = total_seconds % 60;
    int64_t total_minutes = total_seconds / 60;
    int64_t minutes = total_minutes % 60;
    int64_t hours = total_minutes / 60;

    std::ostringstream oss;
    if (hours > 0) oss << hours << "h ";
    if (minutes > 0 || hours > 0) oss << minutes << "m ";
    if (seconds > 0 || minutes > 0 || hours > 0) oss << seconds << "s ";
    oss << ms << "ms";

    return oss.str();
}

void StorageManager::cleanupTemporaryFiles() {
    _ticker.resetTime();

    GET_CONFIG(string, hls_save_path, Protocol::kHlsSavePath)
    cleanupFolder(hls_save_path);
    DebugL << "Cleanup hls save path";

    GET_CONFIG(string, snap_save_path, "api.snapRoot");
    cleanupFolder(snap_save_path);
    DebugL << "Cleanup snap save path";

    GET_CONFIG(string, extract_save_path, "api.extractRoot")
    cleanupFolder(extract_save_path);
    DebugL << "Cleanup extract save path";

    InfoL << "Remove temporary files. Finished. " << formatDuration(_ticker.elapsedTime()) << " elapsed" ;
}

} // namespace managerkit
