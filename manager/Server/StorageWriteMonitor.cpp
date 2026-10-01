#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Common/config.h"
#include "Common/BlockingExecutors.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "StorageWriteMonitor.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

const char *storageWriteStateString(StorageWriteState state) {
    switch (state) {
        case StorageWriteState::READ_ONLY: return "read_only";
        case StorageWriteState::WRITE_FAILED: return "write_failed";
        default: return "writable";
    }
}

Json::Value StorageWriteStatus::toJson() const {
    Json::Value val;
    val["path"] = path;
    val["mount"] = mount_point;
    val["device"] = device;
    val["is_main_storage"] = is_main_storage;
    val["state"] = checked ? storageWriteStateString(state) : "unknown";
    val["error"] = error ? strerror(error) : "";
    val["checked_at"] = static_cast<Json::Int64>(checked_at);
    return val;
}

int StorageWriteMonitor::probeWritable(const std::string &dir) {
    // The record directory may not exist yet; test the nearest existing parent instead of creating it.
    string target = dir;
    struct stat st{};
    while (::stat(target.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            return errno;
        }
        auto pos = target.find_last_of('/');
        if (pos == string::npos || pos == 0) {
            target = "/";
            break;
        }
        target.erase(pos);
    }
    const string file = target + (target.back() == '/' ? "" : "/") + ".s3mk_write_probe";
    int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return errno;
    }
    static const char kData[] = "s3mediakit write probe\n";
    int err = 0;
    if (::write(fd, kData, sizeof(kData) - 1) != static_cast<ssize_t>(sizeof(kData) - 1)) {
        err = errno ? errno : EIO;
    } else if (::fsync(fd) != 0) {
        err = errno;
    }
    if (::close(fd) != 0 && !err) {
        err = errno;
    }
    if (::unlink(file.c_str()) != 0 && !err) {
        err = errno;
    }
    return err;
}

StorageWriteMonitor::StorageWriteMonitor(EventPoller::Ptr poller,
                                         float interval_sec,
                                         const TargetProvider &targets,
                                         const Probe &probe,
                                         TaskExecutor::Ptr executor,
                                         const RepeatProvider &repeat_sec)
    : _poller(poller ? poller : EventPollerPool::Instance().getPoller()),
      _interval_sec(interval_sec > 0 ? interval_sec : 10.0f),
      _targets(targets),
      _probe(probe ? probe : &StorageWriteMonitor::probeWritable),
      _executor(executor),
      _repeat_sec(repeat_sec ? repeat_sec : []() { return 300; }) {
}

StorageWriteMonitor::~StorageWriteMonitor() {
    _timer.reset();
}

void StorageWriteMonitor::start() {
    if (_timer) {
        return;
    }
    weak_ptr<StorageWriteMonitor> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        _interval_sec,
        [weak_self]() {
            auto self = weak_self.lock();
            if (!self) {
                return false;
            }
            self->dispatchChecks();
            return true;
        },
        _poller);
}

void StorageWriteMonitor::dispatchChecks() {
    auto targets = _targets ? _targets() : std::vector<WriteCheckTarget>();
    auto executor = _executor ? _executor : BlockingExecutors::api();
    weak_ptr<StorageWriteMonitor> weak_self = shared_from_this();
    {
        // Forget paths that are no longer watched (pool unregistered); keep in-flight ones until they finish.
        lock_guard<mutex> lck(_mtx);
        for (auto it = _states.begin(); it != _states.end();) {
            bool watched = false;
            for (const auto &t : targets) {
                watched = watched || t.path == it->first;
            }
            it = (!watched && !it->second.in_flight) ? _states.erase(it) : std::next(it);
        }
    }
    for (const auto &target : targets) {
        {
            lock_guard<mutex> lck(_mtx);
            auto &st = _states[target.path];
            if (st.in_flight) {
                continue;
            }
            st.in_flight = true;
        }
        executor->async([weak_self, target]() {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->checkTarget(target);
        });
    }
}

void StorageWriteMonitor::checkTarget(const WriteCheckTarget &target) {
    // A ro mount fails every write with EROFS; no need to touch it.
    int err = target.read_only ? EROFS : _probe(target.path);
    StorageWriteState state = StorageWriteState::WRITABLE;
    if (err == EROFS) {
        state = StorageWriteState::READ_ONLY;
    } else if (err != 0) {
        state = StorageWriteState::WRITE_FAILED;
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const int repeat_sec = _repeat_sec();

    bool emit = false;
    StorageWriteStatus status;
    {
        lock_guard<mutex> lck(_mtx);
        auto &st = _states[target.path];
        st.in_flight = false;
        const bool first = !st.status.checked;
        const StorageWriteState previous = st.status.state;
        st.status.path = target.path;
        st.status.mount_point = target.mount_point;
        st.status.device = target.device;
        st.status.is_main_storage = target.is_main_storage;
        st.status.checked = true;
        st.status.state = state;
        st.status.error = err;
        st.status.checked_at = now;
        if (state != previous) {
            // A path that starts out writable is not news; one that starts out broken is.
            emit = !(first && state == StorageWriteState::WRITABLE);
        } else if (state != StorageWriteState::WRITABLE && repeat_sec > 0 && now - st.last_emit >= repeat_sec) {
            emit = true;
        }
        if (emit) {
            st.last_emit = now;
        }
        status = st.status;
    }
    if (!emit) {
        return;
    }
    if (state == StorageWriteState::WRITABLE) {
        InfoL << "Storage is writable again: " << target.path;
    } else {
        WarnL << "Storage cannot be written (" << storageWriteStateString(state) << "): " << target.path
              << " mount=" << target.mount_point << " error=" << strerror(err);
    }
    const int state_value = static_cast<int>(state);
    if (!NOTICE_EMIT(BroadcastDiskWriteAlertArgs, mediakit::Broadcast::kBroadcastDiskWriteAlert, status.path,
                     status.mount_point, status.device, state_value, status.error, status.is_main_storage)) {
        TraceL << "No one listen disk write alert event";
    }
}

std::vector<StorageWriteStatus> StorageWriteMonitor::getStatus() {
    std::vector<StorageWriteStatus> out;
    lock_guard<mutex> lck(_mtx);
    for (const auto &kv : _states) {
        if (kv.second.status.checked) {
            out.push_back(kv.second.status);
        }
    }
    return out;
}

} // namespace managerkit
