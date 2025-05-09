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
#include "Common/Parser.h"
#include "ManagerHook.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace toolkit {
StatisticImp(managerkit::Resource);
}

namespace managerkit {

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
        SWITCH_CASE(user);
        SWITCH_CASE(server);
        SWITCH_CASE(file_storage);
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

std::shared_ptr<MultiResourceMuxer> Resource::getMuxer() const {
    auto listener = _listener.lock();
    return listener ? listener->getMuxer(const_cast<Resource&>(*this)) : nullptr;
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

bool Resource::setupRecord(bool start, const string &custom_path, size_t max_second){
    auto listener = _listener.lock();
    if (!listener) {
        WarnL << "The event listener of Resource is not set, setupRecord fails:" << getUrl();
        return false;
    }
    return listener->setupRecord(*this, start, custom_path, max_second);
}

bool Resource::isRecording(){
    auto listener = _listener.lock();
    if(!listener){
        return false;
    }
    return listener->isRecording(*this);
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
    GET_CONFIG(int, maxWaitMS, xGeneral::kMaxResourceWaitTimeMS);
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
        NoticeCenter::Instance().delListener(listener_tag, xBroadcast::kBroadcastResourceChanged);
        cb_once(nullptr);
        return 0;
    });

    auto cancel_all = [on_timeout, listener_tag]() {
        // Cancel the delayed task to prevent multiple callbacks
        on_timeout->cancel();
        // Cancel the media registration event listener
        NoticeCenter::Instance().delListener(listener_tag, xBroadcast::kBroadcastResourceChanged);
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
    NoticeCenter::Instance().addListener(listener_tag, xBroadcast::kBroadcastResourceChanged, on_register);

    function<void()> close_player = [cb_once, cancel_all, poller]() {
        poller->async([cancel_all, cb_once]() {
            cancel_all();
            // Tell the player that the resource does not exist, so it will immediately disconnect the player
            cb_once(nullptr);
        });
    };
    // Broadcast that the resource is not found, at this time you can immediately pull the resource, so it is still in time
    NOTICE_EMIT(BroadcastNotFoundResourceArgs, xBroadcast::kBroadcastNotFoundResource, info, *session, close_player);
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
    src = Resource::find(LOCAL_SCHEMA, vhost, app, id);
    if (src) {
        return src;
    }
    src = Resource::find(STORAGE_SCHEMA, vhost, app, id);
    if (src) {
        return src;
    }
    src = Resource::find(SERVER_SCHEMA, vhost, app, id);
    if (src) {
        return src;
    }
    return Resource::find(USER_SCHEMA, vhost, app, id);
}

void Resource::emitEvent(bool regist) {
    auto listener = _listener.lock();
    if (listener) {
        // Trigger callback
        listener->onRegist(*this, regist);
    }
    // Trigger broadcast
    NOTICE_EMIT(BroadcastResourceChangedArgs, xBroadcast::kBroadcastResourceChanged, regist, *this);
    InfoL << (regist ? "Resource Registration:" : "Resource Cancellation:") << getUrl();
}

void Resource::regist() {
    {
        // Reduce mutex lock critical area
        lock_guard<recursive_mutex> lock(s_resource_mtx);
        auto &ref = s_resource_map[_schema][_tuple.vhost][_tuple.app][_tuple.id];
        auto src = ref.lock();
        if (src) {
            if (src.get() == this) {
                return;
            }
            // Add judgment to prevent re-registration when the current stream is already registered
            throw std::invalid_argument("resource already existed:" + getUrl());
        }
        ref = shared_from_this();
    }
    emitEvent(true);
}

template<typename MAP, typename First, typename ...KeyTypes>
static bool erase_resource(bool &hit, const Resource *thiz, MAP &map, const First &first, const KeyTypes &...keys) {
    auto it = map.find(first);
    if (it != map.end() && erase_resource(hit, thiz, it->second, keys...)) {
        map.erase(it);
    }
    return map.empty();
}

template<typename MAP, typename First>
static bool erase_resource(bool &hit, const Resource *thiz, MAP &map, const First &first) {
    auto it = map.find(first);
    if (it != map.end()) {
        auto src = it->second.lock();
        if (!src || src.get() == thiz) {
            // If the object has been destroyed or the object is itself, then remove it
            map.erase(it);
            hit = true;
        }
    }
    return map.empty();
}

// Unregister the source
bool Resource::unregist() {
    bool ret = false;
    {
        // Reduce mutex lock critical area
        lock_guard<recursive_mutex> lock(s_resource_mtx);
        erase_resource(ret, this, s_resource_map, _schema, _tuple.vhost, _tuple.app, _tuple.id);
    }

    if (ret) {
        emitEvent(false);
    }
    return ret;
}

bool equalResourceTuple(const ResourceTuple& a, const ResourceTuple& b) {
    return a.vhost == b.vhost && a.app == b.app && a.id == b.id;
}
/////////////////////////////////////ResourceInfo//////////////////////////////////////

void ResourceInfo::parse(const std::string &url_in) {
    full_url = url_in;
    auto url = url_in;
    auto pos = url.find("?");
    if (pos != string::npos) {
        params = url.substr(pos + 1);
        url.erase(pos);
    }

    auto schema_pos = url.find("://");
    if (schema_pos != string::npos) {
        schema = url.substr(0, schema_pos);
    } else {
        schema_pos = -3;
    }
    auto split_vec = split(url.substr(schema_pos + 3), "/");
    if (split_vec.size() > 0) {
        splitUrl(split_vec[0], host, port);
        vhost = host;
        if (vhost == "localhost" || isIP(vhost.data())) {
            // If the access is to localhost or ip, then it is the default virtual host
            vhost = DEFAULT_VHOST;
        }
    }
    if (split_vec.size() > 1) {
        app = split_vec[1];
    }
    if (split_vec.size() > 2) {
        string resource_id;
        for (size_t i = 2; i < split_vec.size(); ++i) {
            resource_id.append(split_vec[i] + "/");
        }
        if (resource_id.back() == '/') {
            resource_id.pop_back();
        }
        id = resource_id;
    }

    auto kv = Parser::parseArgs(params);
    auto it = kv.find(VHOST_KEY);
    if (it != kv.end()) {
        vhost = it->second;
    }

    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost || vhost.empty()) {
        // If the virtual host is closed or the virtual host is empty, set the virtual host to the default
        vhost = DEFAULT_VHOST;
    }
}

/////////////////////////////////////ResourceEvent//////////////////////////////////////

void ResourceEvent::onReaderChanged(Resource &sender, int size) {
    GET_CONFIG(bool, enable, xGeneral::kBroadcastResourceCountChanged);
    if (enable) {
        NOTICE_EMIT(BroadcastResourceCountChangedArgs, xBroadcast::kBroadcastResourceCountChanged, sender.getResourceTuple(), sender.totalReaderCount());
    }
    if (size || sender.totalReaderCount()) {
        // Someone is still watching this resource, do not trigger the close event
        _async_close_timer = nullptr;
        return;
    }
    // No one is watching this resource, indicating that the source can be closed.
    GET_CONFIG(int, resource_none_reader_delay, xGeneral::kResourceNoneReaderDelayMS);
    weak_ptr<Resource> weak_sender = sender.shared_from_this();

    _async_close_timer = std::make_shared<Timer>(resource_none_reader_delay / 1000.0f, [weak_sender]() {
        auto strong_sender = weak_sender.lock();
        if (!strong_sender) {
            // The object has been destroyed.
            return false;
        }

        if (strong_sender->totalReaderCount()) {
            // Someone is still watching this video, so the close event is not triggered.
            return false;
        }

        auto muxer = strong_sender->getMuxer();
        if (muxer /*&& muxer->getOption().auto_close*/) {
            // This stream is marked as an automatically closed stream with no viewers.
            WarnL << "Auto cloe stream when none reader: " << strong_sender->getUrl();
            strong_sender->close(false);
        } else {
            // When live streaming, trigger the no-viewer event, allowing developers to choose whether to close it.
            NOTICE_EMIT(BroadcastResourceNoneReaderArgs, xBroadcast::kBroadcastResourceNoneReader, *strong_sender);
        }
        return false;
    }, nullptr);
}

string ResourceEvent::getOriginUrl(Resource &sender) const {
    return sender.getUrl();
}

ResourceOriginType ResourceEventInterceptor::getOriginType(Resource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::getOriginType(sender);
    }
    return listener->getOriginType(sender);
}

string ResourceEventInterceptor::getOriginUrl(Resource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::getOriginUrl(sender);
    }
    auto ret = listener->getOriginUrl(sender);
    if (!ret.empty()) {
        return ret;
    }
    return ResourceEvent::getOriginUrl(sender);
}

std::shared_ptr<SockInfo> ResourceEventInterceptor::getOriginSock(Resource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::getOriginSock(sender);
    }
    return listener->getOriginSock(sender);
}

bool ResourceEventInterceptor::connect(Resource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::connect(sender);
    }
    return listener->connect(sender);
}

bool ResourceEventInterceptor::disconnect(Resource &sender, bool force) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::disconnect(sender, force);
    }
    return listener->disconnect(sender, force);
}

bool ResourceEventInterceptor::pause(Resource &sender, bool pause) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::pause(sender, pause);
    }
    return listener->pause(sender, pause);
}

bool ResourceEventInterceptor::close(Resource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::close(sender);
    }
    return listener->close(sender);
}

int ResourceEventInterceptor::totalReaderCount(Resource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::totalReaderCount(sender);
    }
    return listener->totalReaderCount(sender);
}

void ResourceEventInterceptor::onReaderChanged(Resource &sender, int size) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::onReaderChanged(sender, size);
    }
    listener->onReaderChanged(sender, size);
}

void ResourceEventInterceptor::onRegist(Resource &sender, bool regist) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::onRegist(sender, regist);
    }
    listener->onRegist(sender, regist);
}

toolkit::EventPoller::Ptr ResourceEventInterceptor::getOwnerPoller(Resource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::getOwnerPoller(sender);
    }
    return listener->getOwnerPoller(sender);
}

std::shared_ptr<MultiResourceMuxer> ResourceEventInterceptor::getMuxer(Resource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::getMuxer(sender);
    }
    return listener->getMuxer(sender);
}

bool ResourceEventInterceptor::setupRecord(Resource &sender, bool start, const std::string &custom_path, size_t max_second) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::setupRecord(sender, start, custom_path, max_second);
    }
    return listener->setupRecord(sender, start, custom_path, max_second);
}

bool ResourceEventInterceptor::isRecording(Resource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return ResourceEvent::isRecording(sender);
    }
    return listener->isRecording(sender);
}

void ResourceEventInterceptor::setDelegate(const std::weak_ptr<ResourceEvent> &listener) {
    if (listener.lock().get() == this) {
        throw std::invalid_argument("can not set self as a delegate");
    }
    _listener = listener;
}

std::shared_ptr<ResourceEvent> ResourceEventInterceptor::getDelegate() const {
    return _listener.lock();
}

} // namespace managerkit