#include <set>
#include "ApiWorkDispatcher.h"
#include "WebApi.h"
#include "WebApiErrCode.h"
#include "WebApiMonitor.h"

#include "Manager.h"
#include "Server/GlobalMonitor.h"
#include "Server/HealthMonitor.h"
#include "Server/AlertRegistry.h"
#include "Local/StorageManager.h"
#ifdef ENABLE_TIER_STORAGE
#include "Local/TierStorageManager.h"
#endif
#if defined(ENABLE_VERSION)
#include "S3MVersion.h"
#endif

using namespace std;
using namespace toolkit;
using namespace mediakit;

namespace managerkit {

// --- Bodies shared with the /media/api/* family (WebApiMonitor.h) ----------------------------

void systemStatistic(Json::Value &val) {
    val["data"] = GlobalMonitor::Instance().makeSystemStatisticJson();
    // Recording rate into the record directory, or into the HOT tier when storage tiering is on.
#ifdef ENABLE_TIER_STORAGE
    val["data"]["record_write"] = TierStorageManager::Instance().makeHotWriteJson();
#else
    val["data"]["record_write"] = StorageManager::Instance().makeRecordWriteJson();
#endif
}

void systemStatisticHistory(const ArgsMap &allArgs, Json::Value &val) {
    int64_t from_ts = allArgs["from"];
    int64_t to_ts   = allArgs["to"];
    int     limit   = allArgs["limit"];
    int     bucket_sec = allArgs["bucket_sec"];
    val["data"] = GlobalMonitor::Instance().getSystemStatisticHistory(from_ts, to_ts, limit, bucket_sec);
}

// --- /media/monitor/* (health monitor for the desktop monitor app) ---------------------------
// Every route sits behind CHECK_SECRET() (api.secret + http.allow_ip_range), reads and config
// alike; no JWT. See docs/api-health-monitor.md.

static bool apiSecretIsDefault() {
    GET_CONFIG(string, api_secret, API::kSecret);
    return api_secret == API::kDefaultSecret;
}

static Json::Value versionJson() {
    Json::Value ver(Json::objectValue);
#if defined(ENABLE_VERSION)
    ver["buildTime"] = BUILD_TIME;
    ver["branchName"] = BRANCH_NAME;
    ver["commitHash"] = COMMIT_HASH;
#endif
    return ver;
}

// data of a disabled monitor: overall unknown, app shows "monitor off" instead of an error.
static Json::Value disabledHealthJson() {
    GET_CONFIG(string, media_server_id, General::kMediaServerId);
    Json::Value data;
    data["schemaVersion"] = 1;
    data["mediaServerId"] = media_server_id;
    data["enabled"] = false;
    data["overall"] = "unknown";
    return data;
}

// Snapshot part of /health and /summary. `?rev=` equal to the current one answers notModified
// (levels and reasons unchanged; /summary values still move, so it only applies to /health).
static void monitorSnapshotResponse(const ArgsMap &allArgs, Json::Value &val, bool summary) {
    auto snap = HealthMonitor::Instance().snapshot();
    if (!snap) {
        val["data"] = disabledHealthJson();
        val["data"]["apiSecretIsDefault"] = apiSecretIsDefault();
        return;
    }
    if (!summary) {
        string rev = allArgs["rev"];
        if (!rev.empty() && rev == to_string(snap->rev)) {
            val["notModified"] = true;
            return;
        }
    }
    val["data"] = summary ? snap->summary : snap->health;
    val["data"]["enabled"] = true;
    val["data"]["version"] = versionJson();
    val["data"]["apiSecretIsDefault"] = apiSecretIsDefault();
}

static string iniValue(const string &key) {
    auto &ini = mINI::Instance();
    auto it = ini.find(key);
    return it == ini.end() ? string() : (const string &)it->second;
}

static Json::Value monitorConfigJson() {
    Json::Value data;
    data["schemaVersion"] = 1;
    Json::Value &values = data["values"];
    values = Json::objectValue;
    for (const auto &key : HealthMonitorConfig::editableKeys()) {
        values[key] = iniValue(key);
    }
    // Thresholds in force: the VMS server config also sets CPU/RAM/disk (kept until its next
    // push) and the reader limits come from the license, so these can differ from `values`.
    auto &gm = GlobalMonitor::Instance();
    auto put = [&](const char *name, ResourceType type) {
        auto pr = gm.getThreshold(type);
        data["effective"][name]["warn"] = pr.first;
        data["effective"][name]["crit"] = pr.second;
    };
    put("cpu", ResourceType::CPU);
    put("ram", ResourceType::MEMORY);
    put("disk", ResourceType::HDD);
    put("reader", ResourceType::READER);
    data["restartRequired"] = Json::arrayValue;
    data["restartRequired"].append(HealthMonitorConfig::kEnable);
    data["restartRequired"].append(HealthMonitorConfig::kIntervalSec);
    data["apiSecretIsDefault"] = apiSecretIsDefault();
    return data;
}

static void registerHealthMonitorApis() {
    if (apiSecretIsDefault()) {
        WarnL << "api.secret still has its built-in value: /media/monitor/* and every other api.secret "
                 "endpoint accept it, change it in the config file";
    }

    api_regist("/media/monitor/health", [](API_ARGS_MAP) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        monitorSnapshotResponse(allArgs, val, false);
    });

    api_regist("/media/monitor/summary", [](API_ARGS_MAP) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        monitorSnapshotResponse(allArgs, val, true);
    });

    // Cameras with their quality metrics, most severe first. ?health=warning,critical (default),
    // or "all"; ?page= (from 1) and ?size= (default 50, at most 500).
    api_regist("/media/monitor/cameras", [](API_ARGS_MAP) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        auto snap = HealthMonitor::Instance().snapshot();
        if (!snap) {
            val["data"] = disabledHealthJson();
            return;
        }
        string health = allArgs["health"];
        if (health.empty()) {
            health = "warning,critical";
        }
        set<string> wanted;
        for (auto &h : split(health, ",")) {
            wanted.insert(trim(h));
        }
        const bool all = wanted.count("all") > 0;
        int page = std::max(1, atoi(allArgs["page"].data()));
        int size = atoi(allArgs["size"].data());
        size = size <= 0 ? 50 : std::min(size, 500);

        Json::Value list(Json::arrayValue);
        size_t matched = 0;
        const size_t first = (size_t)(page - 1) * size;
        for (const auto &cam : snap->cameras) {
            if (!all && !wanted.count(cam["health"].asString())) {
                continue;
            }
            if (matched >= first && list.size() < (Json::ArrayIndex)size) {
                list.append(cam);
            }
            ++matched;
        }
        GET_CONFIG(string, media_server_id, General::kMediaServerId);
        Json::Value &data = val["data"];
        data["schemaVersion"] = 1;
        data["mediaServerId"] = media_server_id;
        data["generatedAt"] = (Json::Int64)snap->generated_at;
        data["rev"] = (Json::UInt64)snap->rev;
        data["total"] = (Json::UInt64)matched;
        data["page"] = page;
        data["size"] = size;
        data["cameras"] = list;
    });

    // Open alerts + events after `since` (the `seq` of the previous answer): the app polls with it to
    // get only the new events.
    api_regist("/media/monitor/alerts", [](API_ARGS_MAP) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        string since = allArgs["since"];
        uint64_t since_seq = since.empty() ? 0 : strtoull(since.c_str(), nullptr, 10);
        GET_CONFIG(string, media_server_id, General::kMediaServerId);
        val["data"] = AlertRegistry::Instance().toJson(since_seq);
        val["data"]["schemaVersion"] = 1;
        val["data"]["mediaServerId"] = media_server_id;
        val["data"]["generatedAt"] = (Json::Int64)time(nullptr);
    });

    // Health history for charts: ?from=&to= (unix seconds, default the last 24 h, at most 31 days),
    // ?bucket_sec= (wider ranges are bucketed to ~2000 points), ?fields= (point fields and "system").
    api_regist("/media/monitor/history", [](API_ARGS_MAP_ASYNC) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        if (!HealthMonitor::Instance().running()) {
            val["data"] = disabledHealthJson();
            invoker(200, headerOut, val.toStyledString());
            return;
        }
        const int64_t now = (int64_t)time(nullptr);
        int64_t to = allArgs["to"].empty() ? now : (int64_t)allArgs["to"];
        int64_t from = allArgs["from"].empty() ? to - 86400 : (int64_t)allArgs["from"];
        int bucket_sec = allArgs["bucket_sec"];
        if (from <= 0 || to <= from || to - from > 31 * 86400 || bucket_sec < 0) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "from/to must be unix seconds, from < to, at most 31 days");
        }
        string fields = allArgs["fields"];
        dispatchApiWork(sender, headerOut, val, invoker, [from, to, bucket_sec, fields](Json::Value &val) {
            val["data"] = HealthMonitor::Instance().history(from, to, bucket_sec, fields);
        }, nullptr, apiWorkLimit("storage.read"));
    });

    api_regist("/media/monitor/config/get", [](API_ARGS_MAP) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        val["data"] = monitorConfigJson();
    });

    api_regist("/media/monitor/config/set", [](API_ARGS_MAP_ASYNC) {
        GET_CONFIG(string, api_secret, API::kSecret);
        CHECK_SECRET();
        if (allArgs.getParser().method() != "POST") {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "POST only");
            return;
        }
        map<string, string> changes;
        for (const auto &pr : allArgs.args) {
            if (pr.first != "secret") {
                changes[pr.first] = pr.second;
            }
        }
        if (changes.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, "no key to change");
            return;
        }
        auto err = HealthMonitorConfig::validateChanges(changes, iniValue);
        if (!err.empty()) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_INVALID_ARGS, err);
            return;
        }

        // Apply on the request poller like /index/api/setServerConfig (mINI and the reload
        // broadcast are not touched from worker threads); only the file write is dispatched.
        auto &ini = mINI::Instance();
        int changed = 0;
        for (const auto &kv : changes) {
            if (iniValue(kv.first) != kv.second) {
                ini[kv.first] = kv.second;
                ++changed;
            }
        }
        if (changed > 0) {
            NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
            // Same path as the VMS server config: the CPU/RAM/disk monitors take the new pair now.
            auto apply = [&](ResourceType type, const string &warn_key, const string &crit_key) {
                if (changes.count(warn_key) || changes.count(crit_key)) {
                    GlobalMonitor::Instance().setThreshold(type, ini[warn_key].as<float>(), ini[crit_key].as<float>());
                }
            };
            apply(ResourceType::CPU, GlobalMonitorConfig::kCpuWarningThreshold, GlobalMonitorConfig::kCpuCriticalThreshold);
            apply(ResourceType::MEMORY, GlobalMonitorConfig::kMemoryWarningThreshold, GlobalMonitorConfig::kMemoryCriticalThreshold);
            apply(ResourceType::HDD, GlobalMonitorConfig::kHddWarningThreshold, GlobalMonitorConfig::kHddCriticalThreshold);
        }
        val["changed"] = changed;
        if (changed == 0) {
            val["data"] = monitorConfigJson();
            invoker(200, headerOut, val.toStyledString());
            return;
        }
        auto ini_copy = std::make_shared<mINI>(ini);
        dispatchApiWork(sender, headerOut, val, invoker, [ini_copy](Json::Value &val) {
            ini_copy->dumpFile(g_ini_file);
        }, [](Json::Value &val) {
            val["data"] = monitorConfigJson();
        }, apiWorkSerial("monitor.config"));
    });
}

void registerMonitorApis() {
    registerHealthMonitorApis();

    // Register the Web API monitor endpoints here
    api_regist("/media/mserver/systemStatistic", [](API_ARGS_MAP) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);

        systemStatistic(val);
    });

    api_regist("/media/mserver/systemStatisticHistory", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("from", "to", "limit");

        // Blocking part (statistics DB) runs on the API worker.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            systemStatisticHistory(allArgs, val);
        });
    });

    api_regist("/media/mserver/storage/list", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("mediaServerId");

        // Blocking part (statistics DB, disk usage, per-camera storage) runs on the API worker.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string id = allArgs["mediaServerId"];
            GET_CONFIG(string, mediaServerId, General::kMediaServerId)
            if (id != mediaServerId) {
                throw ApiResponseException(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            }
            val["data"] = StorageManager::Instance().makeSystemStorageJson();
        });
    });

    api_regist("/media/mserver/device/storage", [](API_ARGS_MAP_ASYNC) {
        CHECK_AUTH_TOKEN();
        CHECK_USER_PERMISSION(READ_MSERVER_PERMISSION_CODE);
        CHECK_ARGS_("mediaServerId");

        // Blocking part (statistics DB, disk usage, per-camera storage) runs on the API worker.
        dispatchApiWork(sender, headerOut, val, invoker, [allArgs](Json::Value &val) {
            string id = allArgs["mediaServerId"];
            GET_CONFIG(string, mediaServerId, General::kMediaServerId)
            if (id != mediaServerId) {
                throw ApiResponseException(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            }
            val["data"] = makeDeviceStoragesJson();
        });
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