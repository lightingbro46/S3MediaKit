#ifndef SRC_PUSHER_MEDIAPUSHER_H_
#define SRC_PUSHER_MEDIAPUSHER_H_

#include <memory>
#include <string>
#include "PusherBase.h"

namespace mediakit {

class MediaPusher : public PusherImp<PusherBase,PusherBase> {
public:
    using Ptr = std::shared_ptr<MediaPusher>;

    MediaPusher(const std::string &schema,
                const std::string &vhost,
                const std::string &app,
                const std::string &stream,
                const toolkit::EventPoller::Ptr &poller = nullptr);

    MediaPusher(const MediaSource::Ptr &src,
                const toolkit::EventPoller::Ptr &poller = nullptr);

    void publish(const std::string &url) override;
    toolkit::EventPoller::Ptr getPoller();
    void setOnCreateSocket(toolkit::Socket::onCreateSocket cb);
    std::shared_ptr<MediaSource> getSrc() { return _src.lock(); }
    const std::string& getUrl() const { return _url; }

private:
    std::weak_ptr<MediaSource> _src;
    toolkit::EventPoller::Ptr _poller;
    toolkit::Socket::onCreateSocket _on_create_socket;
    std::string _url;
};

} /* namespace mediakit */

#endif /* SRC_PUSHER_MEDIAPUSHER_H_ */
