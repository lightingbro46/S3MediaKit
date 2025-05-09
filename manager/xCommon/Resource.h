#ifndef MANAGER_RESOURCE_H
#define MANAGER_RESOURCE_H

#include <string>
#include <atomic>
#include <memory>
#include <functional>
#include "Util/mini.h"
#include "Network/Socket.h"

#define USER_SCHEMA "user"
#define SERVER_SCHEMA "server"
#define STORAGE_SCHEMA "storage"
#define LOCAL_SCHEMA "local"
#define CAMERA_SCHEMA "camera" 

namespace toolkit {
class Session;
}

namespace managerkit {

struct ResourceTuple {
    std::string vhost;
    std::string app;
    std::string id;
    std::string parent_id;
    std::string xtype_id;
    std::string name;
    std::string params;
    std::string shortUrl() const {
        return vhost + "/" + app + "/" + id;
    }
};

enum class ResourceOriginType : uint8_t {
    unknown = 0,
    user,
    server,
    file_storage,
    camera,
    virtual_camera
};

std::string getOriginTypeString(ResourceOriginType type);

class Resource;
class MultiResourceMuxer;

class ResourceEvent {
public:
    friend class Resource;

    class NotImplemented: public std::runtime_error {
    public:
        template<typename ...T>
        NotImplemented(T && ...args) : std::runtime_error(std::forward<T>(args)...) {}
    };

    virtual ~ResourceEvent() = default;

    // Get resource type
    virtual ResourceOriginType getOriginType(Resource &sender) const { return ResourceOriginType::unknown; }
    // Get resource url or file path
    virtual std::string getOriginUrl(Resource &sender) const;
    // Get resource client related information
    virtual std::shared_ptr<toolkit::SockInfo> getOriginSock(Resource &sender) const {return  nullptr; }

    // Notify connect
    virtual bool connect(Resource &sender) { return false; }
    // Notify disconnect
    virtual bool disconnect(Resource &sender, bool force) { return false; }
    // Notify pause or resume
    virtual bool pause(Resource &sender, bool pause) { return false; }
    // Notify it to stop generating streams
    virtual bool close(Resource &sender) { return false; }
    // Get the total number of viewers, this function is generally forced to overload
    virtual int totalReaderCount(Resource &sender) { throw NotImplemented(toolkit::demangle(typeid(*this).name()) + "::totalReaderCount not implemented"); }
    // Notify the change in the number of viewers
    virtual void onReaderChanged(Resource &sender, int size);
    // Stream registration or deregistration event
    virtual void onRegist(Resource &sender, bool regist) {}
    // Get the current thread, this function is generally forced to overload
    virtual toolkit::EventPoller::Ptr getOwnerPoller(Resource &sender) { throw NotImplemented(toolkit::demangle(typeid(*this).name()) + "::getOwnerPoller not implemented"); }

    //////////////////////Only for MultiResourceMuxer object inheritance////////////////////////
    // Start or stop recording
    virtual bool setupRecord(Resource &sender, bool start, const std::string &custom_path, size_t max_second) { return false; };
    // Get recording status
    virtual bool isRecording(Resource &sender) { return false; }
    // Get MultiResourceMuxer object
    virtual std::shared_ptr<MultiResourceMuxer> getMuxer(Resource &sender) const { return nullptr; }

private:
    toolkit::Timer::Ptr _async_close_timer;
};

template <typename MAP, typename KEY, typename TYPE>
static void getArgsValue(const MAP &allArgs, const KEY &key, TYPE &value) {
    auto val = ((MAP &)allArgs)[key];
    if (!val.empty()) {
        value = (TYPE)val;
    }
}

template <typename KEY, typename TYPE>
static void getArgsValue(const toolkit::mINI &allArgs, const KEY &key, TYPE &value) {
    auto it = allArgs.find(key);
    if (it != allArgs.end()) {
        value = (TYPE)it->second;
    }
}

class ResourceOption {
public:
    ResourceOption();
};

// This object is used to intercept interesting ResourceEvent events
class ResourceEventInterceptor : public ResourceEvent {
public:
    void setDelegate(const std::weak_ptr<ResourceEvent> &listener);
    std::shared_ptr<ResourceEvent> getDelegate() const;

    ResourceOriginType getOriginType(Resource &sender) const override;
    std::string getOriginUrl(Resource &sender) const override;
    std::shared_ptr<toolkit::SockInfo> getOriginSock(Resource &sender) const override;

    bool connect(Resource &sender) override;
    bool disconnect(Resource &sender, bool force) override;
    bool pause(Resource &sender, bool pause) override;
    bool close(Resource &sender) override;
    int totalReaderCount(Resource &sender) override;
    void onReaderChanged(Resource &sender, int size) override;
    void onRegist(Resource &sender, bool regist) override;
    bool setupRecord(Resource &sender, bool start, const std::string &custom_path, size_t max_second) override;
    bool isRecording(Resource &sender) override;
    toolkit::EventPoller::Ptr getOwnerPoller(Resource &sender) override;
    std::shared_ptr<MultiResourceMuxer> getMuxer(Resource &sender) const override;

private:
    std::weak_ptr<ResourceEvent> _listener;
};

/**
 * Parse the url to get resource information
 */
class ResourceInfo: public ResourceTuple {
public:
    ResourceInfo() = default;
    ResourceInfo(const std::string &url) { parse(url);  }
    void parse(const std::string &url);
    std::string getUrl() const { return schema + "://" + shortUrl(); }

public:
    uint16_t port = 0;
    std::string protocol;
    std::string full_url;
    std::string schema;
    std::string host;
};

bool equalResourceTuple(const ResourceTuple &a, const ResourceTuple &b);

/**
 * Resource, any user/server/storage/local/camera live stream originates from this object
 */
class Resource : public std::enable_shared_from_this<Resource> {
public:
    static Resource& NullResource();
    using Ptr = std::shared_ptr<Resource>;

    Resource(const std::string &schema, const ResourceTuple &tuple);
    virtual ~Resource() = default;

    //////////////Get Resource information////////////////

    // Get schema type
    const std::string& getSchema() const {
        return _schema;
    }

    const ResourceTuple &getResourceTuple() const { 
        return _tuple; 
    }

    std::string getUrl() const { return _schema + "://" + _tuple.shortUrl(); }

    // Get object ownership
    std::shared_ptr<void> getOwnership();

    // Get the stream creation GMT unix timestamp, unit seconds
    uint64_t getCreateStamp() const { return _create_stamp; }
    // Get the stream online time, unit seconds
    uint64_t getAliveSecond() const;

    //////////////ResourceEvent related interface implementation////////////////
    
    // Set listener
    virtual void setListener(const std::weak_ptr<ResourceEvent> &listener);
    // Get listener
    std::weak_ptr<ResourceEvent> getListener() const;

    // This protocol gets the number of viewers, it may return the number of viewers of this protocol, or it may return the total number of viewers
    virtual int readerCount() = 0;
    // Number of viewers
    virtual int totalReaderCount();
    // Get the player list
    virtual void getPlayerList(const std::function<void(const std::list<toolkit::Any> &info_list)> &cb,
                               const std::function<toolkit::Any(toolkit::Any &&info)> &on_change) {
        assert(cb);
        cb(std::list<toolkit::Any>());
    }

    virtual bool broadcastMessage(const toolkit::Any &data) { return false; }

    // Get the resource type
    ResourceOriginType getOriginType() const;
    // Get the resource url or file path
    std::string getOriginUrl() const;
    // Get the resource client information
    std::shared_ptr<toolkit::SockInfo> getOriginSock() const;

    // Connect
    bool connect();
    // Disconnect
    bool disconnect(bool force);
    // Pause
    bool pause(bool pause);
    // Close the stream
    bool close(bool force);
    // The number of viewers of this stream changes
    void onReaderChanged(int size);
    // Turn recording on or off
    bool setupRecord(bool start, const std::string &custom_path, size_t max_second);
    // Get recording status
    bool isRecording();
    // Get the thread where it is running
    toolkit::EventPoller::Ptr getOwnerPoller();
    // Get the MultiResourceMuxer object
    std::shared_ptr<MultiResourceMuxer> getMuxer() const;

    //////////////static methods, find or generate Resource////////////////

    // Synchronously find the stream
    static Ptr find(const std::string &schema, const std::string &vhost, const std::string &app, const std::string &id);
    static Ptr find(const ResourceInfo &info) {
        return find(info.schema, info.vhost, info.app, info.id);
    }

    // Ignore schema, synchronously find the stream, may return rtmp/rtsp/hls type
    static Ptr find(const std::string &vhost, const std::string &app, const std::string &id);

    // Asynchronously find the stream
    static void findAsync(const ResourceInfo &info, const std::shared_ptr<toolkit::Session> &session, const std::function<void(const Ptr &src)> &cb);
    // Traverse all streams
    static void for_each_resource(const std::function<void(const Ptr &src)> &cb, const std::string &schema = "", const std::string &vhost = "", const std::string &app = "", const std::string &id = "");

protected:
    // Resource registration
    void regist();

private:
    // Resource unregistration
    bool unregist();
    // Trigger resource events
    void emitEvent(bool regist);

protected:
    ResourceTuple _tuple;

private:
    std::atomic_flag _owned { false };
    time_t _create_stamp;
    toolkit::Ticker _ticker;
    std::string _schema;
    std::weak_ptr<ResourceEvent> _listener;
    // Object count statistics
    toolkit::ObjectStatistic<Resource> _statistic;
};

std::string generateGuid();

} // namespace managerkit

#endif //MANAGER_RESOURCE_H 