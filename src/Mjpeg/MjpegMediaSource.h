#ifndef MJPEG_MJPEGMEDIASOURCE_H
#define MJPEG_MJPEGMEDIASOURCE_H

#include <list>
#include <memory>
#include <vector>
#include "Common/MediaSource.h"
#include "Network/Buffer.h"
#include "Util/RingBuffer.h"

namespace mediakit {

/**
 * Zero-copy Buffer wrapper for a JPEG payload held in a shared_ptr<vector<uint8_t>>.
 * Avoids copying JPEG bytes when writing to a TCP socket.
 */
class MjpegBuffer : public toolkit::Buffer {
public:
    explicit MjpegBuffer(std::shared_ptr<std::vector<uint8_t>> data)
        : _data(std::move(data)) {}
    char   *data() const override { return reinterpret_cast<char *>(_data->data()); }
    size_t  size() const override { return _data->size(); }
private:
    std::shared_ptr<std::vector<uint8_t>> _data;
};

/**
 * Generic reusable MJPEG live-stream media source.
 *
 * Parameterised by FrameType — any struct that clients know how to render.
 * This base class owns the ring-buffer lifecycle and MediaSource registration;
 * subclasses bind a schema and add domain-specific metadata.
 *
 * Lifecycle (same as FMP4/TS media sources):
 *   1. auto src = std::make_shared<Derived>(tuple);
 *   2. src->startStream();   // must be called AFTER make_shared (needs shared_from_this)
 *   3. src->onWrite(pkt);    // push frames from any thread
 *   4. Automatic unregist when last shared_ptr is released.
 *
 * Concrete subclass example:
 *   class MyMjpegSource : public MjpegMediaSource<MyFrame> {
 *   public:
 *       MyMjpegSource(const MediaTuple &t) : MjpegMediaSource<MyFrame>("myschema", t) {}
 *   };
 */
template<typename FrameType>
class MjpegMediaSource : public MediaSource {
public:
    using Ptr      = std::shared_ptr<MjpegMediaSource<FrameType>>;
    using RingType = toolkit::RingBuffer<std::shared_ptr<FrameType>>;

    MjpegMediaSource(const std::string &schema, const MediaTuple &tuple, int ring_size = 10)
        : MediaSource(schema, tuple), _ring_size(ring_size) {}

    ~MjpegMediaSource() override = default;

    /**
     * Create the ring buffer and register this source in the global registry.
     * Must be called once *after* make_shared — shared_from_this() is required.
     */
    void startStream() {
        if (!_ring) {
            createRing();
        }
    }

    /** Ring buffer for attaching live readers. */
    const typename RingType::Ptr &getRing() const { return _ring; }

    /** Number of currently connected streaming clients. */
    int readerCount() override {
        return _ring ? _ring->readerCount() : 0;
    }

    /**
     * Push a new frame to all connected readers.
     * Creates the ring on first call if startStream() was not called.
     */
    virtual void onWrite(std::shared_ptr<FrameType> pkt) {
        if (!_ring) createRing();
        _ring->write(std::move(pkt), /*key=*/true);
    }

    void getPlayerList(
        const std::function<void(const std::list<toolkit::Any> &)> &cb,
        const std::function<toolkit::Any(toolkit::Any &&)>         &on_change) override
    {
        if (_ring) _ring->getInfoList(cb, on_change);
    }

    void clearCache() {
        if (_ring) _ring->clearCache();
    }

protected:
    void createRing() {
        std::weak_ptr<MjpegMediaSource<FrameType>> weak_self =
            std::static_pointer_cast<MjpegMediaSource<FrameType>>(shared_from_this());
        _ring = std::make_shared<RingType>(_ring_size, [weak_self](int size) {
            if (auto s = weak_self.lock()) s->onReaderChanged(size);
        });
        regist();
    }

    int                       _ring_size;
    typename RingType::Ptr    _ring;
};

} // namespace mediakit

#endif // MJPEG_MJPEGMEDIASOURCE_H
