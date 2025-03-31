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
#include "Util/CMD.h"
#include "Util/logger.h"
#include "Common/config.h"
#include "Player/PlayerProxy.h"
#include "Thread/WorkThreadPool.h"

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
                             "in",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             nullptr,/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Pull the stream url, support rtsp/rtmp/hls",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('c',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "count",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "1000",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Number of stream pull agents",/*This option specifies text*/
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

        (*_parser) << Option('D',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "demand",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "1",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Whether to transfer the protocol on demand, set to 1 to improve performance",/*This option specifies text*/
                             nullptr);


    }

    ~CMD_main() override {}

    const char *description() const override {
        return "Main program command parameters";
    }
};

// This program is a pull stream proxy performance test tool for s3m, used to test the pull stream proxy performance
int main(int argc, char *argv[]) {
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

        int threads = cmd_main["threads"];
        LogLevel logLevel = (LogLevel) cmd_main["level"].as<int>();
        logLevel = MIN(MAX(logLevel, LTrace), LError);
        auto in_url = cmd_main["in"];
        auto rtp_type = cmd_main["rtp"].as<int>();
        auto delay_ms = cmd_main["delay"].as<int>();
        auto proxy_count = cmd_main["count"].as<int>();
        auto merge_ms = cmd_main["merge"].as<int>();
        auto demand = cmd_main["demand"].as<int>();

        // Set log
        Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel", logLevel));
        // Start asynchronous log thread
        Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

        // Set the number of threads
        EventPollerPool::setPoolSize(threads);
        WorkThreadPool::setPoolSize(threads);

        // Set merge write
        mINI::Instance()[General::kMergeWriteMS] = merge_ms;
        mINI::Instance()[Protocol::kRtspDemand] = demand;
        mINI::Instance()[Protocol::kRtmpDemand] = demand;
        mINI::Instance()[Protocol::kHlsDemand] = demand;
        mINI::Instance()[Protocol::kTSDemand] = demand;
        mINI::Instance()[Protocol::kFMP4Demand] = demand;

        map<string, PlayerProxy::Ptr> proxyMap;
        ProtocolOption option;
        option.enable_hls = false;
        option.enable_mp4 = false;
        for (auto i = 0; i < proxy_count; ++i) {
            auto stream = to_string(i);
            auto tuple = MediaTuple{DEFAULT_VHOST, "live", stream, ""};
            PlayerProxy::Ptr player(new PlayerProxy(tuple, option));
            (*player)[Client::kRtpType] = rtp_type;
            player->play(in_url);
            proxyMap.emplace(stream, player);
            // Sleep before starting the next pull stream proxy to prevent a large number of connections in a short time
            if (delay_ms > 0) {
                usleep(1000 * delay_ms);
            }
        }

        static semaphore sem;
        signal(SIGINT, [](int) { sem.post(); });// Set the exit signal
        sem.wait();
    }
    return 0;
}

