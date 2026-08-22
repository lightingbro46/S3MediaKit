#ifndef S3MEDIAKIT_HLSMEDIASOURCE_H
#define S3MEDIAKIT_HLSMEDIASOURCE_H

#include "Common/MediaSource.h"
#include "Util/TimeTicker.h"
#include "Util/RingBuffer.h"
#include "Network/Session.h"
#include <atomic>

namespace mediakit {

class HlsMediaSource : public MediaSource {
public:
    friend class HlsCookieData;

    using RingType = toolkit::RingBuffer<std::string>;
    using Ptr = std::shared_ptr<HlsMediaSource>;

    HlsMediaSource(const std::string &schema, const MediaTuple &tuple) : MediaSource(schema, tuple) {}

    /**
     * 	Get the circular buffer of the media source
     */
    const RingType::Ptr &getRing() const { return _ring; }

    /**
     * Get the number of players
     */
    int readerCount() override { return _ring ? _ring->readerCount() : 0; }

    /**
     * Set or clear the m3u8 index file content
     */
    void setIndexFile(std::string index_file);

    /**
     * Asynchronously get the m3u8 file
     */
    void getIndexFile(std::function<void(const std::string &str)> cb);

    /**
     * Synchronously get the m3u8 file
     */
    std::string getIndexFile() const {
        std::lock_guard<std::mutex> lck(_mtx_index);
        return _index_file;
    }

    void onSegmentSize(size_t bytes) { _speed[TrackVideo] += bytes; }

    void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                       const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) override {
        _ring->getInfoList(cb, on_change);
    }

private:
    RingType::Ptr _ring;
    std::string _index_file;
    mutable std::mutex _mtx_index;
    toolkit::List<std::function<void(const std::string &)>> _list_cb;
};

class HlsCookieData {
public:
    using Ptr = std::shared_ptr<HlsCookieData>;

    HlsCookieData(const MediaInfo &info, const std::shared_ptr<toolkit::Session> &session, std::string session_id = "");
    ~HlsCookieData();

    void addByteUsage(size_t bytes);
    void setMediaSource(const HlsMediaSource::Ptr &src);
    HlsMediaSource::Ptr getMediaSource() const;
    const std::string &getSessionId() const { return _session_id; }

private:
    void addReaderCount();
    void attachToSource(const HlsMediaSource::Ptr &src);

private:
    struct AttachmentState;
    std::atomic<uint64_t> _bytes { 0 };
    MediaInfo _info;
    std::string _session_id;
    toolkit::Ticker _ticker;
    std::shared_ptr<toolkit::SockInfo> _sock_info;
    std::weak_ptr<toolkit::Session> _session;
    std::shared_ptr<AttachmentState> _attachment;
    mutable std::mutex _activity_mtx;
};

} // namespace mediakit
#endif // S3MEDIAKIT_HLSMEDIASOURCE_H
