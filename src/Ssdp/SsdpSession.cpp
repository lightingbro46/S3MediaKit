#include "SsdpSession.h"
#include "Common/config.h"
#include "../manager/Server/GlobalMonitor.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

static string ssdp_search_header = "M-SEARCH * HTTP/1.1";
static string ssdp_search_man = "ssdp:discover";
static string service_taget = "urn:schemas-upnp-org:device:MediaServer:1";
static string service_taget_all = "ssdp:all";

SsdpSession::SsdpSession(const Socket::Ptr &sock) : Session(sock) {
    socklen_t addr_len = sizeof(_peer_addr);
    memset(&_peer_addr, 0, addr_len);
    // TraceL<<"before addr len "<<addr_len;
    getpeername(sock->rawFD(), (struct sockaddr *)&_peer_addr, &addr_len);
    // TraceL<<"after addr len "<<addr_len<<" family "<<_peer_addr.ss_family;
}

void SsdpSession::attachServer(const Server &server) {
    SockUtil::setRecvBuf(getSock()->rawFD(), 1024 * 1024);
}

void SsdpSession::onRecv(const Buffer::Ptr &buffer) {
    string data(buffer->data(), buffer->size());
    TraceL << "Received UDP data from " << SockUtil::inet_ntoa((struct sockaddr *)&_peer_addr) << ":" << SockUtil::inet_port((struct sockaddr *)&_peer_addr) << "\n" << data;
    _ticker.resetTime();
    if (data.find(ssdp_search_header) != string::npos && data.find(ssdp_search_man) != string::npos &&
        (data.find(service_taget_all) != string::npos || data.find(service_taget) != string::npos)) {
        /**
         * @brief SSDP message incomming
         * M-SEARCH * HTTP/1.1
         * HOST: 239.255.255.250:1900
         * MAN: "ssdp:discover"
         * MX: 2
         * ST: urn:schemas-upnp-org:device:MediaServer:1
         */
        DebugL << "M-SEARCH request detected!";
        sendResponse();
    } else {
        // WarnL<< "ingore  data";
    }
}

void SsdpSession::onError(const SockException &err) {
    WarnP(this) << err;
}

void SsdpSession::onManager() {
    GET_CONFIG(float, timeoutSec, Ssdp::kTimeOutSec);
    if (_ticker.elapsedTime() > timeoutSec * 1000) {
        shutdown(SockException(Err_timeout, "ssdp connection timeout"));
        return;
    }
}

static string getSystemPlatform() {
    auto osinfo = managerkit::GlobalMonitor::Instance().getOsInfo(); 
    return StrPrinter << osinfo.variant << "/" << osinfo.variant_version;
}

string getSystemLocation(const string &if_ip) {
    ostringstream ss;
    ss << "http://";
    ss << if_ip;
    GET_CONFIG(uint16_t, httpPort, "http.port")
    if (httpPort > 0 || httpPort != 80) {
        ss << ":" << httpPort;
    }
    ss << "/media/mserver/description";
    return ss.str();
}

void SsdpSession::sendResponse() {
    string date = getTimeStr("%Y-%m-%d %H:%M:%S");
    string platform = getSystemPlatform();

    auto peer_ip = SockUtil::inet_ntoa((struct sockaddr *)&_peer_addr);
    auto peer_port = SockUtil::inet_port((struct sockaddr *)&_peer_addr);
    auto local_ip = SockUtil::get_ifr_ip(SockUtil::get_ifr_name(peer_ip.c_str()).c_str());
    string location = getSystemLocation(local_ip);

    GET_CONFIG(string, mediaServerId, General::kMediaServerId)

    ostringstream ss;
        ss << "HTTP/1.1 200 OK\r\n";
        ss << "CACHE-CONTROL: max-age=1800\r\n";
        ss << "DATE: " << date << "\r\n";
        ss << "EXT:\r\n";
        ss << "LOCATION: "<< location << "\r\n";
        ss << "SERVER: "<< platform << " UPnP/1.0 3SPro/3.0\r\n";
        ss << "ST: "<< service_taget << "\r\n";
        ss << "USN: uuid:" << mediaServerId << "::" << service_taget << "\r\n";
        ss << "\r\n";

    auto msg = ss.str();
    SockSender::send(msg);
    InfoL << "Sent SSDP response to " << peer_ip << ":" << peer_port;
}

} // namespace mediakit
