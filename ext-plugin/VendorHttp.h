#ifndef S3MEDIAKIT_VENDORHTTP_H
#define S3MEDIAKIT_VENDORHTTP_H

#include <functional>
#include <string>
#include "Common/DeviceControlTypes.h"

namespace managerkit {

// Answer of one request to a vendor HTTP API.
struct VendorHttpAnswer {
    int sockErr = 0;    // toolkit::ErrCode of the request; Err_success when an answer came back
    int status = 0;     // HTTP status; 0 when no answer came back
    std::string body;
};
using OnVendorHttp = std::function<void(const VendorHttpAnswer &answer)>;

// GET `url` with the account: a 401 challenge (Digest MD5, or Basic) is answered once by sending the
// request again with the credentials. `cb` runs once, on the HTTP poller.
void vendorHttpGet(const std::string &url, const std::string &username, const std::string &password,
                   float timeout_sec, const OnVendorHttp &cb);

// Authorization header for the WWW-Authenticate `challenge`; "" when it is neither Digest (MD5) nor Basic.
// `cnonce` is used when the challenge asks for qop=auth.
std::string vendorHttpAuthorization(const std::string &challenge, const std::string &method, const std::string &uri,
                                    const std::string &username, const std::string &password,
                                    const std::string &cnonce);

// The outcome of a request as a DeviceFailure (network, credentials, other).
DeviceFailure vendorHttpFailure(int sock_err, int http_status);

} // namespace managerkit

#endif // S3MEDIAKIT_VENDORHTTP_H
