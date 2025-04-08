#ifndef S3MEDIAKIT_RTMPMEDIASOURCEMUXER_H
#define S3MEDIAKIT_RTMPMEDIASOURCEMUXER_H

#include "RtmpMuxer.h"
#include "Rtmp/RtmpMediaSource.h"

namespace mediakit {

class RtmpMediaSourceMuxer final : public RtmpMuxer, public MediaSourceEventInterceptor,
                                   public std::enable_shared_from_this<RtmpMediaSourceMuxer> {
public:
    using Ptr = std::shared_ptr<RtmpMediaSourceMuxer>;

    RtmpMediaSourceMuxer(const MediaTuple& tuple,
                         const ProtocolOption &option,
                         const TitleMeta::Ptr &title = nullptr) : RtmpMuxer(title) {
        _option = option;
        _media_src = std::make_shared<RtmpMediaSource>(tuple);
        getRtmpRing()->setDelegate(_media_src);
    }

    ~RtmpMediaSourceMuxer() override {
        try {
            RtmpMuxer::flush();
        } catch (std::exception &ex) {
            WarnL << ex.what();
        }
    }

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener){
        setDelegate(listener);
        _media_src->setListener(shared_from_this());
    }

    void setTimeStamp(uint32_t stamp){
        _media_src->setTimeStamp(stamp);
    }

    int readerCount() const{
        return _media_src->readerCount();
    }

    void addTrackCompleted() override {
        RtmpMuxer::addTrackCompleted();
        makeConfigPacket();
        _media_src->setMetaData(getMetadata());
    }

    void onReaderChanged(MediaSource &sender, int size) override {
        _enabled = _option.rtmp_demand ? size : true;
        if (!size && _option.rtmp_demand) {
            _clear_cache = true;
        }
        MediaSourceEventInterceptor::onReaderChanged(sender, size);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (_clear_cache && _option.rtmp_demand) {
            _clear_cache = false;
            _media_src->clearCache();
        }
        if (_enabled || !_option.rtmp_demand) {
            return RtmpMuxer::inputFrame(frame);
        }
        return false;
    }

    bool isEnabled() {
        // The inputFrame function is still allowed to be triggered when the cache has not been cleared, so that the cache can be cleared in time.
        return _option.rtmp_demand ? (_clear_cache ? true : _enabled) : true;
    }

private:
    bool _enabled = true;
    bool _clear_cache = false;
    ProtocolOption _option;
    RtmpMediaSource::Ptr _media_src;
};


}//namespace mediakit
#endif //S3MEDIAKIT_RTMPMEDIASOURCEMUXER_H
