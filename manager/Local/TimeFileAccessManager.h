#ifndef S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_
#define S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Util/onceToken.h"

namespace managerkit {

class TimeFileAccessManager {
public:
    using ReadGuard = std::shared_ptr<toolkit::onceToken>;

    static TimeFileAccessManager &Instance();

    ReadGuard acquireRead(const std::string &file);
    void beginPendingSwap(const std::string &file);
    void finalizeSwapWhenReadableIdle(const std::string &file, const std::function<void()> &cb);

private:
    TimeFileAccessManager() = default;

    std::shared_ptr<struct TimeFileAccessState> getState(const std::string &file);
    void releaseRead(const std::shared_ptr<struct TimeFileAccessState> &state, const std::string &file);

private:
    std::mutex _mutex;
    std::unordered_map<std::string, std::shared_ptr<struct TimeFileAccessState>> _states;
};

} // namespace managerkit

#endif // S3MEDIAKIT_TIMEFILEACCESSMANAGER_H_
