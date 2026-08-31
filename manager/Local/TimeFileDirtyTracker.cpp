#include "TimeFileDirtyTracker.h"

#include <cstdio>
#include <sstream>

#include "Common/StrUtil.h"
#include "Util/File.h"
#include "Util/logger.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

INSTANCE_IMP(TimeFileDirtyTracker)

string TimeFileDirtyTracker::dirtyFilePath(const string &device_path) {
    return device_path + "/.dirty";
}

void TimeFileDirtyTracker::markDirty(const string &device_path, const string &file_path) {
    if (device_path.empty() || file_path.empty()) {
        return;
    }
    lock_guard<mutex> lock(_mutex);
    auto &state = _devices[device_path];
    auto it = state.files.find(file_path);
    bool newly_added = it == state.files.end();
    state.files[file_path] = state.next_version++;
    // Only touch the disk when the set of dirty files actually grows. Re-marking an
    // already dirty file only bumps the in-memory version (used for race detection),
    // so there is nothing new to persist.
    if (newly_added) {
        flushDeviceLocked(device_path);
    }
}

vector<TimeFileDirtyTracker::DirtyEntry> TimeFileDirtyTracker::snapshot(const string &device_path) {
    vector<DirtyEntry> result;
    lock_guard<mutex> lock(_mutex);
    auto it = _devices.find(device_path);
    if (it == _devices.end()) {
        return result;
    }
    result.reserve(it->second.files.size());
    for (const auto &file : it->second.files) {
        result.push_back(DirtyEntry{file.first, file.second});
    }
    return result;
}

void TimeFileDirtyTracker::clearIfUnchanged(const string &device_path, const string &file_path, uint64_t version) {
    lock_guard<mutex> lock(_mutex);
    auto it = _devices.find(device_path);
    if (it == _devices.end()) {
        return;
    }
    auto &files = it->second.files;
    auto file_it = files.find(file_path);
    if (file_it == files.end()) {
        return;
    }
    if (file_it->second != version) {
        // A new block was written while the rebuild was running, keep the file dirty
        // so it gets rebuilt again on the next cycle.
        WarnL << "Keep dirty time file, changed during rebuild: " << file_path;
        return;
    }
    files.erase(file_it);
    flushDeviceLocked(device_path);
}

uint64_t TimeFileDirtyTracker::currentVersion(const string &device_path, const string &file_path) {
    lock_guard<mutex> lock(_mutex);
    auto it = _devices.find(device_path);
    if (it == _devices.end()) {
        return 0;
    }
    auto file_it = it->second.files.find(file_path);
    return file_it == it->second.files.end() ? 0 : file_it->second;
}

void TimeFileDirtyTracker::flushDeviceLocked(const string &device_path) {    
    auto it = _devices.find(device_path);
    string path = dirtyFilePath(device_path);
    if (it == _devices.end() || it->second.files.empty()) {
        // nothing dirty, remove the persisted file if any
        if (File::fileExist(path)) {
            File::delete_file(path);
        }
        return;
    }

    ostringstream oss;
    for (const auto &file : it->second.files) {
        oss << file.first << "\n";
    }

    string tmp_path = path + ".tmp";
    if (!File::saveFile(oss.str(), tmp_path)) {
        WarnL << "Failed to persist dirty time file set: " << tmp_path;
        return;
    }
    if (rename(tmp_path.data(), path.data()) != 0) {
        WarnL << "Failed to rename dirty file: " << tmp_path << " -> " << path;
        File::delete_file(tmp_path);
    }
}

void TimeFileDirtyTracker::loadFromDisk(const string &record_path) {
    if (record_path.empty() || !File::is_dir(record_path)) {
        return;
    }
    lock_guard<mutex> lock(_mutex);
    File::scanDir(record_path, [&](const string &device_path, bool isDir) {
        if (!isDir) {
            return true;
        }
        string path = dirtyFilePath(device_path);
        if (!File::fileExist(path)) {
            return true;
        }
        string content = File::loadFile(path);
        if (content.empty()) {
            return true;
        }
        auto &state = _devices[device_path];
        istringstream iss(content);
        string line;
        size_t count = 0;
        while (getline(iss, line)) {
            if (line.empty()) {
                continue;
            }
            // ignore files that no longer exist on disk
            if (!File::fileExist(line)) {
                continue;
            }
            state.files[line] = state.next_version++;
            ++count;
        }
        if (count > 0) {
            InfoL << "Loaded " << count << " dirty time file(s) for device path: " << device_path;
        }
        return true;
    });
}

} // namespace managerkit
