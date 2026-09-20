/**
 * WebApiMonitor.h
 *
 * Bodies shared by the two monitoring endpoint families:
 *
 *   /media/mserver/systemStatistic*   behind CHECK_AUTH_TOKEN() + CHECK_USER_PERMISSION  (WebApiMonitor.cpp)
 *   /media/api/systemStatistic*       behind CHECK_CLUSTER_AUTHOR_ASYNC                  (WebApi.cpp)
 *
 * Same split as WebApiStorage.h: only the body is shared, each family keeps its own guard and its
 * own registration because the two have different trust boundaries. Outside the storage family
 * these are the only two routes the two families have in common, and their payloads were already
 * identical — sharing the body keeps them that way.
 */

#ifndef S3MEDIAKIT_WEBAPI_MONITOR_H
#define S3MEDIAKIT_WEBAPI_MONITOR_H

#include "json/json.h"
#include "WebApi.h"

namespace managerkit {

// Cached monitor snapshot (OS, CPU, RAM, network, disks, readers): no blocking work, so both
// families answer it on the request poller.
void systemStatistic(Json::Value &val);

// Statistics DB query over a time range: runs on the API worker pool.
void systemStatisticHistory(const ArgsMap &allArgs, Json::Value &val);

} // namespace managerkit

#endif // S3MEDIAKIT_WEBAPI_MONITOR_H
