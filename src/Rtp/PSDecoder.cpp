#if defined(ENABLE_RTPPROXY)

#include "PSDecoder.h"
#include "mpeg-ps.h"

using namespace toolkit;

namespace mediakit{

PSDecoder::PSDecoder() {
    _ps_demuxer = ps_demuxer_create([](void* param,
                                       int stream,
                                       int codecid,
                                       int flags,
                                       int64_t pts,
                                       int64_t dts,
                                       const void* data,
                                       size_t bytes){
        PSDecoder *thiz = (PSDecoder *)param;
        if(thiz->_on_decode){
            thiz->_on_decode(stream, codecid, flags, pts, dts, data, bytes);
        }
        return 0;
    },this);

    ps_demuxer_notify_t notify = {
            [](void *param, int stream, int codecid, const void *extra, int bytes, int finish) {
                PSDecoder *thiz = (PSDecoder *) param;
                if (thiz->_on_stream) {
                    thiz->_on_stream(stream, codecid, extra, bytes, finish);
                }
            }
    };
    ps_demuxer_set_notify((struct ps_demuxer_t *) _ps_demuxer, &notify, this);
}

PSDecoder::~PSDecoder() {
    ps_demuxer_destroy((struct ps_demuxer_t*)_ps_demuxer);
}

ssize_t PSDecoder::input(const uint8_t *data, size_t bytes) {
    HttpRequestSplitter::input(reinterpret_cast<const char *>(data), bytes);
    return bytes;
}

const char *PSDecoder::onSearchPacketTail(const char *data, size_t len) {
    try {
        auto ret = ps_demuxer_input(static_cast<struct ps_demuxer_t *>(_ps_demuxer), reinterpret_cast<const uint8_t *>(data), len);
        if (ret >= 0 && ret <= (ssize_t)len) {
            // Parse successful, all or part
            return data + ret;
        }

        // Parse failed, discard all data
        return data + len;
    } catch (toolkit::AssertFailedException &ex) {
        InfoL << "Analyze ps exception: bytes=" << len
              << ", exception=" << ex.what()
              << ", hex=" << hexdump(data, MIN(len, 32));
        // Trigger assertion, parse failed, discard all data
        return data + len;
    }
}

}//namespace mediakit
#endif//#if defined(ENABLE_RTPPROXY)
