#ifndef S3MEDIAKIT_SOAPUTIL_H
#define S3MEDIAKIT_SOAPUTIL_H

#include <map>
#include <memory>
#include <functional>
#include <initializer_list>
#include "Common/Parser.h"
#include "Network/Socket.h"
#include "pugixml.hpp"

struct xml_string_writer : pugi::xml_writer {
    std::string result;
    virtual void write(const void *data, size_t size) {
        result.append(static_cast<const char *>(data), size);
    }
};

template<size_t sz, typename ...ARGS>
std::string print_to_string(const char (&str_fmt)[sz], ARGS &&...args) {
    std::string ret;
    //Returning false means no longer listening to the event
    ret.resize(2 * sizeof(str_fmt));
    //The real memory size of string must be one byte larger than size(used to store \0)
    auto size = snprintf((char *) ret.data(), ret.size() + 1, str_fmt, std::forward<ARGS>(args)...);
    ret.resize(size);
    return ret;
}

class SoapObject;

class SoapErr {
public:
    SoapErr(std::string url,
            std::string action,
            toolkit::SockException ex,
            const mediakit::Parser &parser,
            std::string err = "");

    operator std::string() const;
    operator bool() const;
    bool empty() const;
    int httpCode() const;

private:
    std::string _url;
    std::string _action;
    toolkit::SockException _net_err;
    int _http_code = 200;
    std::string _http_msg;
    std::string _other_err;
};

std::ostream& operator<<(std::ostream& sout, const SoapErr &err);

class SoapUtil {
public:
    static std::string createDiscoveryString(const std::string &uuid = "");
    static std::string createUuidString();
    static std::string createSoapRequest(const std::string &body, const std::string &user_name = "", const std::string &passwd = "");

    using SoapRequestCB = std::function<void(const SoapObject &node, const SoapErr &err)>;
    static void sendSoapRequest(const std::string &url, const std::string &action, const std::string &body,
                                const SoapRequestCB &func = nullptr, float timeout_sec = 10);


    using onGetProfilesResponseTuple = std::tuple<std::string/*profile_name*/, std::string /*codec*/, int /*width*/, int /*height*/>;
    using onGetProfilesResponse = std::function<void(const SoapErr &err,
                                                     const std::vector<onGetProfilesResponseTuple> &profile)>;

    /**
     * Get profile
     * @param is_media2 whether it is media2 access method
     * @param media_url media service access address
     * @param user_name username
     * @param pwd password
     * @param cb callback, high resolution profile first
     */
    static void sendGetProfiles(bool is_media2, const std::string &media_url, const std::string &user_name,
                                const std::string &pwd, const onGetProfilesResponse &cb);

    /**
     * Get device information
     * @param device_service device_service service access address
     * @param user_name username
     * @param pwd password
     * @param cb callback
     */
    static void sendGetDeviceInformation(const std::string &device_service, const std::string &user_name,
                                         const std::string &pwd, SoapRequestCB cb);


    using onGetServicesResponseMap = std::map<std::string/*ns*/, std::string/*xaddr*/, mediakit::StrCaseCompare>;
    using onGetServicesResponse = std::function<void(const SoapErr &err, onGetServicesResponseMap &val)>;

    /**
     * Get service url address
     * @param device_service device_service service access address
     * @param ns_filter filter service namespaces
     * @param user_name username
     * @param pwd password
     * @param cb callback
     */
    static void sendGetServices(const std::string &device_service, const std::initializer_list<std::string> &ns_filter,
                                const std::string &user_name, const std::string &pwd, const onGetServicesResponse &cb);


    using onGetStreamUriResponse = std::function<void(const SoapErr &err, const std::string &uri)>;

    /**
     * Get rtsp play url
     * @param is_media2 whether it is media2 method
     * @param media_url media or media2 service access address
     * @param profile resolution scheme obtained by sendGetProfiles interface
     * @param user_name username
     * @param pwd password
     * @param cb callback
     */
    static void sendGetStreamUri(bool is_media2, const std::string &media_url, const std::string &profile,
                                 const std::string &user_name, const std::string &pwd,
                                 const onGetStreamUriResponse &cb);

    using GetStreamUriRetryInvoker = std::function<void(const std::string &user_name, const std::string &pwd)>;
    using AsyncGetStreamUriCB = std::function<void(const SoapErr &err, const GetStreamUriRetryInvoker &invoker,
                                                   int retry_count, const std::string &url)>;

    /**
     * Asynchronously obtain playback url
     * @param onvif_url url returned when device is searched
     * @param cb callback
     */
    static void asyncGetStreamUri(const std::string &onvif_url, const AsyncGetStreamUriCB &cb);

private:
    SoapUtil() = delete;
    ~SoapUtil() = delete;
};

class SoapObject {
public:
    using Ptr = std::shared_ptr<SoapObject>;

    SoapObject(const pugi::xml_node &node, const SoapObject &ref);
    SoapObject();
    operator bool () const;
    void load(const char *data, size_t len);
    SoapObject operator[](const std::string &path) const;

    template<size_t sz>
    SoapObject operator[](const char (&path)[sz]) const{
        return (*this)[std::string(path, sz - 1)];
    }

    SoapObject operator[](size_t index) const;
    std::string as_string() const;
    pugi::xml_node as_xml() const;

private:
    SoapObject(std::shared_ptr<pugi::xml_node> node);

private:
    std::shared_ptr<pugi::xml_node> _root;
};



#endif //S3MEDIAKIT_SOAPUTIL_H
