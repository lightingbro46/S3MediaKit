#ifndef S3MEDIAKIT_WebSocketClient_H
#define S3MEDIAKIT_WebSocketClient_H

#include "Util/util.h"
#include "Util/base64.h"
#include "Util/SHA1.h"
#include "Network/TcpClient.h"
#include "HttpClientImp.h"
#include "WebSocketSplitter.h"

namespace mediakit {

template <typename ClientType, WebSocketHeader::Type DataType>
class HttpWsClient;

/**
 * Helper class for intercepting data sent by TcpClient before sending
 * @tparam ClientType TcpClient derived class
 * @tparam DataType This is useless, used for declaring friends
 */
template <typename ClientType, WebSocketHeader::Type DataType>
class ClientTypeImp : public ClientType {
public:
    friend class HttpWsClient<ClientType, DataType>;

    using onBeforeSendCB = std::function<ssize_t(const toolkit::Buffer::Ptr &buf)>;

    template <typename... ArgsType>
    ClientTypeImp(ArgsType &&...args) : ClientType(std::forward<ArgsType>(args)...) {}

    /**
     * Intercept before sending and package it into websocket protocol
     */
    ssize_t send(toolkit::Buffer::Ptr buf) override {
        if (_beforeSendCB) {
            return _beforeSendCB(buf);
        }
        return ClientType::send(std::move(buf));
    }

protected:
    /**
     * Set the data interception callback function
     * @param cb Interception callback function
     */
    void setOnBeforeSendCB(const onBeforeSendCB &cb) { _beforeSendCB = cb; }

private:
    onBeforeSendCB _beforeSendCB;
};

/**
 * This object completes the weksocket client handshake protocol and bridges to the TcpClient derived class events
 * @tparam ClientType TcpClient derived class
 * @tparam DataType websocket payload type, TEXT or BINARY type
 */
template <typename ClientType, WebSocketHeader::Type DataType = WebSocketHeader::TEXT>
class HttpWsClient : public HttpClientImp, public WebSocketSplitter {
public:
    using Ptr = std::shared_ptr<HttpWsClient>;

    HttpWsClient(const std::shared_ptr<ClientTypeImp<ClientType, DataType>> &delegate) : _weak_delegate(delegate) {
        _Sec_WebSocket_Key = encodeBase64(toolkit::makeRandStr(16, false));
        setPoller(delegate->getPoller());
    }

    /**
     * Initiate ws handshake
     * @param ws_url ws connection url
     * @param fTimeOutSec Timeout time
     */
    void startWsClient(const std::string &ws_url, float fTimeOutSec) {
        std::string http_url = ws_url;
        toolkit::replace(http_url, "ws://", "http://");
        toolkit::replace(http_url, "wss://", "https://");
        setMethod("GET");
        addHeader("Upgrade", "websocket");
        addHeader("Connection", "Upgrade");
        addHeader("Sec-WebSocket-Version", "13");
        addHeader("Sec-WebSocket-Key", _Sec_WebSocket_Key);
        _onRecv = nullptr;
        setHeaderTimeout(fTimeOutSec * 1000);
        sendRequest(http_url);
    }

    void closeWsClient() {
        if (!_onRecv) {
            // Not connected
            return;
        }
        WebSocketHeader header;
        header._fin = true;
        header._reserved = 0;
        header._opcode = CLOSE;
        // Client needs encryption
        header._mask_flag = true;
        WebSocketSplitter::encode(header, nullptr);
    }

protected:
    // HttpClientImp override

    /**
     * Receive http response header
     * @param status Status code, such as: 200 OK
     * @param headers http header
     */
    void onResponseHeader(const std::string &status, const HttpHeader &headers) override {
        if (status == "101") {
            auto Sec_WebSocket_Accept = encodeBase64(toolkit::SHA1::encode_bin(_Sec_WebSocket_Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
            if (Sec_WebSocket_Accept == const_cast<HttpHeader &>(headers)["Sec-WebSocket-Accept"]) {
                // success
                onWebSocketException(toolkit::SockException());
                // Prevent ws server from returning Content-Length
                const_cast<HttpHeader &>(headers).erase("Content-Length");
                return;
            }
            shutdown(toolkit::SockException(toolkit::Err_shutdown, StrPrinter << "Sec-WebSocket-Accept mismatch"));
            return;
        }

        shutdown(toolkit::SockException(toolkit::Err_shutdown, StrPrinter << "bad http status code:" << status));
    };

    /**
     * Receive http response complete,
     */
    void onResponseCompleted(const toolkit::SockException &ex) override {}

    /**
     * Receive websocket payload data
     */
    void onResponseBody(const char *buf, size_t size) override {
        if (_onRecv) {
            // After completing the websocket handshake, intercept the websocket data and parse it
            _onRecv(buf, size);
        }
    };

    // TcpClient override

    void onRecv(const toolkit::Buffer::Ptr &buf) override {
        HttpClientImp::onRecv(buf);
    }

    /**
     * Triggered periodically
     */
    void onManager() override {
        if (_onRecv) {
            // websocket connection succeeded
            if (auto strong_ref = _weak_delegate.lock()) {
                strong_ref->onManager();
            }
        } else {
            // websocket connecting...
            HttpClientImp::onManager();
        }

        if (!_onRecv) {
            // websocket not yet connected
            return;
        }

        if (_recv_ticker.elapsedTime() > 30 * 1000) {
            shutdown(toolkit::SockException(toolkit::Err_timeout, "websocket timeout"));
        } else if (_recv_ticker.elapsedTime() > 10 * 1000) {
            // No response received, send a ping packet every 10 seconds
            WebSocketHeader header;
            header._fin = true;
            header._reserved = 0;
            header._opcode = PING;
            header._mask_flag = true;
            WebSocketSplitter::encode(header, nullptr);
        }
    }

    /**
     * Callback after all data has been sent
     */
    void onFlush() override {
        if (_onRecv) {
            // websocket connection succeeded
            if (auto strong_ref = _weak_delegate.lock()) {
                strong_ref->onFlush();
            }
        } else {
            // websocket connecting...
            HttpClientImp::onFlush();
        }
    }

    /**
     * tcp connection result
     */
    void onConnect(const toolkit::SockException &ex) override {
        if (ex) {
            // tcp connection failed, return failure directly
            onWebSocketException(ex);
            return;
        }
        // Start websocket handshake
        HttpClientImp::onConnect(ex);
    }

    /**
     * tcp connection disconnected
     */
    void onError(const toolkit::SockException &ex) override {
        // Disconnection caused by tcp disconnection or shutdown
        onWebSocketException(ex);
    }

    // WebSocketSplitter override

    /**
     * Receive a webSocket data packet header, and then continue to trigger the onWebSocketDecodePayload callback
     * @param header Data packet header
     */
    void onWebSocketDecodeHeader(const WebSocketHeader &header) override { _payload_section.clear(); }

    /**
     * Receive webSocket data packet payload
     * @param header Data packet header
     * @param ptr Payload data pointer
     * @param len Payload data length
     * @param recved Received data length (including the current data length), equal to header._payload_len when the reception is complete
     */
    void onWebSocketDecodePayload(const WebSocketHeader &header, const uint8_t *ptr, size_t len, size_t recved) override {
        _payload_section.append((char *)ptr, len);
    }

    /**
     * Callback after receiving a complete webSocket data packet
     * @param header Data packet header
     */
    void onWebSocketDecodeComplete(const WebSocketHeader &header_in) override {
        WebSocketHeader &header = const_cast<WebSocketHeader &>(header_in);
        auto flag = header._mask_flag;
        // websocket client needs to encrypt data sent
        header._mask_flag = true;
        _recv_ticker.resetTime();
        switch (header._opcode) {
            case WebSocketHeader::CLOSE: {
                // Server actively closes
                WebSocketSplitter::encode(header, nullptr);
                shutdown(toolkit::SockException(toolkit::Err_eof, "websocket server close the connection"));
                break;
            }

            case WebSocketHeader::PING: {
                // Heartbeat packet
                header._opcode = WebSocketHeader::PONG;
                WebSocketSplitter::encode(header, std::make_shared<toolkit::BufferString>(std::move(_payload_section)));
                break;
            }

            case WebSocketHeader::CONTINUATION:
            case WebSocketHeader::TEXT:
            case WebSocketHeader::BINARY: {
                if (!header._fin) {
                    // There are subsequent fragment data, we cache the data first, and output it all at once after all fragments are collected
                    _payload_cache.append(std::move(_payload_section));
                    if (_payload_cache.size() < MAX_WS_PACKET) {
                        // There is also memory capacity to cache fragment data
                        break;
                    }
                    // Fragment cache is too large, need to clear
                }

                // Last packet
                if (_payload_cache.empty()) {
                    // This packet is the only fragment
                    if (auto strong_ref = _weak_delegate.lock()) {
                        strong_ref->onRecv(std::make_shared<WebSocketBuffer>(header._opcode, header._fin, std::move(_payload_section)));
                    }
                    break;
                }

                // This packet consists of multiple fragments
                _payload_cache.append(std::move(_payload_section));
                if (auto strong_ref = _weak_delegate.lock()) {
                    strong_ref->onRecv(std::make_shared<WebSocketBuffer>(header._opcode, header._fin, std::move(_payload_cache)));
                }
                _payload_cache.clear();
                break;
            }

            default: break;
        }
        _payload_section.clear();
        header._mask_flag = flag;
    }

    /**
     * websocket data encoding callback
     * @param ptr data pointer
     * @param len data pointer length
     */
    void onWebSocketEncodeData(toolkit::Buffer::Ptr buffer) override { HttpClientImp::send(std::move(buffer)); }

private:
    void onWebSocketException(const toolkit::SockException &ex) {
        if (!ex) {
            // websocket handshake successful
            // Here, the data sent by the TcpClient derived class is intercepted and packaged into the websocket protocol
            std::weak_ptr<HttpWsClient> weakSelf = std::static_pointer_cast<HttpWsClient>(shared_from_this());
            if (auto strong_ref = _weak_delegate.lock()) {
                strong_ref->setOnBeforeSendCB([weakSelf](const toolkit::Buffer::Ptr &buf) {
                    auto strong_self = weakSelf.lock();
                    if (strong_self) {
                        WebSocketHeader header;
                        header._fin = true;
                        header._reserved = 0;
                        header._opcode = DataType;
                        // Client needs encryption
                        header._mask_flag = true;
                        strong_self->WebSocketSplitter::encode(header, buf);
                    }
                    return buf->size();
                });
                // Set sock, otherwise shutdown and other interfaces are invalid
                strong_ref->setSock(HttpClientImp::getSock());
                // Trigger connection success event
                strong_ref->onConnect(ex);
            }

            // Intercept websocket data reception
            _onRecv = [this](const char *data, size_t len) {
                // Parse websocket data packet
                this->WebSocketSplitter::decode((uint8_t *)data, len);
            };
            return;
        }

        // websocket handshake failed or tcp connection failed or disconnected in the middle
        if (_onRecv) {
            // Disconnected in the middle after handshake success
            _onRecv = nullptr;
            if (auto strong_ref = _weak_delegate.lock()) {
                strong_ref->onError(ex);
            }
            return;
        }

        // websocket handshake failed or tcp connection failed
        if (auto strong_ref = _weak_delegate.lock()) {
            strong_ref->onConnect(ex);
        }
    }

private:
    std::string _Sec_WebSocket_Key;
    std::function<void(const char *data, size_t len)> _onRecv;
    std::weak_ptr<ClientTypeImp<ClientType, DataType>> _weak_delegate;
    std::string _payload_section;
    std::string _payload_cache;
    toolkit::Ticker _recv_ticker;
};

/**
 * Tcp client to WebSocket client template,
 * Through this template, developers can quickly implement WebSocket protocol packaging without modifying any code of the TcpClient derived class
 * @tparam ClientType TcpClient derived class
 * @tparam DataType websocket payload type, is it TEXT or BINARY type
 * @tparam useWSS Whether to use ws or wss connection
 */
template <typename ClientType, WebSocketHeader::Type DataType = WebSocketHeader::TEXT, bool useWSS = false>
class WebSocketClient : public ClientTypeImp<ClientType, DataType> {
public:
    using Ptr = std::shared_ptr<WebSocketClient>;

    template <typename... ArgsType>
    WebSocketClient(ArgsType &&...args) : ClientTypeImp<ClientType, DataType>(std::forward<ArgsType>(args)...) {}
    ~WebSocketClient() override { _wsClient->closeWsClient(); }

    /**
     * Overload the startConnect method,
     * The purpose is to replace the TcpClient's connection server behavior, so that it completes the WebSocket handshake first
     * @param host websocket server ip or domain name
     * @param iPort websocket server port
     * @param timeout_sec timeout time
     * @param local_port local listening port, which does not work here
     */
    void startConnect(const std::string &host, uint16_t port, float timeout_sec = 3, uint16_t local_port = 0) override {
        std::string ws_url;
        if (useWSS) {
            // Encrypted ws
            ws_url = StrPrinter << "wss://" + host << ":" << port << "/";
        } else {
            // Plaintext ws
            ws_url = StrPrinter << "ws://" + host << ":" << port << "/";
        }
        startWebSocket(ws_url, timeout_sec);
    }

    void startWebSocket(const std::string &ws_url, float fTimeOutSec = 3) {
        _wsClient = std::make_shared<HttpWsClient<ClientType, DataType>>(std::static_pointer_cast<WebSocketClient>(this->shared_from_this()));
        _wsClient->setOnCreateSocket([this](const toolkit::EventPoller::Ptr &) { return this->createSocket(); });
        _wsClient->startWsClient(ws_url, fTimeOutSec);
    }

    HttpClient &getHttpClient() { return *_wsClient; }

private:
    typename HttpWsClient<ClientType, DataType>::Ptr _wsClient;
};

} // namespace mediakit
#endif // S3MEDIAKIT_WebSocketClient_H
