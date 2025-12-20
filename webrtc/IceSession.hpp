#ifndef S3MEDIAKIT_WEBRTC_ICE_SESSION_H
#define S3MEDIAKIT_WEBRTC_ICE_SESSION_H

#include "Network/Session.h"
#include "IceTransport.hpp"
#include "Http/HttpRequestSplitter.h"

namespace mediakit {

class IceSession : public toolkit::Session, public RTC::IceTransport::Listener, public HttpRequestSplitter {
public:
    using Ptr = std::shared_ptr<IceSession>;
    using WeakPtr = std::weak_ptr<IceSession>;
    IceSession(const toolkit::Socket::Ptr &sock);
    ~IceSession() override;

    static toolkit::EventPoller::Ptr queryPoller(const toolkit::Buffer::Ptr &buffer);

    //// Session override////
    // void attachServer(const Server &server) override;
    void onRecv(const toolkit::Buffer::Ptr &) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;

    // ice related callbacks ///
    void onIceTransportRecvData(const toolkit::Buffer::Ptr& buffer, const RTC::IceTransport::Pair::Ptr& pair) override;
    void onIceTransportGatheringCandidate(const RTC::IceTransport::Pair::Ptr& pair, const RTC::CandidateInfo& candidate) override;
    void onIceTransportDisconnected() override;
    void onIceTransportCompleted() override;

    //// HttpRequestSplitter override ////
    ssize_t onRecvHeader(const char *data, size_t len) override;
    const char *onSearchPacketTail(const char *data, size_t len) override;

    void onRecv_l(const char *data, size_t len);
protected:
    bool _over_tcp = false;

    RTC::IceTransport::Pair::Ptr _session_pair = nullptr;
    RTC::IceServer::Ptr _ice_transport;
};

class IceSessionManager {
public:
    static IceSessionManager &Instance();
    IceSession::Ptr getItem(const std::string& key);
    void addItem(const std::string& key, const IceSession::Ptr &ptr);
    void removeItem(const std::string& key);

private:
    IceSessionManager() = default;

private:
    std::mutex _mtx;
    std::unordered_map<std::string, std::weak_ptr<IceSession>> _map;
};
}// namespace mediakit

#endif //S3MEDIAKIT_WEBRTC_ICE_SESSION_H
