// #ifndef MANAGER_MONITORBASE_H
// #define MANAGER_MONITORBASE_H

// #include <map>
// #include <memory>
// #include <string>
// #include <functional>
// #include "Network/Socket.h"
// #include "Util/mini.h"
// #include "Common/Resource.h"
// // #include "Common/ResourceSink.h"
// // #include "Extension/Frame.h"
// // #include "Extension/Track.h"

// namespace managerkit {

// class MonitorBase : public xTrackSource, public toolkit::mINI {
// public:
//     using Ptr = std::shared_ptr<MonitorBase>;
//     using Event = std::function<void(const toolkit::SockException &ex)>;

//     static Ptr createPlayer(const toolkit::EventPoller::Ptr &poller, const std::string &strUrl);

//     MonitorBase();

//     virtual void play(const std::string &url) {};

//     virtual void pause(bool flag) {};

//     virtual void teardown() {};

//     std::vector<xTrack::Ptr> getTracks(bool ready = true) const override { return std::vector<xTrack::Ptr>(); };

//     virtual void setResource(const Resource::Ptr &src) = 0;

//     virtual void setOnShutdown(const Event &cb) = 0;

//     virtual void setOnPlayResult(const Event &cb) = 0;

//     virtual void setOnResume(const std::function<void()> &cb) = 0;

// protected:
//     virtual void onResume() = 0;
//     virtual void onShutdown(const toolkit::SockException &ex) = 0;
//     virtual void onPlayResult(const toolkit::SockException &ex) = 0;
// };

// template<typename Parent, typename Delegate>
// class xPlayerImp : public Parent {
// public:
//     using Ptr = std::shared_ptr<xPlayerImp>;

//     template<typename ...ArgsType>
//     xPlayerImp(ArgsType &&...args) : Parent(std::forward<ArgsType>(args)...) {}

//     void play(const std::string &url) override {
//         return _delegate ? _delegate->play(url) : Parent::play(url);
//     }

//     void pause(bool flag) override {
//         return _delegate ? _delegate->pause(flag) : Parent::pause(flag);
//     }

//     void teardown() override {
//         return _delegate ? _delegate->teardown() : Parent::teardown();
//     }

//     std::vector<xTrack::Ptr> getTracks(bool ready = true) const override {
//         return _delegate ? _delegate->getTracks(ready) : Parent::getTracks(ready);
//     }

//     std::shared_ptr<toolkit::SockInfo> getSockInfo() const {
//         return std::dynamic_pointer_cast<toolkit::SockInfo>(_delegate);
//     }

//     void setResource(const Resource::Ptr &src) override {
//         if (_delegate) {
//             _delegate->setResource(src);
//         }
//         _resource_src = src;
//     }

//     void setOnShutdown(const std::function<void(const toolkit::SockException &)> &cb) override {
//         if (_delegate) {
//             _delegate->setOnShutdown(cb);
//         }
//         _on_shutdown = cb;
//     }

//     void setOnPlayResult(const std::function<void(const toolkit::SockException &ex)> &cb) override {
//         if (_delegate) {
//             _delegate->setOnPlayResult(cb);
//         }
//         _on_play_result = cb;
//     }

//     void setOnResume(const std::function<void()> &cb) override {
//         if (_delegate) {
//             _delegate->setOnResume(cb);
//         }
//         _on_resume = cb;
//     }

// protected:
//     void onShutdown(const toolkit::SockException &ex) override {
//         if (_on_shutdown) {
//             _on_shutdown(ex);
//             _on_shutdown = nullptr;
//         }
//     }

//     void onPlayResult(const toolkit::SockException &ex) override {
//         if (_on_play_result) {
//             _on_play_result(ex);
//             _on_play_result = nullptr;
//         }
//     }

//     void onResume() override {
//         if (_on_resume) {
//             _on_resume();
//         }
//     }
    
// protected:
//     std::function<void()> _on_resume;
//     PlayerBase::Event _on_shutdown;
//     PlayerBase::Event _on_play_result;
//     MediaSource::Ptr _media_src;
//     std::shared_ptr<Delegate> _delegate;
// };

// } // namespace managerkit

// #endif // MANAGER_MONITORBASE_H
