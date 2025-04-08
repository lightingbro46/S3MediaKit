#ifndef S3MEDIAKIT_RTSPMEDIASOURCEMUXER_H
#define S3MEDIAKIT_RTSPMEDIASOURCEMUXER_H

#include "RtspMuxer.h"
#include "Rtsp/RtspMediaSource.h"

namespace mediakit {

class RtspMediaSourceMuxer final : public RtspMuxer, public MediaSourceEventInterceptor,
                                   public std::enable_shared_from_this<RtspMediaSourceMuxer> {
public:
    using Ptr = std::shared_ptr<RtspMediaSourceMuxer>;

    RtspMediaSourceMuxer(const MediaTuple& tuple,
                         const ProtocolOption &option,
                         const TitleSdp::Ptr &title = nullptr) : RtspMuxer(title) {
        _option = option;
        _media_src = std::make_shared<RtspMediaSource>(tuple);
        getRtpRing()->setDelegate(_media_src);
    }

    ~RtspMediaSourceMuxer() override {
        try {
            RtspMuxer::flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener){
        setDelegate(listener);
        _media_src->setListener(shared_from_this());
    }

    int readerCount() const{
        return _media_src->readerCount();
    }

    void setTimeStamp(uint32_t stamp){
        _media_src->setTimeStamp(stamp);
    }

    void addTrackCompleted() override {
        RtspMuxer::addTrackCompleted();
        _media_src->setSdp(getSdp());
    }

    void onReaderChanged(MediaSource &sender, int size) override {
        _enabled = _option.rtsp_demand ? size : true;
        if (!size && _option.rtsp_demand) {
            _clear_cache = true;
        }
        MediaSourceEventInterceptor::onReaderChanged(sender, size);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (_clear_cache && _option.rtsp_demand) {
            _clear_cache = false;
            _media_src->clearCache();
        }
        if (_enabled || !_option.rtsp_demand) {
            return RtspMuxer::inputFrame(frame);
        }
        return false;
    }

    bool isEnabled() {
        // The inputFrame function is still allowed to be triggered when the cache has not been cleared, so that the cache can be cleared in time.
        return _option.rtsp_demand ? (_clear_cache ? true : _enabled) : true;
    }

private:
    bool _enabled = true;
    bool _clear_cache = false;
    ProtocolOption _option;
    RtspMediaSource::Ptr _media_src;
};


}//namespace mediakit
#endif //S3MEDIAKIT_RTSPMEDIASOURCEMUXER_H
