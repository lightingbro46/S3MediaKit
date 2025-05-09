#ifndef S3MANAGERKIT_XAPI_H
#define S3MANAGERKIT_XAPI_H

#include <unordered_map>
#include "Util/mini.h"
#include "Common/Resource.h"


namespace managerkit {


} // namespace managerkit

void installxApi();
void unInstallxApi();

void handleServerResourceJson(Json::Value);
void getServerStatisticJson(const std::function<void(Json::Value &val)> &cb);

#endif // S3MANAGERKIT_XAPI_H