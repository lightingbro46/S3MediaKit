#ifdef ENABLE_MKV
#include <signal.h>
#include <atomic>
#include <iostream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/CMD.h"
#include "Common/config.h"
#include "Common/Parser.h"
#include "Rtsp/Rtsp.h"
#include "Thread/WorkThreadPool.h"
#include "Pusher/MediaPusher.h"
#include "Player/PlayerProxy.h"
#include "Record/MKVReader.h"
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
                             "Pull the stream url, supports rtsp/rtmp/hls/mp4 files",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('o',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "out",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             nullptr,/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Pushing the streaming url, supporting rtsp/rtmp",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('c',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "count",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "1000",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Number of push stream clients",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('d',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "delay",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "50",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Start push stream client interval, unit milliseconds",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('m',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "merge",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             "300",/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                             "Push stream merge write milliseconds, merge write can improve performance",/*This option specifies text*/
                             nullptr);

        (*_parser) << Option('T',/*This option is abbreviated, if it is \x00, it means there is no abbreviation*/
                             "rtp",/*The full name of this option, each option must have a full name; it must not be null or empty string*/
                             Option::ArgRequired,/*This option must be followed by a value*/
                             to_string((int) (Rtsp::RTP_TCP)).data(),/*This option default value*/
                             true,/*Whether this option must be assigned a value, if there is no default value and is ArgRequired, the user must provide this parameter otherwise an exception will be thrown*/
                            "rtsp stream pulling and streaming pushing methods, support tcp/udp:0/1", /*This option specifies text*/
                            nullptr);
    }

    ~CMD_main() override {}

    const char *description() const override { return "Main program command parameters"; }
};

// This program is used for streaming performance testing
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
    LogLevel logLevel = (LogLevel)cmd_main["level"].as<int>();
    logLevel = MIN(MAX(logLevel, LTrace), LError);
    auto in_url = cmd_main["in"];
    auto out_url = cmd_main["out"];
    auto rtp_type = cmd_main["rtp"].as<int>();
    auto delay_ms = cmd_main["delay"].as<int>();
    auto pusher_count = cmd_main["count"].as<int>();
    auto merge_ms = cmd_main["merge"].as<int>();
    auto schema = findSubString(out_url.data(), nullptr, "://");
    if (schema != RTSP_SCHEMA && schema != RTMP_SCHEMA) {
        cout << "The push streaming protocol only supports rtsp or rtmp!" << endl;
        return -1;
    }
    const std::string app = "app";
    const std::string stream = "test";

    // Set log
    Logger::Instance().add(std::make_shared<ConsoleChannel>("ConsoleChannel", logLevel));
    // Start asynchronous log thread
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    // Set the number of threads
    EventPollerPool::setPoolSize(threads);
    WorkThreadPool::setPoolSize(threads);

    // Set merge write
    mINI::Instance()[General::kMergeWriteMS] = merge_ms;

    ProtocolOption option;
    option.enable_hls = false;
    option.enable_mp4 = false;
    option.enable_mkv = false;
    MediaSource::Ptr src = nullptr;
    PlayerProxy::Ptr proxy = nullptr;;

    auto tuple = MediaTuple { DEFAULT_VHOST, app, stream, "" };
    if (end_with(in_url, ".mkv")) {
        // create MediaSource from mkvfile
        auto reader = std::make_shared<MKVReader>(tuple, in_url);
        //mkv repeat
        reader->startReadMKV(0, true, true);
        src = MediaSource::find(schema, DEFAULT_VHOST, app, stream, false);
        if (!src) {
            // mkv file does not exist
            WarnL << "no such file or directory: " << in_url;
            return -1;
        }
    } else {
        // Add pull stream proxy
        proxy = std::make_shared<PlayerProxy>(tuple, option);
        // rtsp pull stream proxy method
        (*proxy)[Client::kRtpType] = rtp_type;
        // Start pull stream proxy
        proxy->play(in_url);

    }

    auto get_src = [schema,app,stream]() {
        return MediaSource::find(schema, DEFAULT_VHOST, app, stream, false);
    };

    // Streamer map
    recursive_mutex mtx;
    unordered_map<void *, MediaPusher::Ptr> pusher_map;


    auto add_pusher = [&](const MediaSource::Ptr &src, const string &rand_str, size_t index) {
        auto pusher = std::make_shared<MediaPusher>(src);
        auto tag = pusher.get();
        pusher->setOnCreateSocket([](const EventPoller::Ptr &poller) {
            // Socket close mutex, improve performance
            return Socket::createSocket(poller, false);
        });
        // Set push stream failure listener
        pusher->setOnPublished([&mtx, &pusher_map, tag](const SockException &ex) {
            if (ex) {
                // Push stream failed, remove it
                lock_guard<recursive_mutex> lck(mtx);
                pusher_map.erase(tag);
            }
        });
        // Set push stream disconnection listener
        pusher->setOnShutdown([&mtx, &pusher_map, tag](const SockException &ex) {
            // Push stream failed halfway, remove it
            lock_guard<recursive_mutex> lck(mtx);
            pusher_map.erase(tag);
        });
        // Set rtsp push stream method (effective when rtsp push stream)
        (*pusher)[Client::kRtpType] = rtp_type;
        // Initiate push stream request, each push stream end has a different stream_id
        string url = StrPrinter << out_url << "_" << rand_str << "_" << index;
        pusher->publish(url);

        // Keep the object from being destroyed
        lock_guard<recursive_mutex> lck(mtx);
        pusher_map.emplace(tag, std::move(pusher));

        // Sleep and then start the next push stream to prevent massive connections in a short time
        if (delay_ms > 0) {
            usleep(1000 * delay_ms);
        }
    };

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
        InfoL << "Number of online stream pushers:" << alive_pusher;
        auto src = get_src();
        for(size_t i = 0; i < pusher_count - alive_pusher && src && !exit_flag; ++i){
            // Some push streamers failed, so we retry adding
            add_pusher(get_src(), makeRandStr(8), i);
        }
    }

    return 0;
}

#endif
