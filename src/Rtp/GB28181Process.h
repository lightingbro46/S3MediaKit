#ifndef S3MEDIAKIT_GB28181ROCESS_H
#define S3MEDIAKIT_GB28181ROCESS_H

#if defined(ENABLE_RTPPROXY)

#include "Decoder.h"
#include "ProcessInterface.h"
#include "Http/HttpRequestSplitter.h"
#include "Rtsp/RtpCodec.h"
#include "Common/MediaSource.h"

namespace mediakit{

class RtpReceiverImp;
class GB28181Process : public ProcessInterface {
public:
    using Ptr = std::shared_ptr<GB28181Process>;

    GB28181Process(const MediaInfo &media_info, MediaSinkInterface *sink);

    /**
     * Input rtp
     * @param data rtp data pointer
     * @param data_len rtp data length
     * @return Whether the parsing is successful
     */
    bool inputRtp(bool, const char *data, size_t data_len) override;

    /**
     * Refresh and output all caches
     */
    void flush() override;

protected:
    void onRtpSorted(RtpPacket::Ptr rtp);

private:
    void onRtpDecode(const Frame::Ptr &frame);

private:
    MediaInfo _media_info;
    DecoderImp::Ptr _decoder;
    MediaSinkInterface *_interface;
    std::shared_ptr<FILE> _save_file_ps;
    std::unordered_map<uint8_t, RtpCodec::Ptr> _rtp_decoder;
    std::unordered_map<uint8_t, std::shared_ptr<RtpReceiverImp> > _rtp_receiver;
};

}//namespace mediakit
#endif//defined(ENABLE_RTPPROXY)
#endif //S3MEDIAKIT_GB28181ROCESS_H
