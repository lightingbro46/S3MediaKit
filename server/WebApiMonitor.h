/**
 * WebApiMonitor.h
 *
 * Bodies of /media/mserver/systemStatistic* (CHECK_AUTH_TOKEN() + CHECK_USER_PERMISSION,
 * WebApiMonitor.cpp). The /media/api/systemStatistic* copies behind CHECK_CLUSTER_AUTHOR_ASYNC were
 * removed with www/console; the desktop monitor app reads /media/monitor/summary and /history.
 */

#ifndef S3MEDIAKIT_WEBAPI_MONITOR_H
#define S3MEDIAKIT_WEBAPI_MONITOR_H

#include "json/json.h"
#include "WebApi.h"

namespace managerkit {

// Cached monitor snapshot (OS, CPU, RAM, network, disks, readers): no blocking work, so both
// it is answered on the request poller.
void systemStatistic(Json::Value &val);

// Statistics DB query over a time range: runs on the API worker pool.
void systemStatisticHistory(const ArgsMap &allArgs, Json::Value &val);

} // namespace managerkit

#endif // S3MEDIAKIT_WEBAPI_MONITOR_H
