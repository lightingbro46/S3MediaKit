#ifndef LOCAL_STORAGEMANAGER_H
#define LOCAL_STORAGEMANAGER_H

#include <memory>
#include <string>
#include "Poller/EventPoller.h"
#include "Poller/Timer.h"
#include "Util/TimeTicker.h"

namespace managerkit {
 
class StorageManager {
public:
    using Ptr = std::shared_ptr<StorageManager>;

    static StorageManager &Instance();
    ~StorageManager();

private:
    StorageManager(const toolkit::EventPoller::Ptr &poller = nullptr);

    void cleanupTemporaryFiles();

private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Timer::Ptr _timer;
    toolkit::Ticker _ticker;
};

} // namespace managerkit

#endif // LOCAL_STORAGEMANAGER_H