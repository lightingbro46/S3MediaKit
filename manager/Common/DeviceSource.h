#ifndef COMMON_DEVICESOURCE_H
#define COMMON_DEVICESOURCE_H

#include <memory>
#include <string>
#include "Util/util.h"
#include "Util/TimeTicker.h"
#include "Poller/Timer.h"

#define CAMERA_SCHEMA "camera"

namespace managerkit {

struct DeviceTuple {
    std::string vhost;
    std::string device_id;
    std::string name;
    std::string shortUrl() const { return vhost + "/" + device_id; }
};

bool equalDeviceTuple(const DeviceTuple &a, const DeviceTuple &b);

enum class DeviceOriginType : uint8_t {
    unknown = 0,
    generic_rtsp_camera,
    onvif_camera,
};

std::string getOriginTypeString(DeviceOriginType type);

class DeviceSource;
class DeviceSourceEvent {
public:
    friend class DeviceSource;
    class NotImplemented : public std::runtime_error {
    public:
        template<typename ...T>
        NotImplemented(T && ...args) : std::runtime_error(std::forward<T>(args)...) {}
    };

    virtual ~DeviceSourceEvent() = default;
    // Get device source type
    virtual DeviceOriginType getOriginType(DeviceSource &sender) const { return DeviceOriginType::unknown; }
    // Get device source url or file path
    virtual std::string getOriginUrl(DeviceSource &sender) const;
    // Device registration or deregistration event
    virtual void onRegist(DeviceSource &sender, bool regist) {}
    // Get the current thread, this function is generally forced to overload
    virtual toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) { throw NotImplemented(toolkit::demangle(typeid(*this).name()) + "::getOwnerPoller not implemented"); }
};

// This object is used to intercept interesting DeviceSourceEvent events
class DeviceSourceEventInterceptor : public DeviceSourceEvent {
public:
    void setDelegate(const std::weak_ptr<DeviceSourceEvent> &listener);
    std::shared_ptr<DeviceSourceEvent> getDelegate() const;

    DeviceOriginType getOriginType(DeviceSource &sender) const override;
    std::string getOriginUrl(DeviceSource &sender) const override;

    void onRegist(DeviceSource &sender, bool regist) override;
    toolkit::EventPoller::Ptr getOwnerPoller(DeviceSource &sender) override;

private:
    std::weak_ptr<DeviceSourceEvent> _listener;
};

/**
 * Device source, any generic rtsp camera, onvif camera originates from this object
 */
class DeviceSource : public std::enable_shared_from_this<DeviceSource> {
public:
    using Ptr = std::shared_ptr<DeviceSource>;
    DeviceSource(const std::string &schema, const DeviceTuple &tuple);
    virtual ~DeviceSource();

    //////////////Get MediaSource information////////////////

    // Get protocol type
    const std::string& getSchema() const { return _schema; }
    // get camera tuple
    const DeviceTuple &getDeviceTuple() const { return _tuple; };

    std::string getUrl() const { return _schema + "://" + _tuple.shortUrl(); }

    // used to control device in http session
    std::shared_ptr<void> getOwnership();

    // Get the stream creation GMT unix timestamp, unit seconds
    uint64_t getCreateStamp() const { return _create_stamp; }
    // Get the stream online time, unit seconds
    uint64_t getAliveSecond() const;

    // //////////////MediaSourceEvent related interface implementation////////////////

    // Set listener
    virtual void setListener(const std::weak_ptr<DeviceSourceEvent> &listener);
    // Get listener
    std::weak_ptr<DeviceSourceEvent> getListener() const;

    // Get the device source type
    DeviceOriginType getOriginType() const;
    // Get the device source url or file path
    std::string getOriginUrl() const;

    // Get the thread where it is running
    toolkit::EventPoller::Ptr getOwnerPoller();

    //////////////static methods, find or generate DeviceSource////////////////

    // Synchronously find the camera by id
    static Ptr find(const std::string &schema, const std::string &vhost, const std::string &device_id);
    // Ignore schema, synchronously find the device, may return generic camera/onvif camera type
    static Ptr find(const std::string &vhost, const std::string &device_id);
    
    // Traverse all device
    static void for_each_device(const std::function<void(const Ptr &src)> &cb, const std::string schema = "", const std::string vhost = "", const std::string device_id = "");

protected:
    // Device registration
    void regist();

private:
    // Device unregistration
    bool unregist();

protected:
    DeviceTuple _tuple;
    // Trigger device events
    void emitEvent(bool regist);

private:
    std::atomic_flag _owned { false };
    time_t _create_stamp;
    toolkit::Ticker _ticker;
    std::string _schema;
    std::weak_ptr<DeviceSourceEvent> _listener;
    // Object count statistics
    toolkit::ObjectStatistic<DeviceSource> _statistic;
};

} // namespace managerkit 

#endif // COMMON_DEVICESOURCE_H