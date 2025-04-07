#include <signal.h>
#include <string>
#include <iostream>
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Http/WebSocketClient.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

class EchoTcpClient : public TcpClient {
public:
    EchoTcpClient(const EventPoller::Ptr &poller = nullptr){
        InfoL;
    }
    ~EchoTcpClient() override {
        InfoL;
    }
protected:
    void onRecv(const Buffer::Ptr &pBuf) override {
        DebugL << pBuf->toString();
    }
    // Passive disconnection callback
    void onError(const SockException &ex) override {
        WarnL << ex;
    }
    // Triggered every 2 seconds after a successful TCP connection
    void onManager() override {
        SockSender::send("echo test!");
        DebugL << "send echo test";
    }
    // Server connection result callback
    void onConnect(const SockException &ex) override{
        DebugL << ex;
    }

    // Callback after all data has been sent
    void onFlush() override{
        DebugL;
    }
};

int main(int argc, char *argv[]) {
    // Set exit signal processing function
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// Set the exit signal

    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    {
        WebSocketClient<EchoTcpClient>::Ptr client = std::make_shared<WebSocketClient<EchoTcpClient> >();
        client->startConnect("127.0.0.1", 80);
        sem.wait();
    }
    return 0;
}

