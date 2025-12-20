#include "Util/util.h"
#include "Common/config.h"
#include "WebRtcTransport.h"
#include "WebRtcSignalingMsg.h"
#include "WebRtcSignalingSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit::Rtc;

namespace mediakit {

// Registered peers list
static std::atomic<uint32_t> s_room_idx_generate { 1 };
static ServiceController<WebRtcSignalingSession> s_rooms;

void listWebrtcRooms(const std::function<void(const std::string &key, const WebRtcSignalingSession::Ptr &p)> &cb) {
    s_rooms.for_each(cb);
}

Json::Value ToJson(const WebRtcSignalingSession::Ptr &p) {
    return p->makeInfoJson();
}

WebRtcSignalingSession::Ptr getWebrtcRoomKeeper(const string &room_id) {
    return s_rooms.find(room_id);
}

////////////  WebRtcSignalingSession //////////////////////////

WebRtcSignalingSession::WebRtcSignalingSession(const Socket::Ptr &sock) : Session(sock) {
    DebugL;
}

WebRtcSignalingSession::~WebRtcSignalingSession() {
    DebugL << "room_id: " << _room_id;
}

void WebRtcSignalingSession::onRecv(const Buffer::Ptr &buffer) {
    DebugL << "recv msg:\r\n" << buffer->data();

    Json::Value args;
    Json::Reader reader;
    reader.parse(buffer->data(), args);
    Parser parser;
    HttpAllArgs<decltype(args)> allArgs(parser, args);

    using MsgHandler = void (WebRtcSignalingSession::*)(SIGNALING_MSG_ARGS);
    static std::unordered_map<std::pair<std::string /*class*/, std::string /*method*/>, MsgHandler, ClassMethodHash> s_msg_handlers;

    static onceToken token([]() {
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_REQUEST, METHOD_VALUE_REGISTER), &WebRtcSignalingSession::handleRegisterRequest);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_REQUEST, METHOD_VALUE_UNREGISTER), &WebRtcSignalingSession::handleUnregisterRequest);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_REQUEST, METHOD_VALUE_CALL), &WebRtcSignalingSession::handleCallRequest);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_ACCEPT, METHOD_VALUE_CALL), &WebRtcSignalingSession::handleCallAccept);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_REJECT, METHOD_VALUE_CALL), &WebRtcSignalingSession::handleCallReject);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_INDICATION, METHOD_VALUE_BYE), &WebRtcSignalingSession::handleByeIndication);
        s_msg_handlers.emplace(std::make_pair(CLASS_VALUE_INDICATION, METHOD_VALUE_CANDIDATE), &WebRtcSignalingSession::handleCandidateIndication);
    });

    try {
        CHECK_ARGS(CLASS_KEY, METHOD_KEY, TRANSACTION_ID_KEY);
        auto it = s_msg_handlers.find(std::make_pair(allArgs[CLASS_KEY], allArgs[METHOD_KEY]));
        if (it == s_msg_handlers.end()) {
            WarnL << " not support class: " << allArgs[CLASS_KEY] << ", method: " << allArgs[METHOD_KEY] << ", ignore";
            return;
        }

        (this->*(it->second))(allArgs);
    } catch (std::exception &ex) {
        WarnL << "process msg fail: " << ex.what();
    }
}

void WebRtcSignalingSession::onError(const SockException &err) {
    WarnL << "room_id: " << _room_id;
    notifyByeIndication();
    s_rooms.erase(_room_id);
}

void WebRtcSignalingSession::onManager() {
    // The Websocket session will send PING/PONG messages regularly and manage the timeout by itself. This object does not need heartbeat timeout processing for the time being.
}

void WebRtcSignalingSession::handleRegisterRequest(SIGNALING_MSG_ARGS) {
    DebugL;

    std::string room_id;
    Json::Value body;
    body[METHOD_KEY] = METHOD_VALUE_REGISTER;

    // If the client does not provide a room_id, the server automatically assigns one.
    if (allArgs[ROOM_ID_KEY].empty()) {
        auto idx = s_room_idx_generate.fetch_add(1);
        room_id = std::to_string(idx) + "_" + makeRandStr(16);
        DebugL << "auto generated room_id: " << room_id;
    } else {
        room_id = allArgs[ROOM_ID_KEY];
        if (s_rooms.find(room_id)) {
            // Already registered
            body[ROOM_ID_KEY] = room_id;
            return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "room id conflict");
        }
    }

    body[ROOM_ID_KEY] = room_id;

    _room_id = room_id;
    s_rooms.emplace(_room_id, shared_from_this());
    sendRegisterAccept(body, allArgs[TRANSACTION_ID_KEY]);
}

void WebRtcSignalingSession::handleUnregisterRequest(SIGNALING_MSG_ARGS) {
    DebugL;
    CHECK_ARGS(ROOM_ID_KEY);

    Json::Value body;
    body[METHOD_KEY] = METHOD_VALUE_UNREGISTER;
    body[ROOM_ID_KEY] = allArgs[ROOM_ID_KEY];

    if (_room_id.empty()) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "unregistered");
    }

    if (allArgs[ROOM_ID_KEY] != getRoomId()) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], StrPrinter << "room_id: \"" << allArgs[ROOM_ID_KEY] << "\" not match room_id:" << getRoomId());
    }

    sendAcceptResponse(body, allArgs[TRANSACTION_ID_KEY]);

    // At the same time, actively send bye to all connected peer sessions.
    notifyByeIndication();

    if (s_rooms.find(_room_id)) {
        s_rooms.erase(_room_id);
    }
}

void WebRtcSignalingSession::handleCallRequest(SIGNALING_MSG_ARGS) {
    DebugL;
    CHECK_ARGS(TRANSACTION_ID_KEY, GUEST_ID_KEY, ROOM_ID_KEY, CALL_VHOST_KEY, CALL_APP_KEY, CALL_STREAM_KEY, TYPE_KEY, SDP_KEY);

    Json::Value body;
    body[METHOD_KEY] = METHOD_VALUE_CALL;
    body[ROOM_ID_KEY] = allArgs[ROOM_ID_KEY];
    body[GUEST_ID_KEY] = allArgs[GUEST_ID_KEY];
    body[CALL_VHOST_KEY] = allArgs[CALL_VHOST_KEY];
    body[CALL_APP_KEY] = allArgs[CALL_APP_KEY];
    body[CALL_STREAM_KEY] = allArgs[CALL_STREAM_KEY];
    body[TYPE_KEY] = allArgs[TYPE_KEY];
    if (_room_id.empty()) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "should register first");
    }
    auto peer_id = allArgs[ROOM_ID_KEY];
    auto session = getWebrtcRoomKeeper(peer_id);
    if (!session) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], StrPrinter << "room_id: \"" << peer_id << "\" unregistered");
    }

    _tours.emplace(allArgs[GUEST_ID_KEY], peer_id);
    // forwardOffer
    weak_ptr<WebRtcSignalingSession> sender_ptr = static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    session->forwardCallRequest(sender_ptr, allArgs);
}

void WebRtcSignalingSession::handleCallAccept(SIGNALING_MSG_ARGS) {
    DebugL;
    CHECK_ARGS(GUEST_ID_KEY, ROOM_ID_KEY, CALL_VHOST_KEY, CALL_APP_KEY, CALL_STREAM_KEY);

    Json::Value body;
    body[ROOM_ID_KEY] = allArgs[ROOM_ID_KEY];

    if (_room_id.empty()) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "should register first");
    }

    auto it = _guests.find(allArgs[GUEST_ID_KEY]);
    if (it == _guests.end()) {
        WarnL << "guest_id: \"" << allArgs[GUEST_ID_KEY] << "\" not register";
        return;
    }
    auto session = it->second.lock();
    if (!session) {
        WarnL << "guest_id: \"" << allArgs[GUEST_ID_KEY] << "\" leave alreadly";
        return;
    }

    session->forwardCallAccept(allArgs);
}

void WebRtcSignalingSession::handleByeIndication(SIGNALING_MSG_ARGS) {
    DebugL;
    CHECK_ARGS(GUEST_ID_KEY, ROOM_ID_KEY);
    auto guest_id = allArgs[GUEST_ID_KEY];

    Json::Value body;
    body[METHOD_KEY] = METHOD_VALUE_BYE;
    body[ROOM_ID_KEY] = allArgs[ROOM_ID_KEY];
    body[GUEST_ID_KEY] = guest_id;
    if (_room_id.empty()) {
        return sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "should register first");
    }
    if (allArgs[ROOM_ID_KEY] == getRoomId()) {
        // As the called party, receive bye
        auto it = _guests.find(guest_id);
        if (it == _guests.end()) {
            WarnL << "guest_id: \"" << guest_id << "\" not register";
            return;
        }
        auto session = it->second.lock();
        if (!session) {
            WarnL << "guest_id: \"" << guest_id << "\" leave alreadly";
            return;
        }
        _guests.erase(guest_id);
        session->forwardBye(allArgs);
    } else {
        // As the calling party, accept bye
        auto session = getWebrtcRoomKeeper(allArgs[ROOM_ID_KEY]);
        if (!session) {
            WarnL << "room_id: \"" << allArgs[ROOM_ID_KEY] << "\" not register";
            return;
        }
        _tours.erase(guest_id);
        session->forwardBye(allArgs);
    }
}

void WebRtcSignalingSession::handleCandidateIndication(SIGNALING_MSG_ARGS) {
    DebugL;
    CHECK_ARGS(TRANSACTION_ID_KEY, GUEST_ID_KEY, ROOM_ID_KEY, CANDIDATE_KEY, UFRAG_KEY, PWD_KEY);

    Json::Value body;
    body[METHOD_KEY] = METHOD_VALUE_CANDIDATE;
    body[ROOM_ID_KEY] = allArgs[ROOM_ID_KEY];

    if (_room_id.empty()) {
        sendRejectResponse(body, allArgs[TRANSACTION_ID_KEY], "should register first");
    } else {
        handleOtherMsg(allArgs);
    }
}

void WebRtcSignalingSession::handleOtherMsg(SIGNALING_MSG_ARGS) {
    DebugL;
    if (allArgs[ROOM_ID_KEY] == getRoomId()) {
        // As the called party, receive bye
        auto guest_id = allArgs[GUEST_ID_KEY];
        auto it = _guests.find(guest_id);
        if (it == _guests.end()) {
            WarnL << "guest_id: \"" << guest_id << "\" not register";
            return;
        }
        auto session = it->second.lock();
        if (!session) {
            WarnL << "guest_id: \"" << guest_id << "\" leave alreadly";
            return;
        }

        session->forwardPacket(allArgs);
    } else {
        // As the calling party, accept bye
        auto session = getWebrtcRoomKeeper(allArgs[ROOM_ID_KEY]);
        if (!session) {
            WarnL << "room_id: \"" << allArgs[ROOM_ID_KEY] << "\" not register";
            return;
        }
        session->forwardPacket(allArgs);
    }
}

void WebRtcSignalingSession::notifyByeIndication() {
    DebugL;

    Json::Value allArgs;
    allArgs[CLASS_KEY] = CLASS_VALUE_INDICATION;
    allArgs[METHOD_KEY] = METHOD_VALUE_BYE;
    allArgs[REASON_KEY] = "peer unregister";
    // As the called party
    for (auto it : _guests) {
        auto session = it.second.lock();
        if (session) {
            allArgs[TRANSACTION_ID_KEY] = makeRandStr(32);
            allArgs[GUEST_ID_KEY] = it.first;
            allArgs[ROOM_ID_KEY] = getRoomId();
            session->forwardBye(allArgs);
        }
    }

    // As the calling party
    for (auto it : _tours) {
        auto guest_id = it.first;
        auto peer_room_id = it.second;
        auto session = getWebrtcRoomKeeper(peer_room_id);
        if (session) {
            allArgs[TRANSACTION_ID_KEY] = makeRandStr(32);
            allArgs[GUEST_ID_KEY] = guest_id;
            allArgs[ROOM_ID_KEY] = peer_room_id;
            session->forwardBye(allArgs);
        }
    }
}

void WebRtcSignalingSession::forwardCallRequest(WebRtcSignalingSession::WeakPtr sender, SIGNALING_MSG_ARGS) {
    DebugL;
    WeakPtr weak_self = std::static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    getPoller()->async([weak_self, sender, allArgs]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->_guests.emplace(allArgs[GUEST_ID_KEY], sender);
            strong_self->sendPacket(allArgs.getArgs());
        }
    });
}

void WebRtcSignalingSession::forwardCallAccept(SIGNALING_MSG_ARGS) {
    DebugL;
    WeakPtr weak_self = std::static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    getPoller()->async([weak_self, allArgs]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->sendPacket(allArgs.getArgs());
        }
    });
}

void WebRtcSignalingSession::forwardBye(SIGNALING_MSG_ARGS) {
    DebugL;
    WeakPtr weak_self = std::static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    getPoller()->async([weak_self, allArgs]() {
        if (auto strong_self = weak_self.lock()) {
            if (allArgs[ROOM_ID_KEY] == strong_self->getRoomId()) {
                // as called
                strong_self->_guests.erase(allArgs[GUEST_ID_KEY]);
            } else {
                // as caller
                strong_self->_tours.erase(allArgs[GUEST_ID_KEY]);
            }
            strong_self->sendPacket(allArgs.getArgs());
        }
    });
}

void WebRtcSignalingSession::forwardBye(Json::Value allArgs) {
    DebugL;
    WeakPtr weak_self = std::static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    getPoller()->async([weak_self, allArgs]() {
        if (auto strong_self = weak_self.lock()) {
            if (allArgs[ROOM_ID_KEY] == strong_self->getRoomId()) {
                // as called
                strong_self->_guests.erase(allArgs[GUEST_ID_KEY].asString());
            } else {
                // as caller
                strong_self->_tours.erase(allArgs[GUEST_ID_KEY].asString());
            }
            strong_self->sendPacket(allArgs);
        }
    });
}

void WebRtcSignalingSession::forwardPacket(SIGNALING_MSG_ARGS) {
    WeakPtr weak_self = std::static_pointer_cast<WebRtcSignalingSession>(shared_from_this());
    getPoller()->async([weak_self, allArgs]() {
        if (auto strong_self = weak_self.lock()) {
            strong_self->sendPacket(allArgs.getArgs());
        }
    });
}

void WebRtcSignalingSession::sendRegisterAccept(Json::Value& body, const std::string& transaction_id) {
    DebugL;
    body[CLASS_KEY] = CLASS_VALUE_ACCEPT;

    Json::Value ice_server;
    GET_CONFIG(uint16_t, icePort, Rtc::kIcePort);
    GET_CONFIG(bool, enable_turn, Rtc::kEnableTurn);
    GET_CONFIG(string, iceUfrag, Rtc::kIceUfrag);
    GET_CONFIG(string, icePwd, Rtc::kIcePwd);
    GET_CONFIG_FUNC(std::vector<std::string>, extern_ips, Rtc::kExternIP, [](string str) {
        std::vector<std::string> ret;
        if (str.length()) {
            ret = split(str, ",");
        }
        translateIPFromEnv(ret);
        return ret;
    });

    // If extern_ips is configured, select the first IP as the turn server
    // If not configured, get the network interface
    std::string extern_ip;
    if (!extern_ips.empty()) {
        extern_ip = extern_ips.front();
    } else {
        extern_ip = SockUtil::get_local_ip();
    }

    // TODO: support multi extern ip
    // TODO: support third stun/turn server

    std::string url;
    // SUPPORT:
    // stun:host:port?transport=udp
    // turn:host:port?transport=udp

    // NOT SUPPORT NOW TODO:
    // turns:host:port?transport=udp
    // turn:host:port?transport=tcp
    // turns:host:port?transport=tcp
    // stuns:host:port?transport=udp
    // stuns:host:port?transport=udp
    // stun:host:port?transport=tcp
    if (enable_turn) {
        url = "turn:" + extern_ip + ":" + std::to_string(icePort) + "?transport=udp";
    } else {
        url = "stun:" + extern_ip + ":" + std::to_string(icePort) + "?transport=udp";
    }

    ice_server[URL_KEY] = url;
    ice_server[UFRAG_KEY] = iceUfrag;
    ice_server[PWD_KEY] = icePwd;

    Json::Value ice_servers;
    ice_servers.append(ice_server);

    body[ICE_SERVERS_KEY] = ice_servers;

    sendAcceptResponse(body, transaction_id);
}

void WebRtcSignalingSession::sendAcceptResponse(Json::Value &body, const std::string &transaction_id) {
    TraceL;
    body[CLASS_KEY] = CLASS_VALUE_ACCEPT;
    return sendResponse(body, transaction_id);
}

void WebRtcSignalingSession::sendRejectResponse(Json::Value &body, const std::string &transaction_id, const std::string &reason) {
    DebugL;
    body[CLASS_KEY] = CLASS_VALUE_REJECT;
    body[REASON_KEY] = reason;
    return sendResponse(body, transaction_id);
}

void WebRtcSignalingSession::sendResponse(Json::Value &body, const std::string &transaction_id) {
    DebugL;
    body[TRANSACTION_ID_KEY] = transaction_id;
    return sendPacket(body);
}

void WebRtcSignalingSession::sendPacket(const Json::Value &body) {
    auto msg = body.toStyledString();
    TraceL << "send msg: " << msg;
    SockSender::send(msg);
}

Json::Value WebRtcSignalingSession::makeInfoJson() {
    Json::Value item;
    item["room_id"] = getRoomId();

    Json::Value tours_obj(Json::arrayValue);
    auto tours = _tours;
    for (auto &tour : tours) {
        Json::Value obj;
        obj["guest_id"] = tour.first;
        obj["room_id"] = tour.second;
        tours_obj.append(std::move(obj));
    }
    item["tours"] = std::move(tours_obj);

    Json::Value guests_obj(Json::arrayValue);
    auto guests = _guests;
    for (auto &guest : guests) {
        Json::Value obj;
        obj["guest_id"] = guest.first;
        guests_obj.append(std::move(obj));
    }
    item["guests"] = std::move(guests_obj);
    return item;
}

} // namespace mediakit
