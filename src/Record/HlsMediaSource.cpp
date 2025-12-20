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

HlsCookieData::HlsCookieData(const MediaInfo &info, const std::shared_ptr<Session> &session) {
    _info = info;
    auto sock_info = std::make_shared<SockInfoImp>();
    sock_info->_identifier = session->getIdentifier();
    sock_info->_peer_ip = session->get_peer_ip();
    sock_info->_peer_port = session->get_peer_port();
    sock_info->_local_ip = session->get_local_ip();
    sock_info->_local_port = session->get_local_port();
    _sock_info = sock_info;
    _session = session;
    _added = std::make_shared<bool>(false);
    addReaderCount();
}

void HlsCookieData::addReaderCount() {
    if (!*_added) {
        auto src = getMediaSource();
        if (src) {
            *_added = true;
            _ring_reader = src->getRing()->attach(EventPollerPool::Instance().getPoller());
            auto added = _added;
            _ring_reader->setDetachCB([added]() {
                // HlsMediaSource has been destroyed
                *added = false;
            });
            std::weak_ptr<Session> weak_session = _session;
            _ring_reader->setGetInfoCB([weak_session]() {
                Any ret;
                ret.set(std::static_pointer_cast<Session>(weak_session.lock()));
                return ret;
            });
        }
    }
}

HlsCookieData::~HlsCookieData() {
    if (*_added) {
        uint64_t duration = (_ticker.createdTime() - _ticker.elapsedTime()) / 1000;
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
    _ticker.resetTime();
}

void HlsCookieData::setMediaSource(const HlsMediaSource::Ptr &src) {
    _src = src;
}

HlsMediaSource::Ptr HlsCookieData::getMediaSource() const {
    return _src.lock();
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
