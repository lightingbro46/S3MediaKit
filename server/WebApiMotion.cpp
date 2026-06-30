#include "WebApi.h"
#include "WebApiErrCode.h"

#include "Common/config.h"
#include "Util/MD5.h"
#include "Manager.h"
#include "Local/SearchEngine.h"


using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

void registerMotionDetectionApis() {
    // Register the Web API motion endpoints here
    api_regist("/media/esc/searchMotion", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("cameraId", "startTime", "endTime", "roiMask");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            uint64_t start_time = allArgs["startTime"];
            uint64_t end_time = allArgs["endTime"];
            string roi_mask = allArgs["roiMask"];
            bool edge = allArgs["edge"];

            if (!start_time) {
                start_time = time(nullptr) - 24 * 3600;
            }

            if (!end_time) {
                end_time = time(nullptr);
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, "", "" };
            SearchEngine::findMotionPeriodByRoi(tuple, start_time, end_time, roi_mask, edge, [&](const SockException &ex, const Value &data) {
                if (ex) {
                    RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                } else {
                    val["data"] = data;
                    InfoL << "Search motion time by ROI success";
                    invoker(200, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    DebugL << "Motion APIs registered";
}

} // namespace managerkit