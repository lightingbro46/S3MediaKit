#ifndef S3MEDIAKIT_FMP4MEDIASOURCEMUXER_H
#define S3MEDIAKIT_FMP4MEDIASOURCEMUXER_H

#include "FMP4MediaSource.h"
#include "Record/MP4Muxer.h"

namespace mediakit {

class FMP4MediaSourceMuxer final : public MP4MuxerMemory, public MediaSourceEventInterceptor,
                                   public std::enable_shared_from_this<FMP4MediaSourceMuxer> {
public:
    using Ptr = std::shared_ptr<FMP4MediaSourceMuxer>;

    FMP4MediaSourceMuxer(const MediaTuple& tuple, const ProtocolOption &option) {
        _option = option;
        _media_src = std::make_shared<FMP4MediaSource>(tuple);
    }

    ~FMP4MediaSourceMuxer() override {
        try {
            MP4MuxerMemory::flush();
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

    void onReaderChanged(MediaSource &sender, int size) override {
        _enabled = _option.fmp4_demand ? size : true;
        if (!size && _option.fmp4_demand) {
            _clear_cache = true;
        }
        MediaSourceEventInterceptor::onReaderChanged(sender, size);
    }

    bool inputFrame(const Frame::Ptr &frame) override {
        if (_clear_cache && _option.fmp4_demand) {
            _clear_cache = false;
            _media_src->clearCache();
        }
        if (_enabled || !_option.fmp4_demand) {
            return MP4MuxerMemory::inputFrame(frame);
        }
        return false;
    }

    bool isEnabled() {
        // The inputFrame function is still allowed to be triggered when the cache has not been cleared, so that the cache can be cleared in time.
        return _option.fmp4_demand ? (_clear_cache ? true : _enabled) : true;
    }

    void addTrackCompleted() override {
        MP4MuxerMemory::addTrackCompleted();
        _media_src->setInitSegment(getInitSegment());
    }

protected:
    void onSegmentData(std::string string, uint64_t stamp, bool key_frame) override {
        if (string.empty()) {
            return;
        }
        FMP4Packet::Ptr packet = std::make_shared<FMP4Packet>(std::move(string));
        packet->time_stamp = stamp;
        _media_src->onWrite(std::move(packet), key_frame);
    }

private:
    bool _enabled = true;
    bool _clear_cache = false;
    ProtocolOption _option;
    FMP4MediaSource::Ptr _media_src;
};

}//namespace mediakit

#endif //S3MEDIAKIT_FMP4MEDIASOURCEMUXER_H
