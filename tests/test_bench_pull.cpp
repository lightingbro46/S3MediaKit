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
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/CMD.h"
#include "Common/config.h"
#include "Rtsp/UDPServer.h"
#include "Thread/WorkThreadPool.h"
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
                             "Number of streaming players",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('d',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "delay",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "10",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Start stream pull client interval, in milliseconds",/*This option specifies text*/
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

// This program is used for pulling stream playback performance testing
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
    auto in_url = cmd_main["in"];
    auto rtp_type = cmd_main["rtp"].as<int>();
    auto delay_ms = cmd_main["delay"].as<int>();
    auto player_count = cmd_main["count"].as<int>();

    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel", logLevel));
    // Start asynchronous log thread
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // Set the number of threads
    EventPollerPool::setPoolSize(threads);
    WorkThreadPool::setPoolSize(threads);

    // Player map
    recursive_mutex mtx;
    unordered_map<void *, MediaPlayer::Ptr> player_map;

    auto add_player = [&]() {
        auto player = std::make_shared<MediaPlayer>();
        auto tag = player.get();
        player->setOnCreateSocket([](const EventPoller::Ptr &poller) {
            // Socket close mutex, improve performance
            return Socket::createSocket(poller, false);
        });
        // Set playback failure listener
        player->setOnPlayResult([&mtx, &player_map, tag](const SockException &ex) {
            if (ex) {
                // Playback failed, remove it
                lock_guard<recursive_mutex> lck(mtx);
                player_map.erase(tag);
            }
        });
        // Set playback interruption listener
        player->setOnShutdown([&mtx, &player_map, tag](const SockException &ex) {
            // Playback interrupted, remove it
            lock_guard<recursive_mutex> lck(mtx);
            player_map.erase(tag);
        });
        // Set to performance test mode
        (*player)[Client::kBenchmarkMode] = true;
        // Set RTSP pull mode (effective when pulling RTSP stream)
        (*player)[Client::kRtpType] = rtp_type;
        // Improve stress test performance and accuracy
        (*player)[Client::kWaitTrackReady] = false;
        // Initiate playback request
        player->play(in_url);

        // Keep the object from being destroyed
        lock_guard<recursive_mutex> lck(mtx);
        player_map.emplace(tag, std::move(player));

        // Sleep and then start the next playback to prevent massive connections in a short time
        if (delay_ms > 0) {
            usleep(1000 * delay_ms);
        }
    };

    // Add so many players
    for (auto i = 0; i < player_count; ++i) {
        add_player();
    }

    // Set exit signal
    static bool exit_flag = false;
    signal(SIGINT, [](int) { exit_flag = true; });
    while (!exit_flag) {
        // Sleep for one second and print
        sleep(1);

        size_t alive_player = 0;
        {
            lock_guard<recursive_mutex> lck(mtx);
            alive_player = player_map.size();
        }
        InfoL << "Number of online players:" << alive_player;
        size_t re_try = player_count - alive_player;
        while (!exit_flag && re_try--) {
            // Some players failed to play, so we retry adding them
            add_player();
        }
    }

    return 0;
}

