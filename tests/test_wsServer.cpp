#include <signal.h>
#include <string>
#include <iostream>
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Http/WebSocketSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

/**
 * Echo Session
*/
class EchoSession : public Session {
public:
    EchoSession(const Socket::Ptr &pSock) : Session(pSock){
        DebugL;
    }
    virtual ~EchoSession(){
        DebugL;
    }

    void attachServer(const Server &server) override{
        DebugL << getIdentifier() << " " << Session::getIdentifier();
    }
    void onRecv(const Buffer::Ptr &buffer) override {
        // Echo Data
        SockSender::send("from EchoSession:");
        send(buffer);
    }
    void onError(const SockException &err) override{
        WarnL << err.what();
    }
    // Triggered at regular intervals, used for timeout management
    void onManager() override{
        DebugL;
    }
};


class EchoSessionWithUrl : public Session {
public:
    EchoSessionWithUrl(const Socket::Ptr &pSock) : Session(pSock){
        DebugL;
    }
    virtual ~EchoSessionWithUrl(){
        DebugL;
    }

    void attachServer(const Server &server) override{
        DebugL << getIdentifier() << " " << Session::getIdentifier();
    }
    void onRecv(const Buffer::Ptr &buffer) override {
        // Echo Data
        SockSender::send("from EchoSessionWithUrl:");
        send(buffer);
    }
    void onError(const SockException &err) override{
        WarnL << err.what();
    }
    // Triggered at regular intervals, used for timeout management
    void onManager() override{
        DebugL;
    }
};


/**
 * This object can create different objects based on the URL accessed by the WebSocket client
 */
struct EchoSessionCreator {
    // The returned Session must inherit from SendInterceptor, can return null (refuse connection)
    Session::Ptr operator()(const Parser &header, const HttpSession &parent, const Socket::Ptr &pSock, mediakit::WebSocketHeader::Type &type) {
//        return nullptr;
        if (header.url() == "/") {
            // Transport method can be specified
            // type = mediakit::WebSocketHeader::BINARY;
            return std::make_shared<SessionTypeImp<EchoSession> >(header, parent, pSock);
        }
        return std::make_shared<SessionTypeImp<EchoSessionWithUrl> >(header, parent, pSock);
    }
};

int main(int argc, char *argv[]) {
    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    SSL_Initor::Instance().loadCertificate((exeDir() + "ssl.p12").data());

    {
        TcpServer::Ptr httpSrv(new TcpServer());
        // HTTP server, supports WebSocket
        httpSrv->start<WebSocketSessionBase<EchoSessionCreator, HttpSession> >(80);//Default 80

        TcpServer::Ptr httpsSrv(new TcpServer());
        // HTTPS server, supports WebSocket
        httpsSrv->start<WebSocketSessionBase<EchoSessionCreator, HttpsSession> >(443);//Default 443

        TcpServer::Ptr httpSrvOld(new TcpServer());
        // Compatible with previous code (but does not support generating Session type based on URL)
        httpSrvOld->start<WebSocketSession<EchoSession, HttpSession> >(80);

        DebugL << "Please open the web page:http://www.websocket-test.com/, perform a test";
        DebugL << "connect ws://127.0.0.1/xxxx，ws://127.0.0.1/ The test results will be different, and different processing logics are supported according to the URL selection.";

        // Set exit signal processing function
        static semaphore sem;
        signal(SIGINT, [](int) { sem.post(); });// Set the exit signal
        sem.wait();
    }

    return 0;
}

