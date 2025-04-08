#ifndef S3MEDIAKIT_HTTPFILEMANAGER_H
#define S3MEDIAKIT_HTTPFILEMANAGER_H

#include "HttpBody.h"
#include "HttpCookie.h"
#include "Common/Parser.h"
#include "Network/Session.h"
#include "Util/function_traits.h"

namespace mediakit {

class HttpResponseInvokerImp{
public:
    typedef std::function<void(int code, const StrCaseMap &headerOut, const HttpBody::Ptr &body)> HttpResponseInvokerLambda0;
    typedef std::function<void(int code, const StrCaseMap &headerOut, const std::string &body)> HttpResponseInvokerLambda1;

    template<typename C>
    HttpResponseInvokerImp(const C &c):HttpResponseInvokerImp(typename toolkit::function_traits<C>::stl_function_type(c)) {}
    HttpResponseInvokerImp(const HttpResponseInvokerLambda0 &lambda);
    HttpResponseInvokerImp(const HttpResponseInvokerLambda1 &lambda);

    void operator()(int code, const StrCaseMap &headerOut, const toolkit::Buffer::Ptr &body) const;
    void operator()(int code, const StrCaseMap &headerOut, const HttpBody::Ptr &body) const;
    void operator()(int code, const StrCaseMap &headerOut, const std::string &body) const;

    void responseFile(const StrCaseMap &requestHeader,const StrCaseMap &responseHeader,const std::string &file, bool use_mmap = true, bool is_path = true) const;
    operator bool();
private:
    HttpResponseInvokerLambda0 _lambad;
};

/**
 * This object is used to control access permissions for the http static folder server.
 */
class HttpFileManager  {
public:
    typedef std::function<void(int code, const std::string &content_type, const StrCaseMap &responseHeader, const HttpBody::Ptr &body)> invoker;

    /**
     * Access files or folders
     * @param sender Event trigger
     * @param parser http request
     * @param cb Callback object
    */
    static void onAccessPath(toolkit::Session &sender, Parser &parser, const invoker &cb);

    /**
     * Get mime value
     * @param name File suffix
     * @return mime value
     */
    static const std::string &getContentType(const char *name);

    /**
     * Whether this ip is in the whitelist
     * @param ip Supports ipv4 and ipv6
     */
    static bool isIPAllowed(const std::string &ip);

private:
    HttpFileManager() = delete;
    ~HttpFileManager() = delete;
};

}


#endif //S3MEDIAKIT_HTTPFILEMANAGER_H
