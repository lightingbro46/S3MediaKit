/*	
 * Copyright (c) 2025-present The S3MediaKit project authors. All Rights Reserved.
 *	
 * This file is part of S3MediaKit(https://github.com/S3MediaKit/S3MediaKit).
 *	
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors	
 * may be found in the AUTHORS file in the root of the source tree.	
 */

#include <signal.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/CMD.h"
#include "Util/File.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Rtsp/Rtsp.h"
#include "Thread/WorkThreadPool.h"
#include "Pusher/MediaPusher.h"
#include "Player/PlayerProxy.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

class CMD_main : public CMD {
public:
    CMD_main() {
        _parser.reset(new OptionParser(nullptr));

        (*_parser) << Option('l',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "level",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string(LTrace).data(),/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Log Level,LTrace~LError(0~4)",/*This option specifies text*/
                             nullptr);


        (*_parser) << Option('t',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "threads",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string(thread::hardware_concurrency()).data(),/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Number of threads triggered by startup event",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('i',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "inputs",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "/tmp/inputs.txt",/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Pulling the stream address configuration file, supporting rtmp, rtsp, hls, multiple addresses are divided by \"line breaks\"",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('o',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "outputs",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "/tmp/outputs.txt",/*This option default value*/
                             false,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Pushing address configuration file, supports rtmp and rtsp, and multiple addresses are divided by \"line breaks\"",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('d',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "delay",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "50",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Start stream pull proxy interval, in milliseconds",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('m',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "merge",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "300",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Merge write milliseconds, merge write can improve performance",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('T',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "rtp",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string((int) (Rtsp::RTP_TCP)).data(),/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "rtsp stream pulling method, supports tcp/udp/multicast:0/1/2",/*This option specifies text*/
                             nullptr);

    }

    ~CMD_main() override {}

    const char *description() const override {
        return "Main program command parameters";
    }
};


// This program is a performance testing tool for s3m's relay push, used to test the relay push performance of the pull stream agent
int main(int argc, char *argv[]) {
    CMD_main cmd_main;
    try {
        cmd_main.operator()(argc, argv);
    } catch (ExitException &) {
        return 0;
    } catch (std::exception &ex) {
        cout << ex.what() << endl;
        return -1;
    }

    int threads = cmd_main["threads"];
    LogLevel logLevel = (LogLevel) cmd_main["level"].as<int>();
    logLevel = MIN(MAX(logLevel, LTrace), LError);
    auto in_urls = cmd_main["inputs"];
    auto out_urls = cmd_main["outputs"];
    auto rtp_type = cmd_main["rtp"].as<int>();
    auto delay_ms = cmd_main["delay"].as<int>();
    auto merge_ms = cmd_main["merge"].as<int>();

    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel", logLevel));
    // Start asynchronous log thread
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // Set the number of threads
    EventPollerPool::setPoolSize(threads);
    WorkThreadPool::setPoolSize(threads);

    // Set merge write
    mINI::Instance()[General::kMergeWriteMS] = merge_ms;


    std::vector<std::string> input_urls;
    std::vector<std::string> output_urls;

    auto parse_urls = [&]() {
        // Get input source list
        auto inputs = ::split(toolkit::File::loadFile(in_urls), "\n");
        for(auto &url : inputs){
            if(url.empty() || url.find("://") == std::string::npos) {
                continue;
            }
            auto input_url = ::trim(url);
            input_urls.emplace_back(input_url);
        }
        // Get output source list
        auto outputs = ::split(toolkit::File::loadFile(out_urls), "\n");
        for(auto &url : outputs){
            if(url.empty() || url.find("://") == std::string::npos){
                continue;
            }
            auto output_url = ::trim(url);
            output_urls.emplace_back(output_url);
        }

        if(input_urls.empty() || input_urls.size() != output_urls.size()){
            return -1;
        }

        for(size_t i = 0; i < input_urls.size(); i++){
            InfoL << "Pull the stream address: " << input_urls[i] << ",Pushing address:" << output_urls[i];
        }
        return 0;
    };

    if (0 != parse_urls()){
        cout << "Please check whether the inputs and outputs files are correct！" << endl;
        return -1;
    }

    // Pusher map
    recursive_mutex mtx;
    unordered_map<int, PlayerProxy::Ptr> proxy_map;
    unordered_map<int, MediaPusher::Ptr> pusher_map;

    auto add_pusher = [&](const MediaSource::Ptr &src, const string &url, int index) {
        auto pusher = std::make_shared<MediaPusher>(src);
        pusher->setOnCreateSocket([](const EventPoller::Ptr &poller) {
            // Socket close mutex, improve performance
            return Socket::createSocket(poller, false);
        });
        // Set push failure listener
        pusher->setOnPublished([&mtx, &pusher_map, index](const SockException &ex) {
            if (ex) {
                // Push failure, remove it
                lock_guard<recursive_mutex> lck(mtx);
                pusher_map.erase(index);
            }
        });
        // Set push midway disconnection listener
        pusher->setOnShutdown([&mtx, &pusher_map, index](const SockException &ex) {
            // Push midway failure, remove it
            lock_guard<recursive_mutex> lck(mtx);
            pusher_map.erase(index);
        });
        // Set RTSP push mode (effective when pushing RTSP)
        (*pusher)[Client::kRtpType] = rtp_type;
        pusher->publish(url);
        // Keep the object from being destroyed
        lock_guard<recursive_mutex> lck(mtx);
        pusher_map.emplace(index, std::move(pusher));
        // Sleep and then start the next push to prevent massive connections in a short time
        if (delay_ms > 0) {
            usleep(1000 * delay_ms);
        }
    };

    // Add relay task
    for(size_t i = 0; i < input_urls.size(); i++) {
        // Sleep for one second and print
        sleep(1);
        auto schema = findSubString(output_urls[i].data(), nullptr, "://");
        if (schema != RTSP_SCHEMA && schema != RTMP_SCHEMA) {
            cout << "The push streaming protocol only supports rtsp or rtmp!" << endl;
            return -1;
        }
        ProtocolOption option;
        option.enable_ts = false;
        option.enable_fmp4 = false;
        option.enable_hls = false;
        option.enable_mp4 = false;
        option.modify_stamp = (int)ProtocolOption::kModifyStampRelative;
        // Add pull stream agent
        auto tuple = MediaTuple { DEFAULT_VHOST, "app", std::to_string(i), "" };
        auto proxy = std::make_shared<PlayerProxy>(tuple, option, -1, nullptr, 1);
        // Start pull stream agent
        proxy->play(input_urls[i]);
        proxy_map.emplace(i, std::move(proxy));
    }

    // Set exit signal
    static bool exit_flag = false;
    signal(SIGINT, [](int) { exit_flag = true; });
    while (!exit_flag) {
        // Sleep for one second and print
        sleep(1);

        size_t alive_pusher = 0;
        {
            lock_guard<recursive_mutex> lck(mtx);
            alive_pusher = pusher_map.size();
        }
        InfoL << "Number of online retweeters:" << alive_pusher;

        auto find_pusher = [&](int index){
            lock_guard<recursive_mutex> lck(mtx);
            auto it = pusher_map.find(index);
            if (it == pusher_map.end()){
                return false;
            }
            return true;
        };
        for(size_t i = 0; i < input_urls.size(); i++) {
            if (!find_pusher(i)){
                auto input_url = input_urls[i];
                auto src = MediaSource::find(RTMP_SCHEMA, DEFAULT_VHOST, "app", std::to_string(i), false);
                if (src != nullptr){
                    add_pusher(src,output_urls[i],i);
                }
            }
        }
    }

    return 0;
}