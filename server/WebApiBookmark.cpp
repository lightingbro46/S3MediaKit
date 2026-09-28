#include "ApiWorkDispatcher.h"
#include "WebApi.h"
#include "WebApiErrCode.h"
#include "Common/config.h"
#include "Local/SearchEngine.h"
#include "Storage/Bookmark.h"
#include "Manager.h"
#include "Util/base64.h"
#include "Util/MD5.h"
#include "Http/HttpClient.h"
#ifdef ENABLE_TIER_STORAGE
#include "Local/TierStorageManager.h"
#endif
#include <limits>

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

namespace {

using SearchCallback = function<void(const SockException &ex, const Value &data)>;

// Runs a SearchEngine query on the API worker (index/detail SQL, remote peers answer later) and
// calls `respond` with its result on the request poller.
void runSearch(SockInfo &sender, const HttpSession::HttpResponseInvoker &invoker,
               const function<void(const SearchCallback &cb)> &query, const SearchCallback &respond) {
    runApiCallbackWork(apiRequestPoller(sender), apiRequestSession(sender), invoker,
        [query, respond](const ApiReply &reply) {
            query([reply, respond](const SockException &ex, const Value &data) {
                reply([respond, ex, data]() { respond(ex, data); });
            });
        });
}

// Owner lookup of a bookmark/camera on the API worker; `next(owner)` then runs on the request
// poller with the original forward/authorize flow.
void lookupOwner(const EventPoller::Ptr &poller, const weak_ptr<SocketHelper> &session,
                 const HttpSession::HttpResponseInvoker &invoker, bool edge,
                 const function<pair<string, string>()> &lookup,
                 const function<void(const pair<string, string> &owner)> &next) {
    auto owner = std::make_shared<pair<string, string>>();
    runApiWork(poller, session,
        [owner, edge, lookup]() {
            if (!edge) {
                *owner = lookup();
            }
        },
        [owner, next, invoker, session](const exception_ptr &error) {
            if (error) {
                responseApiException(error, invoker, session);
                return;
            }
            next(*owner);
        });
}

} // namespace

void registerBookmarkApis() {
    // Register the Web API bookmark endpoints here
    api_regist("/media/esc/bookmark/search", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("start_time", "end_time", "page", "size", "sort");

        string camera_id   = allArgs["camera_id"];
        int64_t start_time = allArgs["start_time"];
        int64_t end_time   = allArgs["end_time"];
        string search      = allArgs["search"];
        int page           = MAX((int)allArgs["page"], 0);
        int size           = MAX((int)allArgs["size"], 1);
        string sort        = allArgs["sort"];
        string user_id     = allArgs["_user_id"];
        string jwt_token   = allArgs["_jwt_token"];
        bool edge          = allArgs["edge"];

        runSearch(sender, invoker,
            [=](const SearchCallback &cb) {
                SearchEngine::findBookmarks(camera_id, start_time, end_time, search, user_id,
                                            page, size, sort, jwt_token, edge, cb);
            },
            [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    val["code"] = -1;
                    val["msg"]  = ex.what();
                    invoker(500, headerOut, val.toStyledString());
                    return;
                }
                val["data"]        = data["data"];
                val["currentPage"] = data["currentPage"];
                val["totalItems"]  = data["totalItems"];
                val["totalPages"]  = data["totalPages"];
                val["partial"]     = data["partial"];
                invoker(200, headerOut, val.toStyledString());
            });
    });

    // Internal endpoint: fetch full bookmark detail for a comma-separated list of
    // bookmark GUIDs stored on this node.  Called by remote aggregators via
    // SearchEngine::findBookmarks(); should not be exposed to external clients.
    api_regist("/media/esc/bookmark/detail", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_ARGS_("ids");

        string ids_str = allArgs["ids"];
        auto   guids   = toolkit::split(ids_str, ",");
        bool   edge      = allArgs["edge"];

        runSearch(sender, invoker,
            [guids, edge](const SearchCallback &cb) { SearchEngine::getBookmarkDetail(guids, edge, cb); },
            [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    val["code"] = -1;
                    val["msg"]  = ex.what();
                    invoker(500, headerOut, val.toStyledString());
                    return;
                }
                val["data"] = data["data"];
                invoker(200, headerOut, val.toStyledString());
            });
    });

    api_regist("/media/esc/bookmark/create", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("name", "camera_id", "start_time", "duration");

        string camera_id = allArgs["camera_id"];
        bool edge        = allArgs["edge"];
        auto poller      = apiRequestPoller(sender);
        auto session     = apiRequestSession(sender);

        lookupOwner(poller, session, invoker, edge,
            [camera_id]() { return SearchEngine::findCurrentOwnerNode(camera_id); },
            [allArgs, val, invoker, headerOut, enable_authorize, camera_id, poller, session](const pair<string, string> &owner) mutable {
            if (!owner.second.empty()) {
                // Forward to the camera owner node if it is not this node.
                HttpArgs fwd_body;
                fwd_body["name"]        = (string)allArgs["name"];
                fwd_body["description"] = (string)allArgs["description"];
                fwd_body["camera_id"]   = (string)allArgs["camera_id"];
                fwd_body["start_time"]  = (string)allArgs["start_time"];
                fwd_body["end_time"]    = (string)allArgs["end_time"];
                fwd_body["duration"]    = (string)allArgs["duration"];
                fwd_body["tags"]        = (string)allArgs["tags"];
                string fwd_token = allArgs["_jwt_token"];

                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_CREATE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkCreateOrUpdateArgs, Broadcast::kBroadcastSyncBookmarkCreateOrUpdate, owner.second, fwd_body, fwd_token, on_response, true);
                return;
            }

            auto on_access = [allArgs, val, invoker, headerOut, poller, session]() mutable {
                runApiCallbackWork(poller, session, invoker, [allArgs, val, invoker, headerOut](const ApiReply &reply) mutable {
                    string name = allArgs["name"];
                    string description = allArgs["description"];
                    string camera_id = allArgs["camera_id"];
                    int64_t start_time = allArgs["start_time"];
                    int64_t end_time = allArgs["end_time"];
                    int64_t duration = allArgs["duration"];
                    string tags = allArgs["tags"];
                    string user_id = allArgs["_user_id"];

                    auto ret = findDeviceSource(camera_id);
                    if (!ret) {
                        reply([val, invoker, headerOut]() mutable {
                            val["data"]["flag"] = false;
                            RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Camera not found");
                        });
                        return;
                    }

                    Bookmark bm;
                    bm.name = name;
                    bm.description = description;
                    bm.camera_guid = camera_id;
                    bm.start_time = start_time;
                    bm.end_time = end_time;
                    bm.duration = duration;
                    bm.creator_guid = user_id;
                    bm.created = time(nullptr);

                    auto imp = std::make_shared<BookmarkImp>();
                    imp->add(bm, tags);

                    reply([val, invoker, headerOut]() mutable {
                        val["data"]["flag"] = true;
                        invoker(200, headerOut, val.toStyledString());
                    });
                });
            };

            CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_id, on_access);
        });
    });

    api_regist("/media/esc/bookmark/update", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id", "camera_id", "start_time", "duration");

        string bookmark_id = allArgs["id"];
        bool edge          = allArgs["edge"];
        auto poller        = apiRequestPoller(sender);
        auto session       = apiRequestSession(sender);

        lookupOwner(poller, session, invoker, edge,
            [bookmark_id]() { return SearchEngine::findOwnerNodeForBookmark(bookmark_id); },
            [allArgs, val, invoker, headerOut, enable_authorize, bookmark_id, poller, session](const pair<string, string> &owner) mutable {
            if (!owner.second.empty()) {
                // Forward to the bookmark's owner node if it is not this node.
                HttpArgs fwd_body;
                fwd_body["id"]          = bookmark_id;
                fwd_body["name"]        = (string)allArgs["name"];
                fwd_body["description"] = (string)allArgs["description"];
                fwd_body["camera_id"]   = (string)allArgs["camera_id"];
                fwd_body["start_time"]  = (string)allArgs["start_time"];
                fwd_body["end_time"]    = (string)allArgs["end_time"];
                fwd_body["duration"]    = (string)allArgs["duration"];
                fwd_body["tags"]        = (string)allArgs["tags"];
                string fwd_token        = allArgs["_jwt_token"];

                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_UPDATE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkCreateOrUpdateArgs, Broadcast::kBroadcastSyncBookmarkCreateOrUpdate, owner.second, fwd_body, fwd_token, on_response, false);
                return;
            }

            auto on_access = [allArgs, val, invoker, headerOut, poller, session]() mutable {
                runApiCallbackWork(poller, session, invoker, [allArgs, val, invoker, headerOut](const ApiReply &reply) mutable {
                    string id = allArgs["id"];
                    string name = allArgs["name"];
                    string description = allArgs["description"];
                    string camera_id = allArgs["camera_id"];
                    int64_t start_time = allArgs["start_time"];
                    int64_t end_time = allArgs["end_time"];
                    int64_t duration = allArgs["duration"];
                    string tags = allArgs["tags"];
                    string user_id = allArgs["_user_id"];

                    auto imp = std::make_shared<BookmarkImp>();
                    auto ret = imp->findById(id);
                    if (!ret.size()) {
                        WarnL << "Bookmark " << id << " not found";
                        reply([val, invoker, headerOut]() mutable {
                            val["data"]["flag"] = false;
                            RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
                        });
                        return;
                    }

                    Bookmark bm = ret[0];
                    bm.name = name;
                    bm.description = description;
                    bm.camera_guid = camera_id;
                    bm.start_time = start_time;
                    bm.end_time = end_time;
                    bm.duration = duration;
                    bm.creator_guid = user_id;
                    bm.created = time(nullptr);

                    imp->update(bm, tags);
                    reply([val, invoker, headerOut]() mutable {
                        val["data"]["flag"] = true;
                        invoker(200, headerOut, val.toStyledString());
                    });
                });
            };

            CHECK_USER_DEVICE_AUTHOR_ASYNC(allArgs["camera_id"], on_access);
        });
    });

    api_regist("/media/esc/bookmark/delete", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string id    = allArgs["id"];
        bool edge    = allArgs["edge"];
        auto poller  = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);

        // Owner lookup and, for a local bookmark, the camera it belongs to (for the auth check).
        struct Lookup {
            pair<string, string> owner;
            bool found = false;
            string camera_guid;
        };
        auto lookup = std::make_shared<Lookup>();
        runApiWork(poller, session,
            [lookup, id, edge]() {
                if (!edge) {
                    lookup->owner = SearchEngine::findOwnerNodeForBookmark(id);
                    if (!lookup->owner.second.empty()) {
                        return;
                    }
                }
                auto bm_ret = std::make_shared<BookmarkImp>()->findById(id);
                if (!bm_ret.empty()) {
                    lookup->found = true;
                    lookup->camera_guid = bm_ret[0].camera_guid;
                }
            },
            [allArgs, val, invoker, headerOut, enable_authorize, id, lookup, poller, session](const exception_ptr &error) mutable {
            if (error) {
                responseApiException(error, invoker, session);
                return;
            }
            if (!lookup->owner.second.empty()) {
                // Forward to the bookmark's owner node if it is not this node.
                string fwd_token = allArgs["_jwt_token"];
                Broadcast::OnResInvoker on_response = [val, invoker, headerOut](const string &err, const int&, const Json::Value &res) mutable {
                    if (!err.empty()) {
                        RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_DELETE_FAILED, err);
                        return;
                    }
                    invoker(200, headerOut, res.toStyledString());
                };
                NOTICE_EMIT(BroadcastSyncBookmarkDeleteArgs, Broadcast::kBroadcastSyncBookmarkDelete, lookup->owner.second, id, fwd_token, on_response);
                return;
            }

            if (!lookup->found) {
                WarnL << "Bookmark " << id << " not found";
                val["data"]["flag"] = false;
                RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
                return;
            }

            string camera_guid = lookup->camera_guid;
            auto on_access = [val, invoker, headerOut, id, poller, session]() mutable {
                dispatchApiWork(poller, session, headerOut, val, invoker, [id](Value &val) {
                    std::make_shared<BookmarkImp>()->remove(id);
                    val["data"]["flag"] = true;
                });
            };

            CHECK_USER_DEVICE_AUTHOR_ASYNC(camera_guid, on_access);
        });
    });

    api_regist("/media/esc/bookmark/mostUsedTags", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("size");

        int size = allArgs["size"];
        if (size < 0) size = 1;
        
        dispatchApiWork(sender, headerOut, val, invoker, [size](Value &val) {
            auto imp = std::make_shared<BookmarkTagCountImp>();
            auto tags = imp->findTagsByCountDesc(size);

            val["data"] = arrayValue;
            for (const auto &tag : tags) {
                val["data"].append(tag);
            }
        });
    });

    api_regist("/media/esc/bookmark/recent", [](API_ARGS_MAP_ASYNC) { 
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("size", "sort"); 

        string camera_id = allArgs["camera_id"];
        int size = MAX((int)allArgs["size"], 1);
        string sort = allArgs["sort"];
        string user_id = allArgs["_user_id"];
        string jwt_token = allArgs["_jwt_token"];
        bool edge = allArgs["edge"];
        
        runSearch(sender, invoker,
            [=](const SearchCallback &cb) {
                SearchEngine::findRecentBookmarks(camera_id, user_id, size, sort, jwt_token, edge, cb);
            },
            [val, invoker, headerOut](const SockException &ex, const Value &data) mutable {
                if (ex) {
                    val["code"] = -1;
                    val["msg"]  = ex.what();
                    invoker(500, headerOut, val.toStyledString());
                    return;
                }
                val["data"]        = data["data"];
                val["currentPage"] = data["currentPage"];
                val["totalItems"]  = data["totalItems"];
                val["totalPages"]  = data["totalPages"];
                val["partial"]     = data["partial"];
                invoker(200, headerOut, val.toStyledString());
            });
    });

    api_regist("/media/esc/bookmark/recordThumbnail", [](API_ARGS_MAP_ASYNC) { 
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(PLAYBACK_PERMISSION_CODE);
        CHECK_ARGS_("id");

        string bookmark_id = allArgs["id"];
        bool edge = allArgs["edge"];

        auto poller  = apiRequestPoller(sender);
        auto session = apiRequestSession(sender);

        // Owner lookup and the local bookmark are read on the API worker.
        struct Lookup {
            pair<string, string> owner;
            vector<Bookmark> found;
        };
        auto lookup = std::make_shared<Lookup>();
        runApiWork(poller, session,
            [lookup, bookmark_id, edge]() {
                if (!edge) {
                    lookup->owner = SearchEngine::findOwnerNodeForBookmark(bookmark_id);
                    if (!lookup->owner.second.empty()) {
                        return;
                    }
                }
                lookup->found = std::make_shared<BookmarkImp>()->findById(bookmark_id);
            },
            [allArgs, val, invoker, headerOut, enable_authorize, bookmark_id, lookup, poller, session](const exception_ptr &error) mutable {
            if (error) {
                responseApiException(error, invoker, session);
                return;
            }
            if (!lookup->owner.second.empty()) {
                // Forward to the bookmark's owner node if it is not this node.
                string fwd_token = allArgs["_jwt_token"];
                NOTICE_EMIT(BroadcastSyncBookmarkThumbnailArgs, Broadcast::kBroadcastSyncBookmarkThumbnail, lookup->owner.second, bookmark_id, fwd_token, invoker);
                return;
            }

            if (lookup->found.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_BOOKMARK_NOT_FOUND, "Bookmark not found");
                return;
            }
            const Bookmark bm = lookup->found[0];
            string device_id = bm.camera_guid;
            auto on_access = [allArgs, val, invoker, headerOut, bm, poller, session]() mutable {
                // Opening the timefile, rendering the overlay (which writes the SVG mask to disk)
                // and starting the thumbnail task run on the API worker; every answer goes through
                // `reply`, which runs it on the request poller owning the session.
                runApiCallbackWork(poller, session, invoker,
                    [allArgs, val, invoker, headerOut, bm](const ApiReply &reply) mutable {
                    string camera_id = bm.camera_guid;
                    string stream_id = ""; // TODO: support stream id in bookmark
                    uint64_t start_time = (uint64_t)bm.start_time;
                    uint64_t diff_time = 0;

                    MediaTuple tuple = { DEFAULT_VHOST, camera_id, stream_id, "" };
                    TimeQuery::Ptr query;
                    try {
                        query = std::make_shared<TimeQuery>(tuple);
                    } catch(...) {}

                    string src_path;
                    TimeBlock selected_block;
                    bool has_selected_block = false;
                    if (query) {
                        auto block = query->getLastBlock(start_time);
                        if (block) {
                            selected_block = *block;
                            has_selected_block = true;
                            src_path = decodeBase64(block->file_path());
                            diff_time = start_time > block->start_time() ? start_time - block->start_time() : 0;
                        }
                    }

                    if (src_path.empty()) {
                        throw ApiResponseException(ApiErrCode::CODE_TIMELINE_NOT_FOUND, "No data in period");
                    }

                    if (has_selected_block) {
                        stream_id = selected_block.stream();
                        tuple.stream = stream_id;
                    }

                    GET_CONFIG(string, snap_root, API::kSnapRoot);
                    auto scan_path = File::absolutePath(camera_id + "/" + stream_id, snap_root) + "/";
                    string filter_complex;
                    string policy_key;
                    vector<string> overlay_temp_paths;
                    const string overlay_prefix = StrPrinter << scan_path << start_time << ".bookmark.overlay.svg";
                    if (!FFmpegOverlayFilter::buildThumbnailOverlay(tuple, allArgs["_jwt_token"], overlay_prefix,
                                                                    filter_complex, overlay_temp_paths, policy_key)) {
                        for (const auto &temp_path : overlay_temp_paths) {
                            File::delete_file(temp_path);
                        }
                        throw ApiResponseException(ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY, "Unable to render thumbnail overlay");
                    }

                    const string cache_key = MD5(policy_key).hexdigest();
                    string snap_path = StrPrinter << scan_path << start_time << ".bookmark." << cache_key << ".jpeg";
                    if (File::fileExist(snap_path) && File::fileSize(snap_path) > 0) {
                        for (const auto &temp_path : overlay_temp_paths) {
                            File::delete_file(temp_path);
                        }
                        reply([snap_path, allArgs, invoker]() mutable {
                            StrCaseMap response_headers;
                            response_headers["Content-Type"] = HttpFileManager::getContentType(".jpeg");
                            invoker.responseFile(allArgs.parser.getHeader(), response_headers, snap_path);
                        });
                        return;
                    }

                    File::delete_file(snap_path);
                    GET_CONFIG(float, thumbnail_timeout_sec, FFmpeg::kRecordedThumbnailTimeoutSec);
                    RecordedThumbnailTask::Request request;
                    request.tuple = tuple;
                    request.source_path = src_path;
                    request.segment_start_time = static_cast<int64_t>(selected_block.start_time());
                    request.segment_size = selected_block.file_size() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                        ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(selected_block.file_size());
                    request.seek_time = diff_time;
                    request.output_path = snap_path;
                    request.filter_complex = filter_complex;
                    request.temporary_paths = overlay_temp_paths;
                    request.restore_reason = "BOOKMARK_THUMBNAIL";
                    request.timeout_sec = thumbnail_timeout_sec;
                    auto task = std::make_shared<RecordedThumbnailTask>(request,
                        [reply, invoker, allArgs, val, headerOut, snap_path](const RecordedThumbnailTask::Result &result) mutable {
                            reply([result, invoker, allArgs, val, headerOut, snap_path]() mutable {
                                if (result.success) {
                                    StrCaseMap response_headers;
                                    response_headers["Content-Type"] = HttpFileManager::getContentType(".jpeg");
                                    invoker.responseFile(allArgs.parser.getHeader(), response_headers, snap_path);
                                    return;
                                }
                                if (result.timeout) {
                                    val["code"] = ApiErrCode::CODE_EXTRACT_THUMBNAIL_TIMEOUT;
                                    val["msg"] = result.error;
                                    val["data"]["status"] = "TIMEOUT";
                                    val["data"]["stage"] = result.stage;
                                    val["data"]["progressPercent"] = result.progress_percent;
                                    invoker(getStatusCode(ApiErrCode::CODE_EXTRACT_THUMBNAIL_TIMEOUT),
                                            headerOut, val.toStyledString());
                                    return;
                                }
                                const ApiErrCode code = result.storage_unavailable
                                    ? ApiErrCode::CODE_STORAGE_POOL_OFFLINE
                                    : (result.restore_unavailable
                                        ? ApiErrCode::CODE_STORAGE_RESTORE_JOB_CREATION_FAILED
                                        : (result.recording_unavailable
                                            ? ApiErrCode::CODE_TIMELINE_NOT_FOUND
                                            : ApiErrCode::CODE_EXTRACT_THUMBNAIL_EMPTY));
                                val["code"] = code;
                                val["msg"] = result.error;
                                invoker(getStatusCode(code), headerOut, val.toStyledString());
                            });
                        });
                    task->start();
                }, apiWorkLimit("media.thumbnail"));
            };

            CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, on_access);
        });
    });

    DebugL << "Bookmark APIs registered";
}

} // namespace managerkit
