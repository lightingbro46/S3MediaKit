#include <signal.h>
#include <string>
#include <iostream>
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Poller/EventPoller.h"
#include "Http/HttpRequester.h"
#include "Http/HttpDownloader.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

int main(int argc, char *argv[]) {
    // Set the exit signal processing function
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); });// Set the exit signal

    // Set the log
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // Load the certificate, the certificate contains the public key and private key
    SSL_Initor::Instance().loadCertificate((exeDir() + "ssl.p12").data());
    // Trust a self-signed certificate
    SSL_Initor::Instance().trustCertificate((exeDir() + "ssl.p12").data());
    // Do not ignore invalid certificates (such as self-signed or expired certificates)
    SSL_Initor::Instance().ignoreInvalidCertificate(false);

    ///////////////////////////////http downloader///////////////////////
    // Downloader map
    map<string, HttpDownloader::Ptr> downloaderMap;
    // Download two files, one is HTTP download, and the other is HTTPS download
    auto urlList = {"http://www.baidu.com/img/baidu_resultlogo@2.png",
                    "https://www.baidu.com/img/baidu_resultlogo@2.png"};

    for (auto &url : urlList) {
        // Create a downloader
        HttpDownloader::Ptr downloader(new HttpDownloader());
        downloader->setOnResult([](const SockException &ex, const string &filePath) {
            DebugL << "=====================HttpDownloader result=======================";
            // Download result callback
            if (!ex) {
                // File download successful
                InfoL << "download file success:" << filePath;
            } else {
                // Download failed
                WarnL << "code:" << ex.getErrCode() << " msg:" << ex.what();
            }
        });
        // Resume function, enabling it may encounter a 416 error (because the file was downloaded completely last time)
        downloader->startDownload(url, exeDir() + MD5(url).hexdigest() + ".jpg", true);
        // The downloader must be strongly referenced, otherwise, it will be destroyed when the scope is invalid
        downloaderMap.emplace(url, downloader);
    }

    ///////////////////////////////http get///////////////////////
    // Create an HTTP requestor
    HttpRequester::Ptr requesterGet(new HttpRequester());
    // Use the GET method to request
    requesterGet->setMethod("GET");
    // Set the HTTP request header, we assume setting the cookie, of course, you can also set other HTTP headers
    requesterGet->addHeader("Cookie", "SESSIONID=e1aa89b3-f79f-4ac6-8ae2-0cea9ae8e2d7");
    // Start the request, this API will return the current host's external network IP and other information
    requesterGet->startRequester("http://pv.sohu.com/cityjson?ie=utf-8",//URL address
                                 [](const SockException &ex,                                 //Network-related failure information, if empty, it means success
                                    const Parser &parser) {                              //http reply body
                                     DebugL << "=====================HttpRequester GET===========================";
                                     if (ex) {
                                         // Network-related errors
                                         WarnL << "network err:" << ex.getErrCode() << " " << ex.what();
                                     } else {
                                         // Print HTTP response information
                                         _StrPrinter printer;
                                         for (auto &pr: parser.getHeader()) {
                                             printer << pr.first << ":" << pr.second << "\r\n";
                                         }
                                         InfoL << "status:" << parser.status() << "\r\n"
                                               << "header:\r\n" << (printer << endl)
                                               << "\r\nbody:" << parser.content();
                                     }
                                 });

    ///////////////////////////////http post///////////////////////
    // Create an HTTP requestor
    HttpRequester::Ptr requesterPost(new HttpRequester());
    // Use the POST method to request
    requesterPost->setMethod("POST");
    // Set the HTTP request header
    requesterPost->addHeader("X-Requested-With", "XMLHttpRequest");
    requesterPost->addHeader("Origin", "http://fanyi.baidu.com");
    // Set the POST parameter list
    HttpArgs args;
    args["query"] = "test";
    args["from"] = "en";
    args["to"] = "zh";
    args["transtype"] = "translang";
    args["simple_means_flag"] = "3";
    requesterPost->setBody(args.make());
    // Start the request
    requesterPost->startRequester("http://fanyi.baidu.com/langdetect",//URL address
                                  [](const SockException &ex,                          //Network-related failure information, if empty, it means success
                                     const Parser &parser) {                       //http reply body
                                      DebugL << "=====================HttpRequester POST==========================";
                                      if (ex) {
                                          // Network-related errors
                                          WarnL << "network err:" << ex.getErrCode() << " " << ex.what();
                                      } else {
                                          // Print HTTP response information
                                          _StrPrinter printer;
                                          for (auto &pr: parser.getHeader()) {
                                              printer << pr.first << ":" << pr.second << "\r\n";
                                          }
                                          InfoL << "status:" << parser.status() << "\r\n"
                                                << "header:\r\n" << (printer << endl)
                                                << "\r\nbody:" << parser.content();
                                      }
                                  });

    ///////////////////////////////http upload///////////////////////
    // Create an HTTP requestor
    HttpRequester::Ptr requesterUploader(new HttpRequester());
    // Use the POST method to request
    requesterUploader->setMethod("POST");
    // Set the HTTP request header
    HttpArgs argsUploader;
    argsUploader["query"] = "test";
    argsUploader["from"] = "en";
    argsUploader["to"] = "zh";
    argsUploader["transtype"] = "translang";
    argsUploader["simple_means_flag"] = "3";

    static string boundary = "0xKhTmLbOuNdArY";
    HttpMultiFormBody::Ptr body(new HttpMultiFormBody(argsUploader, exePath(), boundary));
    requesterUploader->setBody(body);
    requesterUploader->addHeader("Content-Type", HttpMultiFormBody::multiFormContentType(boundary));
    // Start the request
    requesterUploader->startRequester("http://fanyi.baidu.com/langdetect",//URL address
                                      [](const SockException &ex,                          //Network-related failure information, if empty, it means success
                                         const Parser &parser) {                       //http reply body
                                          DebugL << "=====================HttpRequester Uploader==========================";
                                          if (ex) {
                                              // Network-related errors
                                              WarnL << "network err:" << ex.getErrCode() << " " << ex.what();
                                          } else {
                                              // Print HTTP response information
                                              _StrPrinter printer;
                                              for (auto &pr: parser.getHeader()) {
                                                  printer << pr.first << ":" << pr.second << "\r\n";
                                              }
                                              InfoL << "status:" << parser.status() << "\r\n"
                                                    << "header:\r\n" << (printer << endl)
                                                    << "\r\nbody:" << parser.content();
                                          }
                                      });

    sem.wait();
    return 0;
}

