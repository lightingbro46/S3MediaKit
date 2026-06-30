#include "WebApi.h"
#include "WebApiErrCode.h"

#include "Manager.h"
#include "Server/GlobalMonitor.h"
#include "Local/StorageManager.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

void registerMonitorApis() {
    // Register the Web API monitor endpoints here
    api_regist("/media/mserver/systemStatistic", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        // string id = allArgs["mediaServerId"];
        // GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        // if (id != mediaServerId) {
        //     RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
        //     return;
        // }
        val["data"] = GlobalMonitor::Instance().makeSystemStatisticJson();
    });

    api_regist("/media/mserver/systemStatisticHistory", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("from", "to", "limit");
        int64_t from_ts = allArgs["from"];
        int64_t to_ts   = allArgs["to"];
        int     limit   = allArgs["limit"];
        int     bucket_sec = allArgs["bucket_sec"];
        val["data"] = GlobalMonitor::Instance().getSystemStatisticHistory(from_ts, to_ts, limit, bucket_sec);
    });

    api_regist("/media/mserver/storage/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        val["data"] = StorageManager::Instance().makeSystemStorageJson();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/device/storage", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }
        val["data"] = makeDeviceStoragesJson();
        invoker(200, headerOut, val.toStyledString());
    });

    api_regist("/media/mserver/reader/available", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(LIVE_VIEW_PERMISSION_CODE, PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("deviceId");
        string device_id = allArgs["deviceId"];

        auto on_access = [allArgs, val, invoker, headerOut, device_id]() mutable {

            auto flag_on_mserver = GlobalMonitor::Instance().isReaderCountAvailable();
            if (!flag_on_mserver) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STREAM_READER_ON_MSERVER_LIMITED, "Stream reader is limited due to too many readers on media server");
                return;
            }

            auto flag_per_camera = GlobalMonitor::Instance().isReaderCountAvailable(device_id);
            if (!flag_per_camera) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_STREAM_READER_PER_CAMERA_LIMITED, "Stream reader is limited due to too many readers on camera");
                return;
            }
            // Stream reader is available
            invoker(200, headerOut, val.toStyledString());
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, on_access);
    });

    api_regist("/media/mserver/device/statistics", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("id");

        string id = allArgs["id"];
        auto device = findDeviceSource(id);
        if (!device) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
            return;
        }

        val["data"] = makeDeviceStatisticJson(device);
        invoker(200, headerOut, val.toStyledString());
    });

    DebugL << "Monitor APIs registered";
}

} // namespace managerkit