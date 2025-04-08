#ifndef SRC_HTTP_HTTPSESSION_H_
#define SRC_HTTP_HTTPSESSION_H_

#include <functional>
#include "Network/Session.h"
#include "Rtmp/FlvMuxer.h"
#include "HttpRequestSplitter.h"
#include "WebSocketSplitter.h"
#include "HttpCookieManager.h"
#include "HttpFileManager.h"
#include "TS/TSMediaSource.h"
#include "FMP4/FMP4MediaSource.h"

namespace mediakit {

class HttpSession: public toolkit::Session,
                   public FlvMuxer,
                   public HttpRequestSplitter,
                   public WebSocketSplitter {
public:
    using Ptr = std::shared_ptr<HttpSession>;
    using KeyValue = StrCaseMap;
    using HttpResponseInvoker = HttpResponseInvokerImp ;
    friend class AsyncSender;
    /**
     * @param errMsg If empty, it means authentication passed, otherwise it is an error message
     * @param accessPath The root directory to run or prohibit access
     * @param cookieLifeSecond Authentication cookie validity period
     **/
    using HttpAccessPathInvoker = std::function<void(const std::string &errMsg,const std::string &accessPath, int cookieLifeSecond)>;

    HttpSession(const toolkit::Socket::Ptr &pSock);

    void onRecv(const toolkit::Buffer::Ptr &) override;
    void onError(const toolkit::SockException &err) override;
    void onManager() override;
    void setTimeoutSec(size_t second);
    void setMaxReqSize(size_t max_req_size);

protected:
    //FlvMuxer override
    void onWrite(const toolkit::Buffer::Ptr &data, bool flush) override ;
    void onDetach() override;
    std::shared_ptr<FlvMuxer> getSharedPtr() override;

    //HttpRequestSplitter override
    ssize_t onRecvHeader(const char *data,size_t len) override;
    void onRecvContent(const char *data,size_t len) override;

    /**
     * Overload for handling indefinite length content
     * This function can be used to handle large file uploads, http-flv streaming
     * @param header http request header
     * @param data content fragment data
     * @param len content fragment data size
     * @param totalSize total content size, if 0, it is unlimited length content
     * @param recvedSize received data size
     */
    virtual void onRecvUnlimitedContent(const Parser &header,
                                        const char *data,
                                        size_t len,
                                        size_t totalSize,
                                        size_t recvedSize){
        shutdown(toolkit::SockException(toolkit::Err_shutdown,"http post content is too huge,default closed"));
    }

    /**
     * websocket client connection event
     * @param header http header
     * @return true means allow websocket connection, otherwise refuse
     */
    virtual bool onWebSocketConnect(const Parser &header){
        WarnP(this) << "http server do not support websocket default";
        return false;
    }

    //WebSocketSplitter override
    /**
     * Callback after sending data for websocket protocol packaging
     * @param buffer websocket protocol data
     */
    void onWebSocketEncodeData(toolkit::Buffer::Ptr buffer) override;

    /**
     * Callback after receiving a complete webSocket data packet
     * @param header data packet header
     */
    void onWebSocketDecodeComplete(const WebSocketHeader &header_in) override;

    // Overload to get client ip
    std::string get_peer_ip() override;

private:
    void onHttpRequest_GET();
    void onHttpRequest_POST();
    void onHttpRequest_HEAD();
    void onHttpRequest_OPTIONS();

    bool checkLiveStream(const std::string &schema, const std::string  &url_suffix, const std::function<void(const MediaSource::Ptr &src)> &cb);

    bool checkLiveStreamFlv(const std::function<void()> &cb = nullptr);
    bool checkLiveStreamTS(const std::function<void()> &cb = nullptr);
    bool checkLiveStreamFMP4(const std::function<void()> &fmp4_list = nullptr);

    bool checkWebSocket();
    bool emitHttpEvent(bool doInvoke);
    void urlDecode(Parser &parser);
    void sendNotFound(bool bClose);
    void sendResponse(int code, bool bClose, const char *pcContentType = nullptr,
                      const HttpSession::KeyValue &header = HttpSession::KeyValue(),
                      const HttpBody::Ptr &body = nullptr, bool no_content_length = false);

    // Set socket flag
    void setSocketFlags();

protected:
    MediaInfo _media_info;

private:
    bool _is_live_stream = false;
    bool _live_over_websocket = false;
    bool _is_websocket = false;
    // Timeout
    size_t _keep_alive_sec = 0;
    // Maximum http request byte size
    size_t _max_req_size = 0;
    // Total traffic consumed
    uint64_t _total_bytes_usage = 0;
    // Origin field in http request
    std::string _origin;
    Parser _parser;
    toolkit::Ticker _ticker;
    TSMediaSource::RingType::RingReader::Ptr _ts_reader;
    FMP4MediaSource::RingType::RingReader::Ptr _fmp4_reader;
    // Callback to handle content data
    std::function<bool (const char *data,size_t len) > _on_recv_body;
};

using HttpsSession = toolkit::SessionWithSSL<HttpSession>;

} /* namespace mediakit */

#endif /* SRC_HTTP_HTTPSESSION_H_ */
