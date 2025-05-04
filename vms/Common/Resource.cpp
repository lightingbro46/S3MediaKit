#include <iostream>
#include <random>
#include <sstream>
#include <iomanip>
#include <mutex>
#include "Util/util.h"
#include "Util/NoticeCenter.h"
#include "Network/sockutil.h"
#include "Network/Session.h"
#include "Resource.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace toolkit {
StatisticImp(vmskit::Resource);
}

namespace mediakit {
namespace Manager {
const std::string kMaxResourceWaitTimeMS = "15.0";
}

namespace Broadcast {
const string kBroadcastResourceChanged = "kBroadcastResourceChanged";
const string kBroadcastNotFoundResource = "kBroadcastNotFoundResource";    
}
}

namespace vmskit {

std::string generateGuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist32(0, 0xFFFFFFFF);
    std::uniform_int_distribution<uint16_t> dist16(0, 0xFFFF);
    std::uniform_int_distribution<uint8_t>  dist8(0, 0xFF);

    std::stringstream ss;

    ss << std::hex << std::setfill('0');
    ss << std::setw(8) << dist32(gen) << "-";
    ss << std::setw(4) << dist16(gen) << "-";
    ss << std::setw(4) << dist16(gen) << "-";

    // Ensure first two bits of first byte of D4 are 10xx (variant 1)
    uint16_t d4 = (dist8(gen) & 0x3F) | 0x80;
    ss << std::setw(2) << static_cast<int>(d4);
    ss << std::setw(2) << static_cast<int>(dist8(gen)) << "-";

    for (int i = 0; i < 6; ++i)
        ss << std::setw(2) << static_cast<int>(dist8(gen));

    return ss.str();
}

static recursive_mutex s_resource_mtx;
using ResourceMap = unordered_map<string/*id*/, weak_ptr<Resource> >;
using AppResourceMap = unordered_map<string/*app*/, ResourceMap>;
using VhostAppResourceMap = unordered_map<string/*vhost*/, AppResourceMap>;
using SchemaVhostAppResourceMap = unordered_map<string/*schema*/, VhostAppResourceMap>;
static SchemaVhostAppResourceMap s_resource_map;

string getOriginTypeString(ResourceOriginType type) {
#define SWITCH_CASE(type) case ResourceOriginType::type : return #type
    switch (type) {
        SWITCH_CASE(unknown);
        SWITCH_CASE(camera);
        SWITCH_CASE(virtual_camera);
        default : return "unknown";
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

ResourceOption::ResourceOption() {}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ResourceNull : public Resource {
    ResourceNull() : Resource("schema", ResourceTuple { "vhost", "app", "id", "" }) {};
    int readerCount() override { return 0; }
};

Resource &Resource::NullResource() {
    static std::shared_ptr<Resource> s_null = std::make_shared<ResourceNull>();
    return *s_null;
}

Resource::Resource(const string &schema, const ResourceTuple &tuple) : _tuple(tuple) {
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost || _tuple.vhost.empty()) {
        _tuple.vhost = DEFAULT_VHOST;
    }
    _schema = schema;
    _create_stamp = time(NULL);
}

Resource::~Resource() {
    try {
        unregist();
    } catch (std::exception &ex) {
        WarnL << "Exception occurred: " << ex.what();
    }
}

std::shared_ptr<void> Resource::getOwnership() {
    if (_owned.test_and_set()) {
        // Already owned by all
        return nullptr;
    }
    weak_ptr<Resource> weak_self = shared_from_this();
    // Ensure that the returned Ownership smart pointer is not empty, 0x01 has no practical meaning
    return std::shared_ptr<void>((void *) 0x01, [weak_self](void *ptr) {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->_owned.clear();
        }
    });
}

uint64_t Resource::getAliveSecond() const {
    // The purpose of using the Ticker object to obtain the survival time is to prevent the modification of the system time from causing a rollback
    return _ticker.createdTime() / 1000;
}

void Resource::setListener(const std::weak_ptr<ResourceEvent> &listener){
    _listener = listener;
}

std::weak_ptr<ResourceEvent> Resource::getListener() const {
    return _listener;
}

int Resource::totalReaderCount(){
    auto listener = _listener.lock();
    if(!listener){
        return readerCount();
    }
    return listener->totalReaderCount(*this);
}

ResourceOriginType Resource::getOriginType() const {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceOriginType::unknown;
    }
    return listener->getOriginType(const_cast<Resource &>(*this));
}

string Resource::getOriginUrl() const {
    auto listener = _listener.lock();
    if (!listener) {
        return getUrl();
    }
    auto ret = listener->getOriginUrl(const_cast<Resource &>(*this));
    if (!ret.empty()) {
        return ret;
    }
    return getUrl();
}

std::shared_ptr<SockInfo> Resource::getOriginSock() const {
    auto listener = _listener.lock();
    if (!listener) {
        return nullptr;
    }
    return listener->getOriginSock(const_cast<Resource &>(*this));
}

bool Resource::pause(bool pause) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    return listener->pause(*this, pause);
}

bool Resource::close(bool force) {
    auto listener = _listener.lock();
    if (!listener) {
        return false;
    }
    if (!force && totalReaderCount()) {
        // Someone is watching, do not force close
        return false;
    }
    return listener->close(*this);
}

toolkit::EventPoller::Ptr Resource::getOwnerPoller() {
    toolkit::EventPoller::Ptr ret;
    auto listener = _listener.lock();
    if (listener) {
        return listener->getOwnerPoller(*this);
    }
    throw std::runtime_error(toolkit::demangle(typeid(*this).name()) + "::getOwnerPoller failed: " + getUrl());
}

void Resource::onReaderChanged(int size) {
    try {
        weak_ptr<Resource> weak_self = shared_from_this();
        getOwnerPoller()->async([weak_self, size]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            auto listener = strong_self->_listener.lock();
            if (listener) {
                listener->onReaderChanged(*strong_self, size);
            }
        });
    } catch (ResourceEvent::NotImplemented &ex) {
        // The interface is not implemented, an exception should be printed
        WarnL << ex.what();
    } catch (...) {
        // The getOwnerPoller() interface should only throw exceptions externally, not internally
        // Therefore, the exception that the listener has been destroyed and the ownership thread cannot be obtained is directly ignored
    }
}

template<typename MAP, typename LIST, typename First, typename ...KeyTypes>
static void for_each_resource_l(const MAP &map, LIST &list, const First &first, const KeyTypes &...keys) {
    if (first.empty()) {
        for (auto &pr : map) {
            for_each_resource_l(pr.second, list, keys...);
        }
        return;
    }
    auto it = map.find(first);
    if (it != map.end()) {
        for_each_resource_l(it->second, list, keys...);
    }
}

template<typename LIST, typename Ptr>
static void emplace_back(LIST &list, const Ptr &ptr) {
    auto src = ptr.lock();
    if (src) {
        list.emplace_back(std::move(src));
    }
}

template<typename MAP, typename LIST, typename First>
static void for_each_resource_l(const MAP &map, LIST &list, const First &first) {
    if (first.empty()) {
        for (auto &pr : map) {
            emplace_back(list, pr.second);
        }
        return;
    }
    auto it = map.find(first);
    if (it != map.end()) {
        emplace_back(list, it->second);
    }
}

void Resource::for_each_resource(const function<void(const Ptr &src)> &cb,
                                 const string &schema,
                                 const string &vhost,
                                 const string &app,
                                 const string &id) {
    deque<Ptr> src_list;
    {
        lock_guard<recursive_mutex> lock(s_resource_mtx);
        for_each_resource_l(s_resource_map, src_list, schema, vhost, app, id);
    }
    for (auto &src : src_list) {
        cb(src);
    }
}

static Resource::Ptr find_l(const string &schema, const string &vhost_in, const string &app, const string &id) {
    string vhost = vhost_in;
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if(vhost.empty() || !enableVhost){
        vhost = DEFAULT_VHOST;
    }

    if (app.empty() || id.empty()) {
        // If no app and resource id are specified, then it is traversal instead of searching, so it should return search failure
        return nullptr;
    }

    Resource::Ptr ret;
    Resource::for_each_resource([&](const Resource::Ptr &src) { ret = std::move(const_cast<Resource::Ptr &>(src)); }, schema, vhost, app, id);
    
    return ret;
}

static void findAsync_l(const ResourceInfo &info, const std::shared_ptr<Session> &session, bool retry,
                        const function<void(const Resource::Ptr &src)> &cb) {
    auto src = find_l(info.schema, info.vhost, info.app, info.id);
    if (src || !retry) {
        cb(src);
        return;
    }
    GET_CONFIG(int, maxWaitMS, Manager::kMaxResourceWaitTimeMS);
    void *listener_tag = session.get();
    auto poller = session->getPoller();
    std::shared_ptr<atomic_flag> invoked(new atomic_flag { false });
    auto cb_once = [cb, invoked](const Resource::Ptr &src) {
        if (invoked->test_and_set()) {
            // The callback has already been executed
            return;
        }
        cb(src);
    };

    auto on_timeout = poller->doDelayTask(maxWaitMS, [cb_once, listener_tag]() {
        // Wait for a certain amount of time at most, if the resource is not registered within this time, return empty
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastResourceChanged);
        cb_once(nullptr);
        return 0;
    });

    auto cancel_all = [on_timeout, listener_tag]() {
        // Cancel the delayed task to prevent multiple callbacks
        on_timeout->cancel();
        // Cancel the media registration event listener
        NoticeCenter::Instance().delListener(listener_tag, Broadcast::kBroadcastMediaChanged);
    };

    weak_ptr<Session> weak_session = session;
    auto on_register = [weak_session, info, cb_once, cancel_all, poller](BroadcastResourceChangedArgs) {
        if (!bRegist ||
            sender.getSchema() != info.schema ||
            !equalResourceTuple(sender.getResourceTuple(), info)) {
            // Not an event of interest, ignore it
            return;
        }

        poller->async([weak_session, cancel_all, info, cb_once]() {
            cancel_all();
            if (auto strong_session = weak_session.lock()) {
                // The resource requested by the player is finally registered, switch to its own thread and reply
                DebugL << "Receive resource registration event, reply to player:" << info.getUrl();
                // Find the resource again, usually it can be found
                findAsync_l(info, strong_session, false, cb_once);
            }
        }, false);
    };

    // Listen for resource registration events
    NoticeCenter::Instance().addListener(listener_tag, Broadcast::kBroadcastResourceChanged, on_register);

    function<void()> close_player = [cb_once, cancel_all, poller]() {
        poller->async([cancel_all, cb_once]() {
            cancel_all();
            // Tell the player that the resource does not exist, so it will immediately disconnect the player
            cb_once(nullptr);
        });
    };
    // Broadcast that the resource is not found, at this time you can immediately pull the resource, so it is still in time
    NOTICE_EMIT(BroadcastNotFoundResourceArgs, Broadcast::kBroadcastNotFoundResource, info, *session, close_player);
};

void Resource::findAsync(const ResourceInfo &info, const std::shared_ptr<Session> &session, const function<void(const Ptr &)> &cb) {
    return findAsync_l(info, session, true, cb);
}

Resource::Ptr Resource::find(const string &schema, const string &vhost, const string &app, const string &id) {
    return find_l(schema, vhost, app, id);
}

Resource::Ptr Resource::find(const string &vhost, const string &app, const string &id) {
    auto src = Resource::find(CAMERA_SCHEMA, vhost, app, id);
    if (src) {
        return src;
    }
    
}



} // namespace vmskit