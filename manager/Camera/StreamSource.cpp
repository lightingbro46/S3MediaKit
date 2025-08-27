#include "StreamSource.h"
#include "Extension/Plugin.h"
#include "../../server/WebApi.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

bool equalStreamTuple(const StreamTuple &a, const StreamTuple &b) {
    return a.vhost == b.vhost && a.device_id == b.device_id &&
            a.stream_id == b.stream_id && a.full_url == b.full_url;
}

const string getStreamTypeString(int type) {
#define SWITCH_CASE(type) case type : return #type
    switch (type) {
        SWITCH_CASE(PrimaryStream);
        SWITCH_CASE(SecondaryStream);
        default : return "unknown";
    }
}

StreamSource::StreamSource(const StreamTuple &tuple, bool record, int rtp_type, int media_port, float timeout_sec) 
    : _tuple(std::move(tuple)), _record(record), _rtp_type(rtp_type), _media_port(media_port), _timeout_sec(timeout_sec) {

    _full_url = tuple.full_url;
    if (_media_port) {
        _full_url = replacePort(tuple.full_url, _media_port);
    }
}

StreamSource::~StreamSource() {
    closePlayer();
}

void StreamSource::start() {
    createPlayer();
}

void StreamSource::createPlayer() {
    MediaTuple tuple = { .vhost = DEFAULT_VHOST, .app = _tuple.device_id, .stream = _tuple.stream_id, .params = "" };

    ProtocolOption option;
    option.enable_mp4 = _record;

    weak_ptr<StreamSource> weak_self = shared_from_this();
    auto setup_player = [weak_self](const string &err, const PlayerProxy::Ptr &player) {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return;
        }
        (*player)[Client::kRtpType] = strong_self->_rtp_type;

        if (strong_self->_timeout_sec > 0.1f) {
            // Play handshake timeout
            (*player)[Client::kTimeoutMS] = strong_self->_timeout_sec * 1000;
        }

        player->setPlayCallbackOnce([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_status = ex.what();

            if (strong_self->_on_ready){
                strong_self->_on_ready();
            }
        });

        player->setOnConnect([weak_self](const TranslationInfo &info) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_live = true;
            strong_self->_info = info;
            strong_self->_status.clear();
        });

        player->setOnDisconnect([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }   
            strong_self->_live = false;
        });

        player->setOnClose([weak_self](const SockException &ex) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->_status = ex.what();
        });

        player->play(strong_self->_tuple.full_url);
        strong_self->_player = player;
        DebugL << "Create stream player proxy: " << strong_self->_tuple.shortUrl();
    };

    addStreamProxy(tuple, option, setup_player);
}

void StreamSource::closePlayer() {
    MediaTuple tuple = {
        .vhost = DEFAULT_VHOST,
        .app = _tuple.device_id,
        .stream = _tuple.stream_id,
        .params = ""
    };
    delStreamProxy(tuple);
    _player.reset();
    DebugL << "Close stream player proxy: " << _tuple.shortUrl();
}

} // namespace managerkit
