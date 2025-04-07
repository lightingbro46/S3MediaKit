#include <map>
#include <signal.h>
#include <iostream>

#include "Util/MD5.h"
#include "Util/logger.h"
#include "Util/SSLBox.h"
#include "Util/onceToken.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"

#include "Common/config.h"
#include "Rtsp/UDPServer.h"
#include "Rtsp/RtspSession.h"
#include "Rtmp/RtmpSession.h"
#include "Shell/ShellSession.h"
#include "Rtmp/FlvMuxer.h"
#include "Player/PlayerProxy.h"
#include "Http/WebSocketSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace mediakit {
// //////////HTTP Configuration///////////
namespace Http {
#define HTTP_FIELD "http."
#define HTTP_PORT 80
const string kPort = HTTP_FIELD"port";
#define HTTPS_PORT 443
const string kSSLPort = HTTP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = HTTP_PORT;
    mINI::Instance()[kSSLPort] = HTTPS_PORT;
},nullptr);
}//namespace Http

// //////////SHELL Configuration///////////
namespace Shell {
#define SHELL_FIELD "shell."
#define SHELL_PORT 9000
const string kPort = SHELL_FIELD"port";
onceToken token1([](){
    mINI::Instance()[kPort] = SHELL_PORT;
},nullptr);
} //namespace Shell

// //////////RTSP Server Configuration///////////
namespace Rtsp {
#define RTSP_FIELD "rtsp."
#define RTSP_PORT 554
#define RTSPS_PORT 322
const string kPort = RTSP_FIELD"port";
const string kSSLPort = RTSP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = RTSP_PORT;
    mINI::Instance()[kSSLPort] = RTSPS_PORT;
},nullptr);

} //namespace Rtsp

// //////////RTMP Server Configuration///////////
namespace Rtmp {
#define RTMP_FIELD "rtmp."
#define RTMP_PORT 1935
const string kPort = RTMP_FIELD"port";
onceToken token1([](){
    mINI::Instance()[kPort] = RTMP_PORT;
},nullptr);
} //namespace RTMP
}  // namespace mediakit


#define REALM "realm_s3mediakit"
static map<string,FlvRecorder::Ptr> s_mapFlvRecorder;
static mutex s_mtxFlvRecorder;

void initEventListener() {
    static onceToken s_token([]() {
        // Listen for kBroadcastOnGetRtspRealm event to decide if rtsp link needs authentication (traditional rtsp authentication scheme) to access
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnGetRtspRealm, [](BroadcastOnGetRtspRealmArgs) {
             DebugL << "Whether RTSP requires authentication events:" << args.getUrl() << " " << args.params;
             if (string("1") == args.stream) {
                 // live/1 needs authentication
                 // This stream needs authentication and sets realm
                 invoker(REALM);
             } else {
                 // Sometimes we need to query redis or database to determine if the stream needs authentication, which can be done asynchronously through invoker
                 // This stream does not need authentication
                 invoker("");
             }
         });

        // Listen for kBroadcastOnRtspAuth event to return the correct rtsp authentication username and password
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnRtspAuth, [](BroadcastOnRtspAuthArgs) {
            DebugL << "RTSP playback authentication:" << args.getUrl() << " " << args.params;
            DebugL << "RTSP Users:" << user_name << (must_no_encrypt ? " Base64" : " MD5") << " Login";
            string user = user_name;
            // Assuming we read the database asynchronously
            if (user == "test0") {
                // Assuming the database stores plaintext
                invoker(false, "pwd0");
                return;
            }

            if (user == "test1") {
                // Assuming the database stores ciphertext
                auto encrypted_pwd = MD5(user + ":" + REALM + ":" + "pwd1").hexdigest();
                invoker(true, encrypted_pwd);
                return;
            }
            if (user == "test2" && must_no_encrypt) {
                // Assuming login is test2 and login in base64 format, in this case we provide encrypted password, which will cause authentication failure
                // You can shield this insecure encryption method in this way
                invoker(true, "pwd2");
                return;
            }

            // Other user passwords are the same as usernames
            invoker(false, user);
        });


        // Listen for rtsp/rtmp push stream event, return result to inform whether there is push stream permission
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
            DebugL << "Promotion and authentication:" << args.getUrl() << " " << args.params;
            invoker("", ProtocolOption());//Authentication was successful
            // invoker("this is auth failed message");//Authentication failed
        });

        // Listen for rtsp/rtsps/rtmp/http-flv playback event, return result to inform whether there is playback permission (rtsp can implement authentication through kBroadcastOnRtspAuth or this event)
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
            DebugL << "Play authentication:" << args.getUrl() << " " << args.params;
            invoker("");//Authentication was successful
            // invoker("this is auth failed message");//Authentication failed
        });

        // Shell login event, you can log in to the server through shell to execute some commands
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastShellLogin, [](BroadcastShellLoginArgs) {
            DebugL << "shell login:" << user_name << " " << passwd;
            invoker("");//Authentication was successful
            // invoker("this is auth failed message");//Authentication failed
        });

        // Listen for rtsp/rtmp source registration or cancellation event; this is used to test rtmp saving as flv recording, saved in the http root directory
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaChanged, [](BroadcastMediaChangedArgs) {
            auto tuple = sender.getMediaTuple();
            if (sender.getSchema() == RTMP_SCHEMA && tuple.app == "live") {
                lock_guard<mutex> lck(s_mtxFlvRecorder);
                auto key = tuple.shortUrl();
                if (bRegist) {
                    DebugL << "Start recording RTMP:" << sender.getUrl();
                    GET_CONFIG(string, http_root, Http::kRootPath);
                    auto path = http_root + "/" + key + "_" + to_string(time(NULL)) + ".flv";
                    FlvRecorder::Ptr recorder(new FlvRecorder);
                    try {
                        recorder->startRecord(EventPollerPool::Instance().getPoller(),
                                              dynamic_pointer_cast<RtmpMediaSource>(sender.shared_from_this()), path);
                        s_mapFlvRecorder[key] = recorder;
                    } catch (std::exception &ex) {
                        WarnL << ex.what();
                    }
                } else {
                    s_mapFlvRecorder.erase(key);
                }
            }
        });

        // Listen for playback failure (stream not found) event
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastNotFoundStream, [](BroadcastNotFoundStreamArgs) {
            /**
             * You can pull the stream again when this event is triggered, which can implement on-demand pulling
             * After pulling the stream successfully, S3MediaKit will immediately forward it to the player (the maximum waiting time is about 5 seconds, if it still fails to pull the stream within 5 seconds, the player will play failure)
             */
            DebugL << "No stream event found:" << args.getUrl() << " " << args.params;
        });


        // Listen for playback or push stream end event to consume traffic
        NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastFlowReport, [](BroadcastFlowReportArgs) {
            DebugL << "Player (Puser) Disconnect Event:" << args.getUrl() << " " << args.params << "\r\nUsage traffic:" << totalBytes << " bytes, Connection time:" << totalDuration << "second";

        });


    }, nullptr);
}

#if !defined(SIGHUP)
#define SIGHUP 1
#endif

int main(int argc,char *argv[]) {
    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().add(std::make_shared<FileChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());
    // Load configuration file, if the configuration file does not exist, create one
    loadIniConfig();
    initEventListener();

    // This is the pull stream address, supports rtmp/rtsp protocol, and the payload must be H264+AAC
    // If it is other unrecognized audio and video, it will be ignored (for example, h264+adpcm will remove audio after forwarding)
    auto urlList = {"rtsp://admin:admin123@192.168.1.64:554/cam/realmonitor?channel=1&subtype=1"
            // rtsp link supports inputting username and password
            /*"rtsp://admin:jzan123456@192.168.0.122/"*/};
    map<string, PlayerProxy::Ptr> proxyMap;
    int i = 0;
    for (auto &url : urlList) {
        // PlayerProxy constructor's first two parameters are application name (app) and stream id (streamId)
        // For example, if the application is live and the stream id is 0, the live address is:

        // hls address: http://127.0.0.1/live/0/hls.m3u8
        // http-flv address: http://127.0.0.1/live/0.flv
        // rtsp address: rtsp://127.0.0.1/live/0
        // rtmp address: rtmp://127.0.0.1/live/0

        // The recorded address is (of course, vlc does not support so many levels of rtmp url, you can use test_player to test rtmp on-demand):
        //http://127.0.0.1/record/live/0/2017-04-11/11-09-38.mp4
        //rtsp://127.0.0.1/record/live/0/2017-04-11/11-09-38.mp4
        //rtmp://127.0.0.1/record/live/0/2017-04-11/11-09-38.mp4
        auto tuple = MediaTuple{DEFAULT_VHOST, "live", std::string("chn") + to_string(i).data(), ""};
        PlayerProxy::Ptr player(new PlayerProxy(tuple, ProtocolOption()));
        // Specify RTP over TCP (effective when playing rtsp)
        (*player)[Client::kRtpType] = Rtsp::RTP_TCP;
        // Start playing. If playback fails or is interrupted, it will automatically retry several times. The number of retries is configured in the configuration file, and the default is to retry indefinitely
        player->play(url);
        // You need to save PlayerProxy, otherwise the object will be destroyed when the scope ends
        proxyMap.emplace(to_string(i), player);
        ++i;
    }

    DebugL << "\r\n"
              " The first two parameters of the PlayerProxy constructor are the application name (app) and the stream id (streamId)\n"
              " For example, if the application is live and the stream id is 0, then the live broadcast address is:\n"
              " hls address : http://127.0.0.1/live/0/hls.m3u8\n"
              " http-flv address : http://127.0.0.1/live/0.flv\n"
              " rtsp address : rtsp://127.0.0.1/live/0\n"
              " rtmp address : rtmp://127.0.0.1/live/0";

    // Load the certificate, which contains the public key and private key
    SSL_Initor::Instance().loadCertificate((exeDir() + "ssl.p12").data());
    // Trust a self-signed certificate
    SSL_Initor::Instance().trustCertificate((exeDir() + "ssl.p12").data());
    // Do not ignore invalid certificates (such as self-signed or expired certificates)
    SSL_Initor::Instance().ignoreInvalidCertificate(false);

    uint16_t shellPort = mINI::Instance()[Shell::kPort];
    uint16_t rtspPort = mINI::Instance()[Rtsp::kPort];
    uint16_t rtspsPort = mINI::Instance()[Rtsp::kSSLPort];
    uint16_t rtmpPort = mINI::Instance()[Rtmp::kPort];
    uint16_t httpPort = mINI::Instance()[Http::kPort];
    uint16_t httpsPort = mINI::Instance()[Http::kSSLPort];

    // A simple telnet server, which can be used for server debugging, but cannot use port 23, otherwise telnet will have inexplicable phenomena
    // Test method: telnet 127.0.0.1 9000
    TcpServer::Ptr shellSrv(new TcpServer());
    TcpServer::Ptr rtspSrv(new TcpServer());
    TcpServer::Ptr rtmpSrv(new TcpServer());
    TcpServer::Ptr httpSrv(new TcpServer());

    shellSrv->start<ShellSession>(shellPort);
    rtspSrv->start<RtspSession>(rtspPort);//Default 554
    rtmpSrv->start<RtmpSession>(rtmpPort);//Default 1935
    // http server
    httpSrv->start<HttpSession>(httpPort);//Default 80

    // If ssl is supported, you can also enable the https server
    TcpServer::Ptr httpsSrv(new TcpServer());
    // https server
    httpsSrv->start<HttpsSession>(httpsPort);//Default 443

    // rtsp server that supports ssl encryption, which can be used for devices such as Amazon Echo Show to access
    TcpServer::Ptr rtspSSLSrv(new TcpServer());
    rtspSSLSrv->start<RtspSessionWithSSL>(rtspsPort);//Default 322

    // The server supports dynamic port switching (without affecting existing connections)
    NoticeCenter::Instance().addListener(ReloadConfigTag,Broadcast::kBroadcastReloadConfig,[&](BroadcastReloadConfigArgs){
        // Recreate the server
        if(shellPort != mINI::Instance()[Shell::kPort].as<uint16_t>()){
            shellPort = mINI::Instance()[Shell::kPort];
            shellSrv->start<ShellSession>(shellPort);
            InfoL << "Restart the shell server:" << shellPort;
        }
        if(rtspPort != mINI::Instance()[Rtsp::kPort].as<uint16_t>()){
            rtspPort = mINI::Instance()[Rtsp::kPort];
            rtspSrv->start<RtspSession>(rtspPort);
            InfoL << "Restart the rtsp server" << rtspPort;
        }
        if(rtmpPort != mINI::Instance()[Rtmp::kPort].as<uint16_t>()){
            rtmpPort = mINI::Instance()[Rtmp::kPort];
            rtmpSrv->start<RtmpSession>(rtmpPort);
            InfoL << "Restart rtmp server" << rtmpPort;
        }
        if(httpPort != mINI::Instance()[Http::kPort].as<uint16_t>()){
            httpPort = mINI::Instance()[Http::kPort];
            httpSrv->start<HttpSession>(httpPort);
            InfoL << "Restart the http server" << httpPort;
        }
        if(httpsPort != mINI::Instance()[Http::kSSLPort].as<uint16_t>()){
            httpsPort = mINI::Instance()[Http::kSSLPort];
            httpsSrv->start<HttpsSession>(httpsPort);
            InfoL << "Restart https server" << httpsPort;
        }

        if(rtspsPort != mINI::Instance()[Rtsp::kSSLPort].as<uint16_t>()){
            rtspsPort = mINI::Instance()[Rtsp::kSSLPort];
            rtspSSLSrv->start<RtspSessionWithSSL>(rtspsPort);
            InfoL << "Restart rtsps server" << rtspsPort;
        }
    });

    // Set the exit signal processing function
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// Set the exit signal
    signal(SIGHUP, [](int) { loadIniConfig(); });
    sem.wait();

    lock_guard<mutex> lck(s_mtxFlvRecorder);
    s_mapFlvRecorder.clear();
    return 0;
}

