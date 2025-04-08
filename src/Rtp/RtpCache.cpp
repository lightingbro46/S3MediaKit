#include "RtpCache.h"

#if defined(ENABLE_RTPPROXY)

using namespace toolkit;

namespace mediakit{

RtpCache::RtpCache(onFlushed cb) {
    _cb = std::move(cb);
}

void RtpCache::onFlush(std::shared_ptr<List<Buffer::Ptr>> rtp_list, bool) {
    _cb(std::move(rtp_list));
}

void RtpCache::input(uint64_t stamp, Buffer::Ptr buffer, bool is_key) {
    inputPacket(stamp, true, std::move(buffer), is_key);
}

void RtpCachePS::flush() {
    PSEncoderImp::flush();
    RtpCache::flush();
}

void RtpCachePS::onRTP(Buffer::Ptr buffer, bool is_key) {
    auto rtp = std::static_pointer_cast<RtpPacket>(buffer);
    auto stamp = rtp->getStampMS();
    input(stamp, std::move(buffer), is_key);
}

void RtpCacheRaw::flush() {
    RawEncoderImp::flush();
    RtpCache::flush();
}

void RtpCacheRaw::onRTP(Buffer::Ptr buffer, bool is_key) {
    auto rtp = std::static_pointer_cast<RtpPacket>(buffer);
    auto stamp = rtp->getStampMS();
    input(stamp, std::move(buffer), is_key);
}

}//namespace mediakit

#endif//#if defined(ENABLE_RTPPROXY)