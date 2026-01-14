#include "StrUtil.h"
#include "Common/Parser.h"
#include "Common/strCoding.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

string UriUtils::replaceIp(const string &in_url, const string &nat_ip) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    string _middle_url;
    auto it = middle_url.rfind(":");
    if (it != string::npos) {
        _middle_url = middle_url.substr(it);
        _middle_url = nat_ip + _middle_url;
    } else {
        _middle_url = nat_ip;
    }
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

string UriUtils::replacePort(const string &in_url, int nat_port) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    auto _middle_url = middle_url;
    auto it = middle_url.rfind(":");
    if (it != string::npos) {
        _middle_url = middle_url.substr(0, it);
    }
    _middle_url += ":" + to_string(nat_port);
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

string UriUtils::replaceCredentials(const string &in_url, const string &username, const string &password) {
    auto schema = findSubString(in_url.data(), nullptr, "://");
    auto middle_url = findSubString(in_url.data(), "://", "/");
    auto _middle_url = middle_url;
    auto it = middle_url.rfind("@");
    if (it != string::npos) {
        _middle_url = middle_url.substr(it + 1);
    }
    if (!username.empty() && !password.empty()) {
        _middle_url = strCoding::UrlDecodeUserOrPass(username) + ":" + strCoding::UrlDecodeUserOrPass(password) + "@" + _middle_url;
    }
    auto path = findSubString(in_url.data() + schema.size() + middle_url.size() + 4, nullptr, nullptr);
    return (StrPrinter << schema << "://" << _middle_url << "/" << path);
}

} // namespace managerkit