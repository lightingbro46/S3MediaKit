#include "WebApi.h"
#include "WebApiErrCode.h"

#include "Server/GlobalMonitor.h"
#include "Manager.h"
#include "Common/StrUtil.h"
#include "WebHook.h"
#include "User/UserAuditLog.h"
#include "Storage/Certification.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;
using namespace Json;

namespace managerkit {

void registerConfigurationApis() {
    // Register the Web API config endpoints here
    api_regist("/media/mserver/description", [](API_ARGS_MAP) {
        Value info;
        info["mediaServerId"] = mINI::Instance()[General::kMediaServerId];
        info["version"] = kServerName;
        auto osinfo = GlobalMonitor::Instance().getOsInfo();
        info["osInfo"]["platform"] = osinfo.platform;
        info["osInfo"]["variant"] = osinfo.variant;
        info["osInfo"]["variantVerison"] = osinfo.variant_version;
        info["httpPort"] =  static_cast<int>(mINI::Instance()["http.port"]);
        info["httpsPort"] = static_cast<int>(mINI::Instance()["http.sslport"]);
        info["clientUseSsl"] = false;
        info["maxDevice"] =  GlobalMonitor::Instance().estimateMaxAvailableDevice();
        val["data"] = info;
    });

    api_regist("/media/mserver/register", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("mediaServerId", "domain", "ip", "httpPort", "httpsPort", "preferSSL")

        string mediaServerId_ = allArgs["mediaServerId"];
        string apiDomain = allArgs["domain"];
        string apiIp = allArgs["ip"];
        int httpPort = allArgs["httpPort"];
        int httpsPort = allArgs["httpsPort"];
        bool preferSSL = allArgs["preferSSL"];
        string mediaServerDomain = allArgs["mediaServerDomain"];
        string mediaServerCert = allArgs["mediaServerCert"];
        string apiSecret = allArgs["apiSecret"];

        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (mediaServerId != mediaServerId_) {
            val["data"]["changed"] = 0;
            invoker(200, headerOut, val.toStyledString());
            return;
        }

        auto origin_urls = UriUtils::getUriList(apiDomain, apiIp, httpPort, httpsPort, preferSSL);
        string origin_urls_str;
        for (size_t i = 0; i < origin_urls.size(); i++) {
            if (i > 0) origin_urls_str += ",";
            origin_urls_str += origin_urls[i];
        }

        Broadcast::HealthInvoker on_health_check = [allArgs, origin_urls, mediaServerDomain, mediaServerCert, apiSecret, val, headerOut, invoker](const string& err, const int& idx) mutable {
            if (!err.empty()) {
                RETURN_API_RESPONSE(ApiErrCode::CODE_HEALTH_CHECK_API_FAILED, err.data());
                return;
            }

            auto apiUrlTmp = origin_urls[idx];
            int changed = 0;
            bool need_to_restart = false;
            auto &ini = mINI::Instance();

            // get new config and compare to old one of system
            if (ini[Hook::kApiUrl] != apiUrlTmp) {
                ini[Hook::kApiUrl] = apiUrlTmp;
                ++changed;
                need_to_restart = true;
            }

            if (ini[Manager::kApiSecret] != apiSecret) {
                ini[Manager::kApiSecret] = apiSecret;
                ++changed;
            }

            if (!mediaServerDomain.empty() && !mediaServerCert.empty()) {
                if (ini[Manager::kMediaServerDomain] != mediaServerDomain) {
                    ini[Manager::kMediaServerDomain] = mediaServerDomain;
                    ++changed;
                }
                
                {
                    auto imp = std::make_shared<CertificateImp>();
                    if (!imp->certExist(mediaServerDomain, mediaServerCert)) {
                        imp->saveCert(mediaServerDomain, mediaServerCert);
                        ++changed;
                    }
                }
            }

            if (changed > 0) {
                // notify to reload config and dump ini file
                NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
                ini.dumpFile(g_ini_file);
            }

            if (need_to_restart) {
                // notify to restart server
                NOTICE_EMIT(BroadcastSystemAuditLogArgs, Broadcast::kBroadcastSystemAuditLog, SystemAuditLogType::RESTART_CONFIG, string("Restart due to api configuration updating"));
                NOTICE_EMIT(BroadcastRestartServerArgs, Broadcast::kBroadcastRestartServer);
            }

            val["data"]["changed"] = changed;
            invoker(200, headerOut, val.toStyledString());
        };
        
        auto flag = NOTICE_EMIT(BroadcastHealthCheckApiServiceArgs, Broadcast::kBroadcastHealthCheckApiService, origin_urls_str, on_health_check);
        if (!flag) {
            // Nobody to handle health check service, just return failed
            on_health_check("No handler to handle health check api service", -1);
        }
    });

    api_regist("/media/mserver/healthcheck", [](API_ARGS_MAP) {
        val["data"]["mediaServerId"] = mINI::Instance()[General::kMediaServerId];
    });

    api_regist("/media/mserver/incur", [](API_ARGS_MAP_ASYNC) {
        CHECK_ARGS_("mediaServerId");

        string id = allArgs["mediaServerId"];
        GET_CONFIG(string, mediaServerId, General::kMediaServerId)
        if (id != mediaServerId) {
            RETURN_API_RESPONSE(ApiErrCode::CODE_MSERVER_NOT_FOUND, "Media server not found");
            return;
        }

        NOTICE_EMIT(BroadcastReloadApiConfigArgs, Broadcast::kBroadcastReloadApiConfig);
        invoker(200, headerOut, val.toStyledString());
    });

    DebugL << "Config APIs registered";
}

} // namespace managerkit