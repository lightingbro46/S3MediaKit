#include "HlsMediaSource.h"
#include "Common/config.h"

using namespace toolkit;

namespace mediakit {

class SockInfoImp : public SockInfo {
public:
    using Ptr = std::shared_ptr<SockInfoImp>;

    std::string get_local_ip() override { return _local_ip; }

    uint16_t get_local_port() override { return _local_port; }

    std::string get_peer_ip() override { return _peer_ip; }

    uint16_t get_peer_port() override { return _peer_port; }

    std::string getIdentifier() const override { return _identifier; }

    std::string _local_ip;
    std::string _peer_ip;
    std::string _identifier;
    uint16_t _local_port;
    uint16_t _peer_port;
};

struct HlsCookieData::AttachmentState {
    std::mutex mtx;
    EventPoller::Ptr poller;
    std::weak_ptr<HlsMediaSource> src;
    HlsMediaSource::RingType::RingReader::Ptr reader;
    uint64_t generation = 0;
    bool attaching = false;
    bool attached = false;
};

HlsCookieData::HlsCookieData(const MediaInfo &info, const std::shared_ptr<Session> &session,
                             std::string session_id) {
    _info = info;
    _session_id = std::move(session_id);
    auto sock_info = std::make_shared<SockInfoImp>();
    sock_info->_identifier = session->getIdentifier();
    sock_info->_peer_ip = session->get_peer_ip();
    sock_info->_peer_port = session->get_peer_port();
    sock_info->_local_ip = session->get_local_ip();
    sock_info->_local_port = session->get_local_port();
    _sock_info = sock_info;
    _session = session;
    _attachment = std::make_shared<AttachmentState>();
    _attachment->poller = session->getPoller();
    addReaderCount();
}

void HlsCookieData::addReaderCount() {
    attachToSource(getMediaSource());
}

void HlsCookieData::attachToSource(const HlsMediaSource::Ptr &src) {
    if (!src || !src->getRing()) {
        return;
    }

    auto state = _attachment;
    HlsMediaSource::RingType::RingReader::Ptr old_reader;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lck(state->mtx);
        auto current = state->src.lock();
        if (current == src && (state->attached || state->attaching)) {
            return;
        }
        ++state->generation;
        generation = state->generation;
        state->src = src;
        state->attaching = true;
        state->attached = false;
        old_reader.swap(state->reader);
    }

    // Releasing a reader may invoke its detach callback. Do not do that while
    // holding the attachment mutex.
    old_reader.reset();

    std::weak_ptr<AttachmentState> weak_state = state;
    std::weak_ptr<Session> weak_session = _session;
    auto poller = state->poller;
    poller->async([weak_state, weak_session, src, poller, generation]() {
        auto strong_state = weak_state.lock();
        if (!strong_state) {
            return;
        }
        {
            std::lock_guard<std::mutex> lck(strong_state->mtx);
            if (strong_state->generation != generation || strong_state->src.lock() != src) {
                return;
            }
        }

        auto reader = src->getRing()->attach(poller);
        reader->setDetachCB([weak_state, generation]() {
            auto state = weak_state.lock();
            if (!state) {
                return;
            }
            std::lock_guard<std::mutex> lck(state->mtx);
            if (state->generation == generation) {
                state->attached = false;
                state->attaching = false;
            }
        });
        reader->setGetInfoCB([weak_session]() {
            Any ret;
            ret.set(std::static_pointer_cast<Session>(weak_session.lock()));
            return ret;
        });

        bool stale = false;
        {
            std::lock_guard<std::mutex> lck(strong_state->mtx);
            if (strong_state->generation != generation || strong_state->src.lock() != src) {
                stale = true;
            } else {
                strong_state->reader = reader;
                strong_state->attaching = false;
                strong_state->attached = true;
            }
        }
        if (stale) {
            reader.reset();
        }
    });
}

HlsCookieData::~HlsCookieData() {
    bool attached = false;
    {
        std::lock_guard<std::mutex> lck(_attachment->mtx);
        attached = _attachment->attached;
    }
    if (attached) {
        uint64_t duration = 0;
        {
            std::lock_guard<std::mutex> lck(_activity_mtx);
            duration = (_ticker.createdTime() - _ticker.elapsedTime()) / 1000;
        }
        WarnL << _sock_info->getIdentifier() << "(" << _sock_info->get_peer_ip() << ":" << _sock_info->get_peer_port()
              << ") " << "HLS player (" << _info.shortUrl() << ") disconnected, time-consuming(s):" << duration;

        GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
        uint64_t bytes = _bytes.load();
        if (bytes >= iFlowThreshold * 1024) {
            try {
                NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _info, bytes, duration, true, *_sock_info);
            } catch (std::exception &ex) {
                WarnL << "Exception occurred: " << ex.what();
            }
        }
    }
}

void HlsCookieData::addByteUsage(size_t bytes) {
    addReaderCount();
    _bytes += bytes;
    std::lock_guard<std::mutex> lck(_activity_mtx);
    _ticker.resetTime();
}

void HlsCookieData::setMediaSource(const HlsMediaSource::Ptr &src) {
    if (!src) {
        return;
    }
    attachToSource(src);
}

HlsMediaSource::Ptr HlsCookieData::getMediaSource() const {
    std::lock_guard<std::mutex> lck(_attachment->mtx);
    return _attachment->src.lock();
}

void HlsMediaSource::setIndexFile(std::string index_file)
{
    if (!_ring) {
        std::weak_ptr<HlsMediaSource> weakSelf = std::static_pointer_cast<HlsMediaSource>(shared_from_this());
        auto lam = [weakSelf](int size) {
            auto strongSelf = weakSelf.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->onReaderChanged(size);
        };
        _ring = std::make_shared<RingType>(0, std::move(lam));
        regist();
    }

    // Assign m3u8 index file content
    std::lock_guard<std::mutex> lck(_mtx_index);
    _index_file = std::move(index_file);

    if (!_index_file.empty()) {
        _list_cb.for_each([&](const std::function<void(const std::string& str)>& cb) { cb(_index_file); });
        _list_cb.clear();
    }
}

void HlsMediaSource::getIndexFile(std::function<void(const std::string& str)> cb)
{
    std::lock_guard<std::mutex> lck(_mtx_index);
    if (!_index_file.empty()) {
        cb(_index_file);
        return;
    }
    // Waiting for m3u8 file generation
    _list_cb.emplace_back(std::move(cb));
}

} // namespace mediakit
