#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpSession.h"
#include "Http/HttpRequester.h"
#include "Network/Session.h"
#include "Record/Recorder.h"
#include "ManagerHook.h"
#include "Local/TimePeriodRecorder.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

static void *x_hook_tag = nullptr;

void installxHook() {
    
#ifdef ENABLE_MP4
    // Broadcast after recording the mp4 file successfully
    NoticeCenter::Instance().addListener(&x_hook_tag, Broadcast::kBroadcastRecordMP4, [=](BroadcastRecordMP4Args) {
        WarnL << "Record mp4 file " << info.app << " " << info.start_time << " " << info.time_len << " " << info.url;
        TimeBlock block;
        block.set_app(info.app);
        block.set_stream(info.stream);
        block.set_start_time(info.start_time);
        block.set_time_len(info.time_len);
        block.set_file_path(info.file_path);
        
        TimeBlockWriter::Instance().addBlock(block);
    });
#endif // ENABLE_MP4

};

void addCameraResource(Value &device, const function<void(const string &camera_id, const string &stream_id, const string &url)> &cb) {
    string camera_id = device["device_id"].asString();
    string username = device["username"].asString();
    string password = device["password"].asString();
    string ip = device["address"].asString();

    // todo: check config and compare with old config if exist

    if (device.isMember("streams") && device["streams"].isArray()) {
        for (const auto &str: device["streams"]) {
            string stream_id = str["channel_id"].asString();
            string url = str["source_url"].asString();
            WarnL << url;
            cb(camera_id, stream_id, url);
        }
    }
}

void delCameraResource(const string &id) {

}
