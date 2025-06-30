#include <cmath>
#include <ctime>
#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Record/Recorder.h"
#include "Manager.h"
#include "Local/TimeRecorder.h"
#include "Local/TimeQuery.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

// void addCameraResource(Json::Value &device, const function<void(const string &camera_id, const string &stream_id, const string &url)> &cb) {
//     string camera_id = device["device_id"].asString();
//     string username = device["username"].asString();
//     string password = device["password"].asString();
//     string ip = device["address"].asString();

//     // todo: check config and compare with old config if exist

//     if (device.isMember("streams") && device["streams"].isArray()) {
//         for (const auto &str: device["streams"]) {
//             string stream_id = str["channel_id"].asString();
//             string url = str["source_url"].asString();
//             WarnL << url;
//             cb(camera_id, stream_id, url);
//         }
//     }
// }

// void delCameraResource(const string &id) {

// }

static void *manager_hook_tag = nullptr;

void installManagerHook () {

#ifdef ENABLE_MP4
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        DebugL << "Record mp4 file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(std::round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MP4

#ifdef ENABLE_MKV
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastRecordMKV, [](BroadcastRecordMKVArgs) {
        TraceL << "Record mkv file " << info.app << " " << info.stream << " " << info.start_time << " " << info.time_len << " " << info.file_path;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(std::round(info.time_len));
        block.set_file_size(info.file_size);
        block.set_file_path(info.file_path);

        TimeRecorder::Instance().inputBlock(block);
    });
#endif // ENABLE_MKV

#if defined(ENABLE_MP4) || defined(ENABLE_MKV)
    NoticeCenter::Instance().addListener(&manager_hook_tag, Broadcast::kBroadcastMediaSeeked, [](BroadcastMediaSeekedArgs) {
        auto infos = split(args.stream, "/");
        MediaTuple tuple = { args.vhost, infos[0], infos[1], "" };
        auto query = std::make_shared<TimeQuery>(tuple);
        int64_t offset = query->getOffsetOfDate(stamp);
        invoker(offset);
    });
#endif // defined(ENABLE_MP4) || defined(ENABLE_MKV)

}

void unInstallManagerHook() {
    NoticeCenter::Instance().delListener(&manager_hook_tag);
}