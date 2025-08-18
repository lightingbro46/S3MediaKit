#include <mutex>
#include "DeviceSource.h"
#include "Common/config.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace toolkit {
    StatisticImp(managerkit::DeviceSource);
}

namespace managerkit {

static recursive_mutex s_device_source_mtx;
using DeviceMap = unordered_map<string/*device_id*/, weak_ptr<DeviceSource>>;
using VhostDeviceMap = unordered_map<string/*vhost*/, DeviceMap>;
using SchemaVhostDeviceMap = unordered_map<string/*schema*/, VhostDeviceMap>;
static SchemaVhostDeviceMap s_device_source_map;

string getOriginTypeString(DeviceOriginType type){
#define SWITCH_CASE(type) case DeviceOriginType::type : return #type
    switch (type) {
        SWITCH_CASE(unknown);
        SWITCH_CASE(generic_rtsp_camera);
        SWITCH_CASE(onvif_camera);
        default : return "unknown";
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

DeviceSource::DeviceSource(const string &schema, const DeviceTuple &tuple) : _tuple(tuple) {
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if (!enableVhost || _tuple.vhost.empty()) {
        _tuple.vhost = DEFAULT_VHOST;
    }
    _schema = schema;
    _create_stamp = time(NULL);
}

DeviceSource::~DeviceSource() {
    try {
        unregist();
    } catch (std::exception &ex) {
        WarnL << "Exception occurred: " << ex.what();
    }
}

std::shared_ptr<void> DeviceSource::getOwnership() {
    if (_owned.test_and_set()) {
        // Already owned by all
        return nullptr;
    }
    weak_ptr<DeviceSource> weak_self = shared_from_this();
    // Ensure that the returned Ownership smart pointer is not empty, 0x01 has no practical meaning
    return std::shared_ptr<void>((void *) 0x01, [weak_self](void *ptr) {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->_owned.clear();
        }
    });
}

uint64_t DeviceSource::getAliveSecond() const {
    // The purpose of using the Ticker object to obtain the survival time is to prevent the modification of the system time from causing a rollback
    return _ticker.createdTime() / 1000;
}

void DeviceSource::setListener(const std::weak_ptr<DeviceSourceEvent> &listener){
    _listener = listener;
}

std::weak_ptr<DeviceSourceEvent> DeviceSource::getListener() const {
    return _listener;
}

DeviceOriginType DeviceSource::getOriginType() const {
    auto listener = _listener.lock();
    if (!listener) {
        return DeviceOriginType::unknown;
    }
    return listener->getOriginType(const_cast<DeviceSource &>(*this));
}

string DeviceSource::getOriginUrl() const {
    auto listener = _listener.lock();
    if (!listener) {
        return getUrl();
    }
    auto ret = listener->getOriginUrl(const_cast<DeviceSource &>(*this));
    if (!ret.empty()) {
        return ret;
    }
    return getUrl();
}

toolkit::EventPoller::Ptr DeviceSource::getOwnerPoller() {
    toolkit::EventPoller::Ptr ret;
    auto listener = _listener.lock();
    if (listener) {
        return listener->getOwnerPoller(*this);
    }
    throw std::runtime_error(toolkit::demangle(typeid(*this).name()) + "::getOwnerPoller failed: " + getUrl());
}

template<typename MAP, typename LIST, typename First, typename ...KeyTypes>
static void for_each_device_l(const MAP &map, LIST &list, const First &first, const KeyTypes &...keys) {
    if (first.empty()) {
        for (auto &pr : map) {
            for_each_device_l(pr.second, list, keys...);
        }
        return;
    }
    auto it = map.find(first);
    if (it != map.end()) {
        for_each_device_l(it->second, list, keys...);
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
static void for_each_device_l(const MAP &map, LIST &list, const First &first) {
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

void DeviceSource::for_each_device(const function<void(const Ptr &src)> &cb, const string schema, const string vhost, const string device_id) {
    lock_guard<recursive_mutex> lock(s_device_source_mtx);
    deque<Ptr> src_list;
    {
        lock_guard<recursive_mutex> lock(s_device_source_mtx);
        for_each_device_l(s_device_source_map, src_list, schema, vhost, device_id);
    }
    for (auto &src : src_list) {
        cb(src);
    }
}

static DeviceSource::Ptr find_l(const string &schema, const string &vhost_in, const string &device_id) {
    string vhost = vhost_in;
    GET_CONFIG(bool, enableVhost, General::kEnableVhost);
    if(vhost.empty() || !enableVhost){
        vhost = DEFAULT_VHOST;
    }

    if (device_id.empty()) {
        // If no device id are specified, then it is traversal instead of searching, so it should return search failure
        return nullptr;
    }

    DeviceSource::Ptr ret;
    DeviceSource::for_each_device([&](const DeviceSource::Ptr &src) { ret = std::move(const_cast<DeviceSource::Ptr &>(src)); }, schema, vhost, device_id);

    return ret;
}

DeviceSource::Ptr DeviceSource::find(const std::string &schema, const std::string &vhost, const std::string &device_id) {
    return find_l(schema, vhost, device_id);
}

DeviceSource::Ptr DeviceSource::find(const std::string &vhost, const std::string &device_id) {
    lock_guard<recursive_mutex> lock(s_device_source_mtx);
    // todo: add more device type    
    return  DeviceSource::find(CAMERA_SCHEMA, vhost, device_id);
}

void DeviceSource::emitEvent(bool regist){
    auto listener = _listener.lock();
    if (listener) {
        // Trigger callback
        listener->onRegist(*this, regist);
    }
    // Trigger broadcast
    NOTICE_EMIT(BroadcastDeviceChangedArgs, Broadcast::kBroadcastDeviceChanged, regist, *this);
    InfoL << (regist ? "Device Registration:" : "Device Cancellation:") << getUrl();
}

void DeviceSource::regist() {
    {
        // Reduce mutex lock critical area
        lock_guard<recursive_mutex> lock(s_device_source_mtx);
        auto &ref = s_device_source_map[_schema][_tuple.vhost][_tuple.device_id];
        auto src = ref.lock();
        if (src) {
            if (src.get() == this) {
                return;
            }
            // Add judgment to prevent re-registration when the current device is already registered
            throw std::invalid_argument("device source already existed:" + _tuple.device_id);
        }
        ref = shared_from_this();
    }
    emitEvent(true);
}

template<typename MAP, typename First, typename ...KeyTypes>
static bool erase_device_source(bool &hit, const DeviceSource *thiz, MAP &map, const First &first, const KeyTypes &...keys) {
    auto it = map.find(first);
    if (it != map.end() && erase_device_source(hit, thiz, it->second, keys...)) {
        map.erase(it);
    }
    return map.empty();
}

template<typename MAP, typename First>
static bool erase_device_source(bool &hit, const DeviceSource *thiz, MAP &map, const First &first) {
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
bool DeviceSource::unregist() {
    bool ret = false;
    {
        // Reduce mutex lock critical area
        lock_guard<recursive_mutex> lock(s_device_source_mtx);
        erase_device_source(ret, this, s_device_source_map, _schema, _tuple.vhost, _tuple.device_id);
    }
    if (ret) {
        emitEvent(false);
    }
    return ret;
}

bool equalDeviceTuple(const DeviceTuple &a, const DeviceTuple &b) {
    return a.vhost == b.vhost && a.device_id == b.device_id;
}

/////////////////////////////////////DeviceSourceEvent//////////////////////////////////////

string DeviceSourceEvent::getOriginUrl(DeviceSource &sender) const {
    return sender.getUrl();
}

DeviceOriginType DeviceSourceEventInterceptor::getOriginType(DeviceSource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return DeviceSourceEvent::getOriginType(sender);
    }
    return listener->getOriginType(sender);
}

string DeviceSourceEventInterceptor::getOriginUrl(DeviceSource &sender) const {
    auto listener = _listener.lock();
    if (!listener) {
        return DeviceSourceEvent::getOriginUrl(sender);
    }
    auto ret = listener->getOriginUrl(sender);
    if (!ret.empty()) {
        return ret;
    }
    return DeviceSourceEvent::getOriginUrl(sender);
}

void DeviceSourceEventInterceptor::onRegist(DeviceSource &sender, bool regist) {
    auto listener = _listener.lock();
    if (!listener) {
        return DeviceSourceEvent::onRegist(sender, regist);
    }
    listener->onRegist(sender, regist);
}

toolkit::EventPoller::Ptr DeviceSourceEventInterceptor::getOwnerPoller(DeviceSource &sender) {
    auto listener = _listener.lock();
    if (!listener) {
        return DeviceSourceEvent::getOwnerPoller(sender);
    }
    return listener->getOwnerPoller(sender);
}


void DeviceSourceEventInterceptor::setDelegate(const std::weak_ptr<DeviceSourceEvent> &listener) {
    if (listener.lock().get() == this) {
        throw std::invalid_argument("can not set self as a delegate");
    }
    _listener = listener;
}

std::shared_ptr<DeviceSourceEvent> DeviceSourceEventInterceptor::getDelegate() const {
    return _listener.lock();
}

} // namespace managerkit
