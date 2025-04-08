#ifndef S3MEDIAKIT_WEBSOCKETSESSION_H
#define S3MEDIAKIT_WEBSOCKETSESSION_H

#include "HttpSession.h"
#include "Network/TcpServer.h"

/**
 * Data Send Interceptor
 */
class SendInterceptor{
public:
    using onBeforeSendCB =std::function<ssize_t (const toolkit::Buffer::Ptr &buf)>;

    virtual ~SendInterceptor() = default;
    virtual void setOnBeforeSendCB(const onBeforeSendCB &cb) = 0;
};

/**
 * This class implements the interception of data sent by the Session derived class.
 * The purpose is to package the websocket protocol before sending business data.
 */
template <typename SessionType>
class SessionTypeImp : public SessionType, public SendInterceptor{
public:
    using Ptr = std::shared_ptr<SessionTypeImp>;

    SessionTypeImp(const mediakit::Parser &header, const mediakit::HttpSession &parent, const toolkit::Socket::Ptr &pSock) :
            SessionType(pSock) {}

    /**
     * Set the send data interception callback function
     * @param cb Interception callback function
     */
    void setOnBeforeSendCB(const onBeforeSendCB &cb) override {
        _beforeSendCB = cb;
    }

protected:
    /**
     * Overload the send function to intercept data
     * @param buf Data to be intercepted
     * @return Number of data bytes
     */
    ssize_t send(toolkit::Buffer::Ptr buf) override {
        if (_beforeSendCB) {
            return _beforeSendCB(buf);
        }
        return SessionType::send(std::move(buf));
    }

private:
    onBeforeSendCB _beforeSendCB;
};

template <typename SessionType>
class SessionCreator {
public:
    // The returned Session must be derived from SendInterceptor, and can return null
    toolkit::Session::Ptr operator()(const mediakit::Parser &header, const mediakit::HttpSession &parent, const toolkit::Socket::Ptr &pSock, mediakit::WebSocketHeader::Type &data_type){
        return std::make_shared<SessionTypeImp<SessionType> >(header,parent,pSock);
    }
};

/**
 * Through this template class, the WebSocket protocol can be transparently implemented.
 * Users only need to implement specific business protocols under the WebSock protocol, such as the Rtmp protocol based on the WebSocket protocol.
*/
template<typename Creator, typename HttpSessionType = mediakit::HttpSession, mediakit::WebSocketHeader::Type DataType = mediakit::WebSocketHeader::TEXT>
class WebSocketSessionBase : public HttpSessionType {
public:
    WebSocketSessionBase(const toolkit::Socket::Ptr &pSock) : HttpSessionType(pSock){}

    // Callback when receiving eof or other events that cause disconnection from TcpServer
    void onError(const toolkit::SockException &err) override{
        HttpSessionType::onError(err);
        if(_session){
            _session->onError(err);
        }
    }
    // Triggered every period of time, used for timeout management
    void onManager() override{
        if (_session) {
            _session->onManager();
        } else {
            HttpSessionType::onManager();
        }
        if (!_session) {
            // websocket is not yet connected
            return;
        }
        if (_recv_ticker.elapsedTime() > 30 * 1000) {
            HttpSessionType::shutdown(toolkit::SockException(toolkit::Err_timeout, "websocket timeout"));
        } else if (_recv_ticker.elapsedTime() > 10 * 1000) {
            // No reply received, send a ping packet every 10 seconds
            mediakit::WebSocketHeader header;
            header._fin = true;
            header._reserved = 0;
            header._opcode = mediakit::WebSocketHeader::PING;
            header._mask_flag = false;
            HttpSessionType::encode(header, nullptr);
        }
    }

    void attachServer(const toolkit::Server &server) override{
        HttpSessionType::attachServer(server);
        _weak_server = const_cast<toolkit::Server &>(server).shared_from_this();
    }

protected:
    /**
     * websocket client connection event
     * @param header http header
     * @return true means allowing websocket connection, otherwise refuse
     */
    bool onWebSocketConnect(const mediakit::Parser &header) override{
        // Create websocket session class
        auto data_type = DataType;
        _session = _creator(header, *this, HttpSessionType::getSock(), data_type);
        if (!_session) {
            // This url is not allowed to create websocket connection
            return false;
        }
        auto strongServer = _weak_server.lock();
        if (strongServer) {
            _session->attachServer(*strongServer);
        }

        // Intercept data here and package it with websocket protocol
        std::weak_ptr<WebSocketSessionBase> weakSelf = std::static_pointer_cast<WebSocketSessionBase>(HttpSessionType::shared_from_this());
        std::dynamic_pointer_cast<SendInterceptor>(_session)->setOnBeforeSendCB([weakSelf, data_type](const toolkit::Buffer::Ptr &buf) {
            auto strongSelf = weakSelf.lock();
            if (strongSelf) {
                mediakit::WebSocketHeader header;
                header._fin = true;
                header._reserved = 0;
                header._opcode = data_type;
                header._mask_flag = false;
                strongSelf->HttpSessionType::encode(header, buf);
            }
            return buf->size();
        });

        // Allow websocket client
        return true;
    }

    /**
     * Start receiving a webSocket data packet
     */
    void onWebSocketDecodeHeader(const mediakit::WebSocketHeader &packet) override{
        // New package, the residual data of the original package is cleared
        _payload_section.clear();
    }

    /**
     * Receive websocket data packet payload
     */
    void onWebSocketDecodePayload(const mediakit::WebSocketHeader &packet,const uint8_t *ptr,size_t len,size_t recved) override {
        _payload_section.append((char *)ptr,len);
    }

    /**
     * Callback after receiving a complete webSocket data packet
     * @param header Data packet header
     */
    void onWebSocketDecodeComplete(const mediakit::WebSocketHeader &header_in) override {
        auto header = const_cast<mediakit::WebSocketHeader&>(header_in);
        auto  flag = header._mask_flag;
        header._mask_flag = false;
        _recv_ticker.resetTime();
        switch (header._opcode){
            case mediakit::WebSocketHeader::CLOSE:{
                HttpSessionType::encode(header,nullptr);
                HttpSessionType::shutdown(toolkit::SockException(toolkit::Err_shutdown, "recv close request from client"));
                break;
            }
            
            case mediakit::WebSocketHeader::PING:{
                header._opcode = mediakit::WebSocketHeader::PONG;
                HttpSessionType::encode(header,std::make_shared<toolkit::BufferString>(_payload_section));
                break;
            }
            
            case mediakit::WebSocketHeader::CONTINUATION:
            case mediakit::WebSocketHeader::TEXT:
            case mediakit::WebSocketHeader::BINARY:{
                if (!header._fin) {
                    // There is subsequent fragment data, we cache the data first, and output it all at once after all fragments are collected
                    _payload_cache.append(std::move(_payload_section));
                    if (_payload_cache.size() < MAX_WS_PACKET) {
                        // There is memory capacity to cache fragment data
                        break;
                    }
                    // Fragment cache is too large, need to be cleared
                }

                // Last package
                if (_payload_cache.empty()) {
                    // This package is the only fragment
                    _session->onRecv(std::make_shared<mediakit::WebSocketBuffer>(header._opcode, header._fin, std::move(_payload_section)));
                    break;
                }

                // This package consists of multiple fragments
                _payload_cache.append(std::move(_payload_section));
                _session->onRecv(std::make_shared<mediakit::WebSocketBuffer>(header._opcode, header._fin, std::move(_payload_cache)));
                _payload_cache.clear();
                break;
            }
            
            default: break;
        }
        _payload_section.clear();
        header._mask_flag = flag;
    }

    /**
     * Callback after sending data and packaging it with websocket protocol
    */
    void onWebSocketEncodeData(toolkit::Buffer::Ptr buffer) override{
        HttpSessionType::send(std::move(buffer));
    }

private:
    std::string _payload_cache;
    std::string _payload_section;
    std::weak_ptr<toolkit::Server> _weak_server;
    toolkit::Session::Ptr _session;
    Creator _creator;
    toolkit::Ticker _recv_ticker;
};


template<typename SessionType,typename HttpSessionType = mediakit::HttpSession, mediakit::WebSocketHeader::Type DataType = mediakit::WebSocketHeader::TEXT>
class WebSocketSession : public WebSocketSessionBase<SessionCreator<SessionType>,HttpSessionType,DataType>{
public:
    WebSocketSession(const toolkit::Socket::Ptr &pSock) : WebSocketSessionBase<SessionCreator<SessionType>,HttpSessionType,DataType>(pSock){}
};

#endif //S3MEDIAKIT_WEBSOCKETSESSION_H
