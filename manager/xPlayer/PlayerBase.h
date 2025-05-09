#ifndef MANAGER_PLAYERBASE_H
#define MANAGER_PLAYERBASE_H

#include <map>
#include <memory>
#include <string>
#include <functional>
#include "Network/Socket.h"
#include "Util/mini.h"
#include "xCommon/Resource.h"
// #include "xCommon/ResourceSink.h"
// #include "xExtension/Frame.h"
// #include "xExtension/Track.h"

namespace managerkit {

class xPlayerBase : public xTrackSource, public toolkit::mINI {
public:
    using Ptr = std::shared_ptr<xPlayerBase>;
    using Event = std::function<void(const toolkit::SockException &ex)>;

    static Ptr createPlayer(const toolkit::EventPoller::Ptr &poller, const std::string &strUrl);

    xPlayerBase();

    /**
     * Start playback
     */
}
} // namespace managerkit

#endif // MANAGER_PLAYERBASE_H
