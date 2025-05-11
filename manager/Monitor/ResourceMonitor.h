#ifndef MANAGER_RESOURCEMONITOR_H
#define MANAGER_RESOURCEMONITOR_H

#include <memory>
#include <string>
#include "MonitorBase.h"

namespace managerkit {

class ResourceMonitor : public xPlayerImp<MonitorBase, MonitorBase> {
public:
    using Ptr = std::shared_ptr<ResourceMonitor>;

    ResourceMonitor(const toolkit::EventPoller::Ptr &poller = nullptr);

    void play(const std::string &url) override;
    toolkit::EventPoller::Ptr getPoller();
    void setOnCreateSocket(toolkit::Socket::onCreateSocket cb);
private:
    toolkit::EventPoller::Ptr _poller;
    toolkit::Socket::onCreateSocket _on_create_socket;
};

} /* namespace managerkit */

#endif // MANAGER_RESOURCEMONITOR_H