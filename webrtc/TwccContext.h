#ifndef S3MEDIAKIT_TWCCCONTEXT_H
#define S3MEDIAKIT_TWCCCONTEXT_H

#include <stdint.h>
#include <map>
#include <functional>
#include <string>

namespace mediakit {

class TwccContext {
public:
    using onSendTwccCB = std::function<void(uint32_t ssrc, std::string fci)>;
    // Maximum RTP ext seq increment indicated by each twcc rtcp packet
    static constexpr size_t kMaxSeqSize = 20;
    // Maximum time interval for sending each twcc rtcp packet, in milliseconds
    static constexpr size_t kMaxTimeDelta = 256;

    void onRtp(uint32_t ssrc, uint16_t twcc_ext_seq, uint64_t stamp_ms);
    void setOnSendTwccCB(onSendTwccCB cb);

private:
    void onSendTwcc(uint32_t ssrc);
    bool needSendTwcc() const;
    int checkSeqStatus(uint16_t twcc_ext_seq) const;
    void clearStatus();

private:
    uint64_t _min_stamp = 0;
    uint64_t _max_stamp;
    std::map<uint32_t /*twcc_ext_seq*/, uint64_t/*recv time in ms*/> _rtp_recv_status;
    uint8_t _twcc_pkt_count = 0;
    onSendTwccCB _cb;
};

}// namespace mediakit
#endif //S3MEDIAKIT_TWCCCONTEXT_H
