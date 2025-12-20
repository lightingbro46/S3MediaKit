#ifndef SRC_PUSHER_PUSHERBASE_H_
#define SRC_PUSHER_PUSHERBASE_H_

#include <map>
#include <memory>
#include <string>
#include <functional>
#include "Network/Socket.h"
#include "Util/mini.h"
#include "Common/MediaSource.h"

namespace mediakit {

class PusherBase : public toolkit::mINI {
public:
    using Ptr = std::shared_ptr<PusherBase>;
    using Event = std::function<void(const toolkit::SockException &ex)>;

    static Ptr createPusher(const toolkit::EventPoller::Ptr &poller,
                            const MediaSource::Ptr &src,
                            const std::string &strUrl);

    PusherBase();
    virtual ~PusherBase() = default;

    /**
     * Start streaming
     * @param strUrl Video url, supports rtsp/rtmp
     */
    virtual void publish(const std::string &strUrl) {};

    /**
     * Stop streaming
     */
    virtual void teardown() {};

    /**
     * Camera streaming result callback
     */
    virtual void setOnPublished(const Event &cb) = 0;

    /**
     * Set disconnect callback
     */
    virtual void setOnShutdown(const Event &cb) = 0;

    virtual size_t getSendSpeed() { return 0; }
    virtual size_t getSendTotalBytes() { return 0; }
    
protected:
    virtual void onShutdown(const toolkit::SockException &ex) = 0;
    virtual void onPublishResult(const toolkit::SockException &ex) = 0;
};

template<typename Parent, typename Delegate>
class PusherImp : public Parent {
public:
    using Ptr = std::shared_ptr<PusherImp>;

    template<typename ...ArgsType>
    PusherImp(ArgsType &&...args) : Parent(std::forward<ArgsType>(args)...) {}

    /**
     * Start streaming
     * @param url Streaming url, supports rtsp/rtmp
     */
    void publish(const std::string &url) override {
        return _delegate ? _delegate->publish(url) : Parent::publish(url);
    }

    /**
     * Stop streaming
     */
    void teardown() override {
        return _delegate ? _delegate->teardown() : Parent::teardown();
    }

    std::shared_ptr<toolkit::SockInfo> getSockInfo() const {
        return std::dynamic_pointer_cast<toolkit::SockInfo>(_delegate);
    }

    /**
     * Camera streaming result callback
     */
    void setOnPublished(const PusherBase::Event &cb) override {
        if (_delegate) {
            _delegate->setOnPublished(cb);
        }
        _on_publish = cb;
    }

    /**
     * Set disconnect callback
     */
    void setOnShutdown(const PusherBase::Event &cb) override {
        if (_delegate) {
            _delegate->setOnShutdown(cb);
        }
        _on_shutdown = cb;
    }

    size_t getSendSpeed() override {
        return _delegate ?  _delegate->getSendSpeed() : Parent::getSendSpeed();
    }
    
   size_t getSendTotalBytes() override {
        return _delegate ? _delegate->getSendTotalBytes() : Parent::getSendTotalBytes();
    }
    
protected:
    void onShutdown(const toolkit::SockException &ex) override {
        if (_on_shutdown) {
            _on_shutdown(ex);
            _on_shutdown = nullptr;
        }
    }

    void onPublishResult(const toolkit::SockException &ex) override {
        if (_on_publish) {
            _on_publish(ex);
            _on_publish = nullptr;
        }
    }

protected:
    PusherBase::Event _on_shutdown;
    PusherBase::Event _on_publish;
    std::shared_ptr<Delegate> _delegate;
};

} /* namespace mediakit */
#endif /* SRC_PUSHER_PUSHERBASE_H_ */
