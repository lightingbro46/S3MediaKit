#include <signal.h>
#include <iostream>
#include "Util/File.h"
#include "Util/logger.h"
#include "Util/SSLBox.h"
#include "Util/onceToken.h"
#include "Util/CMD.h"
#include "Network/TcpServer.h"
#include "Network/UdpServer.h"
#include "Poller/EventPoller.h"
#include "Common/config.h"
#include "Rtsp/RtspSession.h"
#include "Rtmp/RtmpSession.h"
#include "Shell/ShellSession.h"
#include "Http/WebSocketSession.h"
#include "Rtp/RtpServer.h"
#include "WebApi.h"
#include "WebHook.h"
#include "Manager.h"

#if defined(ENABLE_WEBRTC)
#include "../webrtc/WebRtcTransport.h"
#include "../webrtc/WebRtcSession.h"
#endif

#if defined(ENABLE_SRT)
#include "../srt/SrtSession.hpp"
#include "../srt/SrtTransport.hpp"
#endif

#if defined(ENABLE_VERSION)
#include "S3MVersion.h"
#endif

#if !defined(_WIN32)
#include "System.h"
#endif//!defined(_WIN32)

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace mediakit {
// //////////HTTP configuration///////////
namespace Http {
#define HTTP_FIELD "http."
const string kPort = HTTP_FIELD"port";
const string kSSLPort = HTTP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = 80;
    mINI::Instance()[kSSLPort] = 443;
},nullptr);
}//namespace Http

// //////////SHELL configuration///////////
namespace Shell {
#define SHELL_FIELD "shell."
const string kPort = SHELL_FIELD"port";
onceToken token1([](){
    mINI::Instance()[kPort] = 9000;
},nullptr);
} //namespace Shell

// //////////RTSP server configuration///////////
namespace Rtsp {
#define RTSP_FIELD "rtsp."
const string kPort = RTSP_FIELD"port";
const string kSSLPort = RTSP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = 554;
    mINI::Instance()[kSSLPort] = 332;
},nullptr);

} //namespace Rtsp

// //////////RTMP server configuration///////////
namespace Rtmp {
#define RTMP_FIELD "rtmp."
const string kPort = RTMP_FIELD"port";
const string kSSLPort = RTMP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = 1935;
    mINI::Instance()[kSSLPort] = 19350;
},nullptr);
} //namespace RTMP

// //////////Rtp proxy related configuration///////////
namespace RtpProxy {
#define RTP_PROXY_FIELD "rtp_proxy."
const string kPort = RTP_PROXY_FIELD"port";
onceToken token1([](){
    mINI::Instance()[kPort] = 10000;
},nullptr);
} //namespace RtpProxy

}  // namespace mediakit

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser = std::make_shared<OptionParser>(nullptr);

#if !defined(_WIN32)
        (*_parser) << Option('d',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "daemon",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgNone,/*This option must be followed by a value*/
                             nullptr,/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Whether to start in Daemon mode",/*This option description */
                             nullptr);
#endif//!defined(_WIN32)

        (*_parser) << Option('l',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "level",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string(LDebug).data(),/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Log Level,LTrace~LError(0~4)",/*This option description*/
                             nullptr);

        (*_parser) << Option('m',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "max_day",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "7",/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Maximum number of days to save logs",/*This option description*/
                             nullptr);

        (*_parser) << Option('c',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "config",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             (exeDir() + "config.ini").data(),/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Configuration file path",/*This option description*/
                             nullptr);

        (*_parser) << Option('s',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "ssl",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             (exeDir() + "default.pem").data(),/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "SSL certificate file or folder, support p12/pem type",/*This option description*/
                             nullptr);

        (*_parser) << Option('t',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "threads",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string(thread::hardware_concurrency()).data(),/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Number of threads triggered by startup event",/*This option description*/
                             nullptr);

        (*_parser) << Option(0,/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "affinity",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string(1).data(),/*This option must be followed by a value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Whether to enable CPU affinity settings",/*This option description*/
                             nullptr);

#if defined(ENABLE_VERSION)
        (*_parser) << Option('v', "version", Option::ArgNone, nullptr, false, "Show version number",
                             [](const std::shared_ptr<ostream> &stream, const string &arg) -> bool {
                                 // Version information
                                 *stream << "Compilation date: " << BUILD_TIME << std::endl;
                                 *stream << "Code date: " << COMMIT_TIME << std::endl;
                                 *stream << "Current git branch: " << BRANCH_NAME << std::endl;
                                 *stream << "Current git hash value: " << COMMIT_HASH << std::endl;
                                 throw ExitException();
                             });
#endif
        (*_parser) << Option(0,/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "log-slice",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "100",/*This option must be followed by a value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Maximum number of saved log slices",/*This option description*/
                             nullptr);

        (*_parser) << Option(0,/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "log-size",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "256",/*This option must be followed by a value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Maximum capacity of a single log slice, unit MB",/*This option description*/
                             nullptr);

        (*_parser) << Option(0,/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "log-dir",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             (exeDir() + "log/").data(),/*This option must be followed by a value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Log save folder path",/*This option description*/
                             nullptr);
    }
};

// Global variable, used in WebApi to save configuration files
string g_ini_file;

// Loading the ssl certificate function object
std::function<void()> g_reload_certificates;

int start_main(int argc,char *argv[]) {
    {
        CMD_main cmd_main;
        try {
            cmd_main.operator()(argc, argv);
        } catch (ExitException &) {
            return 0;
        } catch (std::exception &ex) {
            cout << ex.what() << endl;
            return -1;
        }

        bool bDaemon = cmd_main.hasKey("daemon");
        LogLevel logLevel = (LogLevel) cmd_main["level"].as<int>();
        logLevel = MIN(MAX(logLevel, LTrace), LError);
        g_ini_file = cmd_main["config"];
        string ssl_file = cmd_main["ssl"];
        int threads = cmd_main["threads"];
        bool affinity = cmd_main["affinity"];

        // Set log
        Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel", logLevel));
#if !defined(ANDROID)
        auto fileChannel = std::make_shared<FileChannel>("FileChannel", cmd_main["log-dir"], logLevel);
        // Maximum number of days to save logs
        fileChannel->setMaxDay(cmd_main["max_day"]);
        fileChannel->setFileMaxCount(cmd_main["log-slice"]);
        fileChannel->setFileMaxSize(cmd_main["log-size"]);
        Logger::Instance().add(fileChannel);
#endif // !defined(ANDROID)

#if !defined(_WIN32)
        pid_t pid = getpid();
        bool kill_parent_if_failed = true;
        if (bDaemon) {
            // Start daemon process
            System::startDaemon(kill_parent_if_failed);
        }
        // Enable crash capture, etc.
        System::systemSetup();
#endif//!defined(_WIN32)

        // Start asynchronous log thread
        Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

        InfoL << kServerName;

        // Load configuration file, create one if it doesn't exist
        loadIniConfig(g_ini_file.data());

        auto &secret = mINI::Instance()[API::kSecret];
        if (secret == "035c73f7-bb6b-4889-a715-d9eb2d1925cc" || secret.empty()) {
            // Starting with the default secret is prohibited
            secret = makeRandStr(32, true);
            mINI::Instance().dumpFile(g_ini_file);
            WarnL << "The " << API::kSecret << " is invalid, modified it to: " << secret
                  << ", saved config file: " << g_ini_file;
        }

        if (!File::is_dir(ssl_file)) {
            // Not a folder, load certificate, certificate contains public key and private key
            g_reload_certificates = [ssl_file] () {
                SSL_Initor::Instance().loadCertificate(ssl_file.data());
            };
        } else {
            // Load all certificates under the folder
            g_reload_certificates = [ssl_file]() {
                File::scanDir(ssl_file, [](const string &path, bool isDir) {
                    if (!isDir) {
                        // The last certificate will be used as the default certificate (client ssl handshake does not specify the host)
                        SSL_Initor::Instance().loadCertificate(path.data());
                    }
                    return true;
                });
            };
        }
        g_reload_certificates();

        auto &mediaServerId = mINI::Instance()[General::kMediaServerId];
        if (mediaServerId == "your_server_id" || mediaServerId.empty()) {
            // Starting with the default media server id is prohibited
            auto hardware_uuid = getHardwareUUID();
            if (hardware_uuid == "Unavailable" || hardware_uuid.empty()) {
                mediaServerId = format_guid(strToLower(makeRandStr(32)));
            } else {
                mediaServerId = format_guid(strToLower(hardware_uuid));
            }
            mINI::Instance().dumpFile(g_ini_file);
            WarnL << "The " << General::kMediaServerId << " is invalid, modified it to: " << mediaServerId
                  << ", saved config file: " << g_ini_file;
        }

        std::string listen_ip = mINI::Instance()[General::kListenIP];
        uint16_t shellPort = mINI::Instance()[Shell::kPort];
        uint16_t rtspPort = mINI::Instance()[Rtsp::kPort];
        uint16_t rtspsPort = mINI::Instance()[Rtsp::kSSLPort];
        uint16_t rtmpPort = mINI::Instance()[Rtmp::kPort];
        uint16_t rtmpsPort = mINI::Instance()[Rtmp::kSSLPort];
        uint16_t httpPort = mINI::Instance()[Http::kPort];
        uint16_t httpsPort = mINI::Instance()[Http::kSSLPort];
        uint16_t rtpPort = mINI::Instance()[RtpProxy::kPort];

        // Set the number of poller threads and CPU affinity. This function must be called before using S3ToolKit network related objects to take effect.
        // If you need to call the getSnap and addFFmpegSource interfaces, you can turn off CPU affinity

        EventPollerPool::setPoolSize(threads);
        WorkThreadPool::setPoolSize(threads);
        EventPollerPool::enableCpuAffinity(affinity);

        // Simple telnet server, can be used for server debugging, but cannot use port 23, otherwise telnet will have inexplicable phenomena
        // Test method: telnet 127.0.0.1 9000
        auto shellSrv = std::make_shared<TcpServer>();

        // rtsp[s] server, can be used for devices such as Amazon Echo Show to access
        auto rtspSrv = std::make_shared<TcpServer>();
        auto rtspSSLSrv = std::make_shared<TcpServer>();

        // rtmp[s] server
        auto rtmpSrv = std::make_shared<TcpServer>();
        auto rtmpsSrv = std::make_shared<TcpServer>();

        // http[s] server
        auto httpSrv = std::make_shared<TcpServer>();
        auto httpsSrv = std::make_shared<TcpServer>();

#if defined(ENABLE_RTPPROXY)
        // GB28181 rtp push stream port, supports UDP/TCP
        auto rtpServer = std::make_shared<RtpServer>();
#endif//defined(ENABLE_RTPPROXY)

#if defined(ENABLE_WEBRTC)
        auto rtcSrv_tcp = std::make_shared<TcpServer>();
        // webrtc udp server
        auto rtcSrv_udp = std::make_shared<UdpServer>();
        rtcSrv_udp->setOnCreateSocket([](const EventPoller::Ptr &poller, const Buffer::Ptr &buf, struct sockaddr *, int) {
            if (!buf) {
                return Socket::createSocket(poller, false);
            }
            auto new_poller = WebRtcSession::queryPoller(buf);
            if (!new_poller) {
                // The webrtc object corresponding to this data is not found, discard it
                return Socket::Ptr();
            }
            return Socket::createSocket(new_poller, false);
        });
        uint16_t rtcPort = mINI::Instance()[Rtc::kPort];
        uint16_t rtcTcpPort = mINI::Instance()[Rtc::kTcpPort];
#endif//defined(ENABLE_WEBRTC)


#if defined(ENABLE_SRT)
        auto srtSrv = std::make_shared<UdpServer>();
        srtSrv->setOnCreateSocket([](const EventPoller::Ptr &poller, const Buffer::Ptr &buf, struct sockaddr *, int) {
            if (!buf) {
                return Socket::createSocket(poller, false);
            }
            auto new_poller = SRT::SrtSession::queryPoller(buf);
            if (!new_poller) {
                // Handshake phase one
                return Socket::createSocket(poller, false);
            }
            return Socket::createSocket(new_poller, false);
        });

        uint16_t srtPort = mINI::Instance()[SRT::kPort];
#endif //defined(ENABLE_SRT)

        installWebApi();
        InfoL << "The http API interface has been started";
        installWebHook();
        InfoL << "The http hook interface has been started";
        installManagerHook();
        InfoL << "The manager hook interface has been started";

        try {
            // rtsp server, default port 554
            if (rtspPort) { rtspSrv->start<RtspSession>(rtspPort, listen_ip); }
            // rtsps server, default port 322
            if (rtspsPort) { rtspSSLSrv->start<RtspSessionWithSSL>(rtspsPort, listen_ip); }

            // rtmp server, default port 1935
            if (rtmpPort) { rtmpSrv->start<RtmpSession>(rtmpPort, listen_ip); }
            // rtmps server, default port 19350
            if (rtmpsPort) { rtmpsSrv->start<RtmpSessionWithSSL>(rtmpsPort, listen_ip); }

            // http server, default port 80
            if (httpPort) { httpSrv->start<HttpSession>(httpPort, listen_ip); }
            // https server, default port 443
            if (httpsPort) { httpsSrv->start<HttpsSession>(httpsPort, listen_ip); }

            // telnet remote debug server
            if (shellPort) { shellSrv->start<ShellSession>(shellPort, listen_ip); }

#if defined(ENABLE_RTPPROXY)
            // create rtp server
            if (rtpPort) { rtpServer->start(rtpPort, listen_ip.c_str()); }
#endif//defined(ENABLE_RTPPROXY)

#if defined(ENABLE_WEBRTC)
            // webrtc udp server
            if (rtcPort) { rtcSrv_udp->start<WebRtcSession>(rtcPort, listen_ip);}

            if (rtcTcpPort) { rtcSrv_tcp->start<WebRtcSession>(rtcTcpPort, listen_ip);}
             
#endif//defined(ENABLE_WEBRTC)

#if defined(ENABLE_SRT)
            // srt udp server
            if (srtPort) { srtSrv->start<SRT::SrtSession>(srtPort, listen_ip); }
#endif//defined(ENABLE_SRT)

        } catch (std::exception &ex) {
            ErrorL << "Start server failed: " << ex.what();
            sleep(1);
#if !defined(_WIN32)
            if (pid != getpid() && kill_parent_if_failed) {
                // kill the daemon process
                kill(pid, SIGINT);
            }
#endif
            return -1;
        }

        // set exit signal handler
        static semaphore sem;
        signal(SIGINT, [](int) {
            InfoL << "SIGINT:exit";
            signal(SIGINT, SIG_IGN); // Set the exit signal
            sem.post();
        }); // Set the exit signal

        signal(SIGTERM,[](int) {
            WarnL << "SIGTERM:exit";
            signal(SIGTERM, SIG_IGN);
            sem.post();
        });

#if !defined(_WIN32)
        signal(SIGHUP, [](int) {
            mediakit::loadIniConfig(g_ini_file.data());
            g_reload_certificates();
        });
#endif
        sem.wait();
    }
    unInstallWebApi();
    unInstallWebHook();
    unInstallManagerHook();
    onProcessExited();

    // sleep for 1 second before exiting, to prevent resource release order errors
    InfoL << "The program is exiting, please wait...";
    sleep(1);
    InfoL << "The program exit is completed!";
    return 0;
}

#ifndef DISABLE_MAIN
int main(int argc,char *argv[]) {
    return start_main(argc,argv);
}
#endif //DISABLE_MAIN


