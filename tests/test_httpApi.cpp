/*
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <map>
#include <signal.h>
#include <iostream>
#include "Util/SSLBox.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Common/config.h"
#include "Http/WebSocketSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace mediakit {
// //////////HTTP Configuration///////////
namespace Http {
#define HTTP_FIELD "http."
#define HTTP_PORT 80
const char kPort[] = HTTP_FIELD"port";
#define HTTPS_PORT 443
extern const char kSSLPort[] = HTTP_FIELD"sslport";
onceToken token1([](){
    mINI::Instance()[kPort] = HTTP_PORT;
    mINI::Instance()[kSSLPort] = HTTPS_PORT;
},nullptr);
}//namespace Http
}  // namespace mediakit

void initEventListener(){
    static onceToken s_token([](){
        NoticeCenter::Instance().addListener(nullptr,Broadcast::kBroadcastHttpRequest,[](BroadcastHttpRequestArgs){
            //const Parser &parser,HttpSession::HttpResponseInvoker &invoker,bool &consumed
            if(strstr(parser.url().data(),"/api/") != parser.url().data()){
                return;
            }
            // URLs starting with "/api/" indicate HTTP API
            consumed = true;//The http request has been consumed

            _StrPrinter printer;
            ////////////////method////////////////////
            printer << "\r\nmethod:\r\n\t" << parser.method();
            ////////////////url/////////////////
            printer << "\r\nurl:\r\n\t" << parser.url();
            ////////////////protocol/////////////////
            printer << "\r\nprotocol:\r\n\t" << parser.protocol();
            ///////////////args//////////////////
            printer << "\r\nargs:\r\n";
            for(auto &pr : parser.getUrlArgs()){
                printer <<  "\t" << pr.first << " : " << pr.second << "\r\n";
            }
            ///////////////header//////////////////
            printer << "\r\nheader:\r\n";
            for(auto &pr : parser.getHeader()){
                printer <<  "\t" << pr.first << " : " << pr.second << "\r\n";
            }
            ////////////////content/////////////////
            printer << "\r\ncontent:\r\n" << parser.content();
            auto contentOut = printer << endl;

            // //////////////We measure asynchronous responses, but you can also respond synchronously/////////////////
            EventPollerPool::Instance().getPoller()->async([invoker,contentOut](){
                HttpSession::KeyValue headerOut;
                // You can customize the header; if it has the same name as the default header, it will override it
                // Default headers include: Server, Connection, Date, Content-Type, Content-Length
                // Please do not override the Connection and Content-Length keys
                // Key name overrides are case-insensitive
                headerOut["TestHeader"] = "HeaderValue";
                invoker(200,headerOut,contentOut);
            });
        });
    }, nullptr);
}

int main(int argc,char *argv[]){
    // Set the exit signal processing function
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// Set the exit signal

    // Set the log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // Load the configuration file; if it does not exist, create one
    loadIniConfig();
    initEventListener();

    // Load the certificate, which includes the public and private keys
    SSL_Initor::Instance().loadCertificate((exeDir() + "ssl.p12").data());
    // Trust a self-signed certificate
    SSL_Initor::Instance().trustCertificate((exeDir() + "ssl.p12").data());
    // Do not ignore invalid certificates (e.g., self-signed or expired certificates)
    SSL_Initor::Instance().ignoreInvalidCertificate(false);


    // Start the HTTP server
    TcpServer::Ptr httpSrv(new TcpServer());
    httpSrv->start<HttpSession>(mINI::Instance()[Http::kPort]);//Default 80

    // If SSL is supported, you can also start the HTTPS server
    TcpServer::Ptr httpsSrv(new TcpServer());
    httpsSrv->start<HttpsSession>(mINI::Instance()[Http::kSSLPort]);//Default 443

    InfoL << "You can enter it in your browser:http://127.0.0.1/api/my_api?key0=val0&key1=Parameter1";

    sem.wait();
    return 0;
}

