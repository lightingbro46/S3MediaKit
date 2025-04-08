#ifndef S3MEDIAKIT_RTPCACHE_H
#define S3MEDIAKIT_RTPCACHE_H

#if defined(ENABLE_RTPPROXY)

#include "PSEncoder.h"
#include "RawEncoder.h"
#include "Common/PacketCache.h"

namespace mediakit{

class RtpCache : protected PacketCache<toolkit::Buffer> {
public:
    using onFlushed = std::function<void(std::shared_ptr<toolkit::List<toolkit::Buffer::Ptr> >)>;
    RtpCache(onFlushed cb);

protected:
    /**
     * Input rtp (for merging)
     * @param buffer rtp data
     */
    void input(uint64_t stamp, toolkit::Buffer::Ptr buffer,bool is_key = false);

protected:
    void onFlush(std::shared_ptr<toolkit::List<toolkit::Buffer::Ptr> > rtp_list, bool) override;

private:
    onFlushed _cb;
};

class RtpCachePS : public RtpCache, public PSEncoderImp {
public:
    RtpCachePS(onFlushed cb, uint32_t ssrc, uint8_t payload_type = 96, bool ps_or_ts = true) :
        RtpCache(std::move(cb)), PSEncoderImp(ssrc, ps_or_ts ? payload_type : static_cast<int>(Rtsp::PT_MP2T), ps_or_ts) {};

    void flush() override;

protected:
    void onRTP(toolkit::Buffer::Ptr rtp, bool is_key = false) override;
};

class RtpCacheRaw : public RtpCache, public RawEncoderImp {
public:
    RtpCacheRaw(onFlushed cb, uint32_t ssrc, uint8_t payload_type = 96, bool send_audio = true) : RtpCache(std::move(cb)), RawEncoderImp(ssrc, payload_type, send_audio) {};
    void flush() override;

protected:
    void onRTP(toolkit::Buffer::Ptr rtp, bool is_key = false) override;
};

} //namespace mediakit

#endif//ENABLE_RTPPROXY
#endif //S3MEDIAKIT_RTPCACHE_H
