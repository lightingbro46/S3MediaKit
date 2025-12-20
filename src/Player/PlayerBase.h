#ifndef SRC_PLAYER_PLAYERBASE_H_
#define SRC_PLAYER_PLAYERBASE_H_

#include <map>
#include <memory>
#include <string>
#include <functional>
#include "Network/Socket.h"
#include "Util/mini.h"
#include "Common/MediaSource.h"
#include "Common/MediaSink.h"
#include "Extension/Frame.h"
#include "Extension/Track.h"
#include "Common/config.h"
#include "Common/Parser.h"

namespace mediakit {

template <typename Type>
void addCustomHeader(Type *c) {
    auto &custom_header = (*c)[Client::kCustomHeader];
    if (!custom_header.empty()) {
        auto args = mediakit::Parser::parseArgs(custom_header);
        for (auto &pr : args) {
            c->addHeader(pr.first, pr.second);
        }
    }
}

class PlayerBase : public TrackSource, public toolkit::mINI {
public:
    using Ptr = std::shared_ptr<PlayerBase>;
    using Event = std::function<void(const toolkit::SockException &ex)>;

    static Ptr createPlayer(const toolkit::EventPoller::Ptr &poller, const std::string &strUrl);

    PlayerBase();

    /**
     * Start playback
     * @param url Video url, supports rtsp/rtmp
     */
    virtual void play(const std::string &url) {};

    /**
     * Pause or resume
     * @param flag true: pause, false: resume
     */
    virtual void pause(bool flag) {};

    /**
     * Get the total duration of the program, in seconds
     */
    virtual float getDuration() const { return 0; };

    /**
     * Playback at a multiple
     * @param speed 1.0 2.0 0.5
     */
    virtual void speed(float speed) {};

    /**
     * Interrupt playback
     */
    virtual void teardown() {};

    /**
     * Get playback progress, value 0.0 ~ 1.0
     */
    virtual float getProgress() const { return 0; };

    /**
     * Get playback progress pos, value relative to the start time increment, unit seconds
     */
    virtual uint32_t getProgressPos() const { return 0; };

    /**
     * Drag the progress bar
     * @param progress Progress, value 0.0 ~ 1.0
     */
    virtual void seekTo(float progress) {};

    /**
     * Drag the progress bar
     * @param pos Progress, value relative to the start time increment, unit seconds
     */
    virtual void seekTo(uint32_t pos) {};

    /**
     * Get packet loss rate, only supports rtsp
     * @param type Audio or video, TrackInvalid for total packet loss rate
     */
    virtual float getPacketLossRate(TrackType type) const { return -1; };

    /**
     * Get all tracks
     */
    std::vector<Track::Ptr> getTracks(bool ready = true) const override { return std::vector<Track::Ptr>(); };

    /**
     * Set a MediaSource, directly produce rtsp/rtmp proxy
     */
    virtual void setMediaSource(const MediaSource::Ptr &src) = 0;

    /**
     * Set exception interrupt callback
     */
    virtual void setOnShutdown(const Event &cb) = 0;

    /**
     * Set playback result callback
     */
    virtual void setOnPlayResult(const Event &cb) = 0;

    /**
     * Set playback resume callback
     */
    virtual void setOnResume(const std::function<void()> &cb) = 0;
   
    virtual size_t getRecvSpeed() { return 0; }
    virtual size_t getRecvTotalBytes() { return 0; }
    virtual std::shared_ptr<toolkit::SockInfo> getSockInfo() const { return nullptr; } 

protected:
    virtual void onResume() = 0;
    virtual void onShutdown(const toolkit::SockException &ex) = 0;
    virtual void onPlayResult(const toolkit::SockException &ex) = 0;
};

template<typename Parent, typename Delegate>
class PlayerImp : public Parent {
public:
    using Ptr = std::shared_ptr<PlayerImp>;

    template<typename ...ArgsType>
    PlayerImp(ArgsType &&...args) : Parent(std::forward<ArgsType>(args)...) {}

    void play(const std::string &url) override {
        return _delegate ? _delegate->play(url) : Parent::play(url);
    }

    void pause(bool flag) override {
        return _delegate ? _delegate->pause(flag) : Parent::pause(flag);
    }

    void speed(float speed) override {
        return _delegate ? _delegate->speed(speed) : Parent::speed(speed);
    }

    void teardown() override {
        return _delegate ? _delegate->teardown() : Parent::teardown();
    }

    float getPacketLossRate(TrackType type) const override {
        return _delegate ? _delegate->getPacketLossRate(type) : Parent::getPacketLossRate(type);
    }

    float getDuration() const override {
        return _delegate ? _delegate->getDuration() : Parent::getDuration();
    }

    float getProgress() const override {
        return _delegate ? _delegate->getProgress() : Parent::getProgress();
    }

    uint32_t getProgressPos() const override {
        return _delegate ? _delegate->getProgressPos() : Parent::getProgressPos();
    }

    void seekTo(float progress) override {
        return _delegate ? _delegate->seekTo(progress) : Parent::seekTo(progress);
    }

    void seekTo(uint32_t pos) override {
        return _delegate ? _delegate->seekTo(pos) : Parent::seekTo(pos);
    }

    std::vector<Track::Ptr> getTracks(bool ready = true) const override {
        return _delegate ? _delegate->getTracks(ready) : Parent::getTracks(ready);
    }

    std::shared_ptr<toolkit::SockInfo> getSockInfo() const override {
        auto ret = std::dynamic_pointer_cast<toolkit::SockInfo>(_delegate);
        if (!ret)
            ret = _delegate ? _delegate->getSockInfo() : Parent::getSockInfo();
        return ret;
    }

    void setMediaSource(const MediaSource::Ptr &src) override {
        if (_delegate) {
            _delegate->setMediaSource(src);
        }
        _media_src = src;
    }

    void setOnShutdown(const std::function<void(const toolkit::SockException &)> &cb) override {
        if (_delegate) {
            _delegate->setOnShutdown(cb);
        }
        _on_shutdown = cb;
    }

    void setOnPlayResult(const std::function<void(const toolkit::SockException &ex)> &cb) override {
        if (_delegate) {
            _delegate->setOnPlayResult(cb);
        }
        _on_play_result = cb;
    }

    void setOnResume(const std::function<void()> &cb) override {
        if (_delegate) {
            _delegate->setOnResume(cb);
        }
        _on_resume = cb;
    }

    size_t getRecvSpeed() override {
        return _delegate ? _delegate->getRecvSpeed() : Parent::getRecvSpeed();
    }

    size_t getRecvTotalBytes() override {
        return _delegate ? _delegate->getRecvTotalBytes() : Parent::getRecvTotalBytes();
    }

protected:
    void onShutdown(const toolkit::SockException &ex) override {
        if (_on_shutdown) {
            _on_shutdown(ex);
            _on_shutdown = nullptr;
        }
    }

    void onPlayResult(const toolkit::SockException &ex) override {
        if (_on_play_result) {
            _on_play_result(ex);
            _on_play_result = nullptr;
        }
    }

    void onResume() override {
        if (_on_resume) {
            _on_resume();
        }
    }

protected:
    std::function<void()> _on_resume;
    PlayerBase::Event _on_shutdown;
    PlayerBase::Event _on_play_result;
    MediaSource::Ptr _media_src;
    std::shared_ptr<Delegate> _delegate;
};

} /* namespace mediakit */

#endif /* SRC_PLAYER_PLAYERBASE_H_ */
