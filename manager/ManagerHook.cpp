#include <sstream>
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Record/Recorder.h"
#include "Common/Resource.h"
#include "ManagerHook.h"

// #include "Camera/CameraResource.h"

using namespace std;
using namespace Json;
using namespace toolkit;
using namespace mediakit;
using namespace managerkit;

namespace xGeneral {
#define XGENERAL_FIELD "manager."
const string kMaxResourceWaitTimeMS = XGENERAL_FIELD"max_resource_wait_ms";
const string kBroadcastResourceCountChanged = XGENERAL_FIELD "broadcast_resource_count_changed";
const string kResourceNoneReaderDelayMS = XGENERAL_FIELD "resource_none_reader_delay_ms";

static onceToken token([]() {
    mINI::Instance()[kMaxResourceWaitTimeMS] = 15000;
    mINI::Instance()[kBroadcastResourceCountChanged] = 0;
    mINI::Instance()[kResourceNoneReaderDelayMS] = 20000;
});
}

namespace xBroadcast {
const string kBroadcastResourceChanged = "kBroadcastResourceChanged";
const string kBroadcastNotFoundResource = "kBroadcastNotFoundResource";    
const string kBroadcastResourceCountChanged = "kBroadcastResourceCountChanged";    
}

static void *vms_hook_tag = nullptr;

void installManagerHook() {
#ifdef ENABLE_MP4
    // Broadcast after recording the mp4 file successfully
    NoticeCenter::Instance().addListener(&vms_hook_tag, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
        WarnL << "Record mp4 file " << info.start_time << " " << info.time_len << info.url;
        // todo: record timeline
    });
#endif // ENABLE_MP4

    NoticeCenter::Instance().addListener(&vms_hook_tag, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
        WarnL << "kBroadcastMediaPlayed ";
        // todo: check rtsp authen
    });

    NoticeCenter::Instance().addListener(&vms_hook_tag, xBroadcast::kBroadcastResourceChanged, [](BroadcastMediaChangedArgs) {
        WarnL << "kBroadcastMediaChanged ";
        // todo: store db runtime_action
    });

    NoticeCenter::Instance().addListener(&vms_hook_tag, xBroadcast::kBroadcastResourceChanged, [](BroadcastResourceChangedArgs) {
        WarnL << "kBroadcastResourceChanged ";
        // todo: store db runtime_action
    });
};

void unInstallManagerHook() {


};

void addCameraResource(Value &device, const function<void(const string &camera_id, const string &stream_id, const string &url)> &cb) {
    string camera_id = device["id"].asString();
    string username = device["username"].asString();
    string password = device["password"].asString();
    string ip = device["address"].asString();
    int media_port = device["mediaPort"].asInt();

    // todo: check config and compare with old config if exist

    if (device.isMember("streams") && device["streams"].isArray()) {
        for (const auto &str: device["streams"]) {
            string stream_id = str["id"].asString();
            string protocol = str["protocol"].asString();
            string path = str["path"].asString();
            string url = protocol + "://" + username + ":" + password + "@" + ip + ":" + to_string(media_port) + path;

            // string url = str["source_url"].asString();
            WarnL << url;
            cb(camera_id, stream_id, url);
        }
    }
}

void delCameraResource(const string &id) {

}


// void addResource(ResourceTuple &tuple, ResourceOption &option) {
    
// }

// void delResource(ResourceTuple &tuple) {

// }