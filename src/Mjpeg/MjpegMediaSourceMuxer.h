#ifndef MJPEG_MJPEGMEDIASOURCEMUXER_H
#define MJPEG_MJPEGMEDIASOURCEMUXER_H

#include <memory>
#include "Common/MediaSource.h"
#include "MjpegMediaSource.h"

namespace mediakit {

/**
 * Generic lifecycle manager and demand controller for any MjpegMediaSource subclass.
 *
 * Mirrors the FMP4MediaSourceMuxer pattern:
 *   - Owns and starts the inner MjpegMediaSource<FrameType>.
 *   - Intercepts onReaderChanged to implement demand gating:
 *       demand=false → isEnabled() always true (always encode/push).
 *       demand=true  → isEnabled() is true only while at least one viewer is connected.
 *   - Subclasses add domain-specific frame write and metadata methods.
 *
 * Template parameter:
 *   SourceType — a concrete subclass of MjpegMediaSource<FrameType>.
 *                Must expose readerCount(), clearCache(), startStream(),
 *                and setListener(weak_ptr<MediaSourceEvent>).
 *
 * Lifecycle:
 *   1.  auto muxer = std::make_shared<DerivedMuxer>(tuple, ...);
 *   2.  muxer->setListener(shared_from_this());   // after make_shared
 *   3.  Producer calls muxer->isEnabled() before encoding, then muxer->onWrite(pkt)
 *   4.  HTTP clients discover the source via MediaSource::findAsync(schema, ...)
 */
template<typename SourceType>
class MjpegMediaSourceMuxer
    : public MediaSourceEventInterceptor
    , public std::enable_shared_from_this<MjpegMediaSourceMuxer<SourceType>> {
public:
    using Ptr = std::shared_ptr<MjpegMediaSourceMuxer<SourceType>>;

    /**
     * @param src     Already-constructed source (make_shared<SourceType>(...)).
     *                startStream() is called here so the source registers itself.
     * @param demand  When true, encoding is gated by viewer presence.
     */
    MjpegMediaSourceMuxer(std::shared_ptr<SourceType> src, bool demand)
        : _media_src(std::move(src)), _demand(demand), _enabled(!demand) {
        _media_src->startStream();
    }

    ~MjpegMediaSourceMuxer() override = default;

    /**
     * Wire the outer event listener and connect the inner source back to this muxer.
     * Must be called after make_shared (shared_from_this is required).
     */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
        setDelegate(listener);
        _media_src->setListener(this->shared_from_this());
    }

    /** Number of live MJPEG readers currently connected. */
    int readerCount() const {
        return _media_src->readerCount();
    }

    /**
     * Called when reader count on the inner MediaSource changes.
     * Implements the demand gate.
     */
    void onReaderChanged(MediaSource &sender, int size) override {
        _enabled = _demand ? (size > 0) : true;
        if (!size && _demand) {
            _media_src->clearCache();
        }
        MediaSourceEventInterceptor::onReaderChanged(sender, size);
    }

    /**
     * Returns true when the producer should encode and push frames.
     * Always true when demand=false; gated by reader count when demand=true.
     */
    bool isEnabled() const { return _enabled; }

protected:
    std::shared_ptr<SourceType> _media_src;

private:
    bool _demand;
    bool _enabled;
};

} // namespace mediakit

#endif // MJPEG_MJPEGMEDIASOURCEMUXER_H
