#ifndef MEDIAKIT_BACKCHANNELMEDIASOURCE_H
#define MEDIAKIT_BACKCHANNELMEDIASOURCE_H

#include "Common/MediaSource.h"
#include "Util/RingBuffer.h"

#define BC_GOP_SIZE 32

namespace mediakit {

class BackchannelFrame : public toolkit::BufferOffset<toolkit::Buffer::Ptr> {
public:
    using Ptr = std::shared_ptr<BackchannelFrame>;
    template <typename... ARGS>
    BackchannelFrame(ARGS&&... args) : BufferOffset(std::forward<ARGS>(args)...) {}

    static Ptr fromPcm(const std::vector<int16_t>& samples) {
        const size_t byteSize = samples.size() * sizeof(int16_t);

        auto buffer = toolkit::BufferRaw::create();
        buffer->setCapacity(byteSize);
        if (byteSize > 0) {
            std::memcpy(buffer->data(), samples.data(), byteSize);
        }
        buffer->setSize(byteSize);
        return std::make_shared<BackchannelFrame>(std::move(buffer));
    }
};

class BackchannelMediaSource : public MediaSource, public toolkit::RingDelegate<BackchannelFrame::Ptr> {
public:
    using Ptr = std::shared_ptr<BackchannelMediaSource>;
    using RingType = toolkit::RingBuffer<BackchannelFrame::Ptr>;

    BackchannelMediaSource(const MediaTuple &tuple, int ring_size = BC_GOP_SIZE)
        : MediaSource(BACKCHANNEL_SCHEMA, tuple) {
        _ring = std::make_shared<RingType>(ring_size);
    }

    ~BackchannelMediaSource() override = default;

    int readerCount() override { return _ring ? _ring->readerCount() : 0; }
    std::vector<Track::Ptr> getTracks(bool /*ready*/ = true) const override { return {}; }

    void onWrite(BackchannelFrame::Ptr frame, bool /*is_key*/) override {
        _ring->write(std::move(frame), true);
    }

    RingType::Ptr getRing() const { return _ring; }

    virtual void stopProducing() {}

    void registerSource() { regist(); }

private:
    RingType::Ptr _ring;
};

} // namespace mediakit
#endif // MEDIAKIT_BACKCHANNELMEDIASOURCE_H