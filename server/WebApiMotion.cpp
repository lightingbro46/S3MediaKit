#include "ApiWorkDispatcher.h"
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

        auto owner = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);
        auto on_access = [allArgs, val, invoker, headerOut, owner, session]() mutable {
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
            // Timeline/motion files are read on the API worker; the answer is sent from the request poller.
            runApiCallbackWork(owner, session, invoker, [tuple, start_time, end_time, roi_mask, edge, val, headerOut, invoker](const ApiReply &reply) mutable {
                SearchEngine::findMotionPeriodByRoi(tuple, start_time, end_time, roi_mask, edge, [reply, val, headerOut, invoker](const SockException &ex, const Value &data) mutable {
                    reply([ex, data, val, headerOut, invoker]() mutable {
                        if (ex) {
                            RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                        } else {
                            val["data"] = data;
                            InfoL << "Search motion time by ROI success";
                            invoker(200, headerOut, val.toStyledString());
                        }
                    });
                });
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    DebugL << "Motion APIs registered";
}

} // namespace managerkit