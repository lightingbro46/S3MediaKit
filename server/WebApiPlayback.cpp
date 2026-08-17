#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Util/logger.h"

#include "Common/config.h"
#include "Manager.h"
#include "Local/SearchEngine.h"
#include "Util/base64.h"
#include "Util/MD5.h"
#include <sys/stat.h>

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

void registerPlaybackApis() {
    // Register the Web API playback endpoints here
    api_regist("/media/esc/recordedTimePeriod", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("cameraId", "startTime", "endTime", "periodType", "detail");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            uint64_t start_time = allArgs["startTime"];
            uint64_t end_time = allArgs["endTime"];
            int period_type = allArgs["periodType"];
            int detail = allArgs["detail"];
            bool include_motion = allArgs["motion"];
            string jwt_token = allArgs["_jwt_token"];
            bool edge = allArgs["edge"];

            if (!start_time) {
                start_time = time(nullptr) - 24 * 3600;
            }

            if (!end_time) {
                end_time = time(nullptr);
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, "", "" };
            SearchEngine::findTimePeriod(tuple, start_time, end_time, period_type, detail, include_motion, jwt_token, edge, [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    RETURN_API_RESPONSE(ex.getCustomCode(), ex.what());
                } else {
                    val["data"] = data;
                    InfoL << "Get recorded time period success";
                    invoker(200, headerOut, val.toStyledString());
                }
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });

    static auto responseSnap = [](const string &snap_path,
                                  const HttpSession::KeyValue &headerIn,
                                  const HttpSession::HttpResponseInvoker &invoker,
                                  const string &err_msg = "") {
        static bool s_snap_success_once = false; // Kiểm tra xem lần đầu chụp có thành công ko
        StrCaseMap headerOut;
        if (!File::fileSize(snap_path)) {

            // TH file size == 0 && lần đầu chụp && file có mã lỗi
            if (!err_msg.empty() && !s_snap_success_once){
#if 0
                // If the screenshot has never been successful or the default screenshot image is empty, then directly return the FFmpeg error log
                headerOut["Content-Type"] = HttpFileManager::getContentType(".txt");
                invoker.responseFile(headerIn, headerOut, err_msg, false, false);
#endif
                Value val;
                val["code"] = ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY;
                val["msg"] = err_msg;
                int status_code = getStatusCode(ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY);
                invoker(status_code, headerOut, val.toStyledString());
                return;
            }

            //TH fize size == 0: Lần thứ > 1 chụp || ko có mã lỗi
            Value val;
            val["code"] = ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY;
            val["msg"] = err_msg;
            int status_code = getStatusCode(ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY);
            invoker(status_code, headerOut, val.toStyledString());
            return;
            
        } else {
            s_snap_success_once = true;
            // The previously generated screenshot file, we default to jpeg format
            headerOut["Content-Type"] = HttpFileManager::getContentType(".jpeg");
        }
        // Return image to http client
        invoker.responseFile(headerIn, headerOut, snap_path);
    };

    // Get screenshot cache or real-time screenshot
    api_regist("/media/esc/recordedThumnail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("cameraId", "pos");

        auto on_access = [allArgs, val, invoker, headerOut]() mutable {
            string camera_id = allArgs["cameraId"];
            string stream_id = allArgs["streamId"];
            string pos_str = allArgs["pos"];
            bool edge = allArgs["edge"];

            // Forward to the node that recorded this camera at the requested time.
            if (!edge) {
                std::pair<string, string> owner;
                if (pos_str == "latest") {
                    owner = SearchEngine::findCurrentOwnerNode(camera_id);
                } else {
                    owner = SearchEngine::findOwnerNodeAtTime(camera_id, (int64_t)stoll(pos_str));
                }
                if (!owner.second.empty()) {
                    string jwt_token = allArgs["_jwt_token"];
                    NOTICE_EMIT(BroadcastSyncThumbnailArgs, Broadcast::kBroadcastSyncThumbnail, owner.second, camera_id, stream_id, pos_str, jwt_token, invoker);
                    return;
                }
            }

            MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
            TimeQuery::Ptr query;
            try {
                query = std::make_shared<TimeQuery>(tuple);
            } catch(...) {}

            string src_path;
            uint64_t pos_time = 0;
            uint64_t diff_time = 0;
            if (query) {
                if (pos_str == "latest" || pos_str == "0") {
                    // todo: get latest jpeg record
                    auto ret = findDeviceSource(tuple.app, GENERIC_RTSP_CAMERA_SCHEMA);
                    if (ret) {
                        auto weak_listener = ret->getListener();
                        if (auto listener = weak_listener.lock()) {
                            auto ptr = dynamic_pointer_cast<GenericRtspCameraImp>(listener);
                            if (ptr) {
                                auto stats_imp = ptr->getCameraStatisticImp();
                                if (stats_imp) {
                                    auto params = stats_imp->getParams();
                                    uint64_t last_archived_time = 0;
                                    if (!tuple.stream.empty() && params.storage_map.find(tuple.stream) != params.storage_map.end()) {
                                        last_archived_time = params.storage_map[tuple.stream].archiveEndTime;
                                        
                                    } else if (tuple.stream.empty()) {
                                        for (const auto &pr : params.storage_map) {
                                            if (pr.second.archiveEndTime > last_archived_time) {
                                                last_archived_time = pr.second.archiveEndTime;
                                            }
                                        }
                                    }
                                    if (last_archived_time > 0) {
                                        auto block = query->getLastBlock(last_archived_time);
                                        if (block) {
                                            pos_time = block->start_time() + block->time_len() - 10;
                                            diff_time = pos_time - block->start_time();
                                            src_path = decodeBase64(block->file_path());
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    pos_time = stoll(pos_str);
                    auto start_time = pos_time - 60;
                    auto end_time = pos_time + 60;
                    query->getRecordedTimePeriod(start_time, end_time, [&pos_time, &src_path, &diff_time](const vector<TimeBlock> &blocks) {
                        for (const auto &block : blocks) {
                            if (block.start_time() > pos_time) {
                                break;
                            }
                            diff_time = pos_time - block.start_time();
                            src_path = decodeBase64(block.file_path());
                        }
                    });
                }
            }

            if (src_path.empty() || pos_time == 0) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_TIMELINE_NOT_FOUND, "No data in period");
                return;
            }

            GET_CONFIG(string, snap_root, API::kSnapRoot);
            int expire_sec = 60;

            auto path = camera_id + "/" + stream_id;
            auto scan_path = File::absolutePath(path, snap_root) + "/";
            string filter_complex;
            string policy_key;
            vector<string> overlay_temp_paths;
            const string overlay_prefix = StrPrinter << scan_path << pos_time << ".overlay";
            if (!FFmpegOverlayFilter::buildThumbnailOverlay(tuple, allArgs["_jwt_token"], overlay_prefix,
                                                            filter_complex, overlay_temp_paths, policy_key)) {
                for (const auto &temp_path : overlay_temp_paths) {
                    File::delete_file(temp_path);
                }
                RETURN_API_RESPONSE(ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY, "Unable to render thumbnail overlay");
                return;
            }

            const string cache_key = MD5(policy_key).hexdigest();
            string new_snap = StrPrinter << scan_path << pos_time << "." << cache_key << ".jpeg";
            if (File::fileExist(new_snap) && File::fileSize(new_snap) > 0) {
                for (const auto &temp_path : overlay_temp_paths) {
                    File::delete_file(temp_path);
                }
                responseSnap(new_snap, allArgs.parser.getHeader(), invoker);
                return;
            }
            File::delete_file(new_snap);
            auto cache_file = File::create_file(new_snap, "wb");
            if (cache_file) {
                fclose(cache_file);
            }

            // Start the FFmpeg process, start taking screenshots, generate temporary files, replace them with formal files after successful screenshots
            auto new_snap_tmp = new_snap + ".tmp";
            auto on_snap = [invoker, allArgs, new_snap, new_snap_tmp, overlay_temp_paths](bool success, const string &err_msg) {
                if (!success) {
                    // Screenshot generation failed, there may be residual empty files
                    File::delete_file(new_snap_tmp);
                } else {
                    // Temporary file changed to formal file
                    File::delete_file(new_snap);
                    rename(new_snap_tmp.data(), new_snap.data());
                }
                for (const auto &temp_path : overlay_temp_paths) {
                    File::delete_file(temp_path);
                }
                responseSnap(new_snap, allArgs.parser.getHeader(), invoker, err_msg);
            };
            if (filter_complex.empty()) {
                FFmpegSnap::makeSnap(false, src_path, new_snap_tmp, diff_time, 2, on_snap);
            } else {
                FFmpegSnap::makeSnapWithFilter(src_path, new_snap_tmp, diff_time, 10, filter_complex, on_snap);
            }
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["cameraId"], on_access);
    });
    
    static auto findPlaybackStream = [](MediaSource::Ptr &ret, const string &url_in) {
        MediaInfo info_in;
        info_in.parse(url_in);
        string url = StrPrinter << "/" << info_in.app << "/" << info_in.stream;

        string url_prefix = "/media";
        string ts_suffix = ".live.ts";
        string flv_suffix = ".live.flv";
        string fmp4_suffix = ".live.mp4"; 
        string fmp4_merged_suffix = ".live2.mp4"; 
        auto prefix_size = url_prefix.size();
        if (prefix_size > 0) {
            if (url.size() < prefix_size || strncasecmp(url.data(), url_prefix.data(), prefix_size)) {
                // Prefix not found
                return false;
            }
            // Remove special prefix from url
            url.erase(0, prefix_size);
        }
        string schema;
        if (end_with(url, fmp4_suffix)) {
            schema = FMP4_SCHEMA;
            url.erase(url.size() - fmp4_suffix.size());
        } else if (end_with(url, fmp4_merged_suffix)) {
            schema = FMP4_SCHEMA;
            url.erase(url.size() - fmp4_merged_suffix.size());
        } else if (end_with(url, ts_suffix)) {
            schema = TS_SCHEMA;
            url.erase(url.size() - ts_suffix.size());
        } else if (end_with(url, flv_suffix)) {
            schema = RTMP_SCHEMA;
            url.erase(url.size() - flv_suffix.size());
        } else {
            // Suffix not found
            return false;
        }

        MediaInfo media_info(schema + "://" + DEFAULT_VHOST + url);
        if (media_info.app.empty() || media_info.stream.empty()) {
            // URL is invalid
            return false;
        }

        ret = MediaSource::find(media_info.schema, media_info.vhost, media_info.app, media_info.stream);
        return ret != nullptr;
    };

    api_regist("/media/mserver/playback/speed", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("url", "speed");
        string url = allArgs["url"];

        MediaSource::Ptr src;
        if (!findPlaybackStream(src, url)) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_STREAM_NOT_FOUND, "Playback stream not found");
            return;
        }
        auto tuple = src->getMediaTuple();
        auto stream = tuple.stream;
        string device_id = split(stream, "/").front();

        auto on_access = [allArgs, val, invoker, headerOut, src]() mutable {
            auto speed = allArgs["speed"].as<float>();
            src->getOwnerPoller()->async([=]() mutable {
                bool flag = src->speed(speed);
                val["code"] = flag ? ApiErrCode::CODE_SUCCESS : ApiErrCode::CODE_OTHER_EXCEPTION;
                val["msg"] = flag ? "Success" : "Failed";
                val["result"] = flag ? 0 : -1;                                                                                                                                                                                                                                                                                                
                invoker(200, headerOut, val.toStyledString());
            });
        };

        CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, on_access);
    });

    DebugL << "Playback APIs registered";
}

} // namespace managerkit