#ifndef S3MEDIAKIT_MANAGERHOOK_H
#define S3MEDIAKIT_MANAGERHOOK_H

#include <string>
#include <functional>
#include "json/json.h"

namespace xGeneral {
extern const std::string kMaxResourceWaitTimeMS;
extern const std::string kBroadcastResourceCountChanged;
extern const std::string kResourceNoneReaderDelayMS;
} // namespace xGeneral

namespace xBroadcast {
// Register or unregister Resource event broadcast
extern const std::string kBroadcastResourceChanged;
#define BroadcastResourceChangedArgs const bool &bRegist, Resource &sender
// This event will be broadcast after the resource is not found. Please pull the resource or other methods to generate the resource after listening to this event, so that you can pull the resource on demand.
extern const std::string kBroadcastNotFoundResource;
#define BroadcastNotFoundResourceArgs const ResourceInfo &args, SockInfo &sender, const std::function<void()> &closePlayer
// broadcast resoruce viewer count changes
extern const std::string kBroadcastResourceCountChanged;
#define BroadcastResourceCountChangedArgs const ResourceTuple& args, const int& count
// Triggered when a resource is not consumed by anyone. The purpose is to achieve business logic such as actively disconnecting the pull resource when no one is watching.
extern const std::string kBroadcastResourceNoneReader;
#define BroadcastResourceNoneReaderArgs Resource &sender
} // namespace xBroadcast

void installManagerHook();
void unInstallManagerHook();

void addCameraResource(Json::Value &data, const std::function<void(const std::string &camera_id, const std::string &stream_id, const std::string &url)> &cb);
void delCameraResource(const std::string &resource_id, const std::function<void(const std::string &camera_id, const std::string &stream_id)> &cb);

// void addServerResource();
// void delCameraResource();

// void addResource(std::string type, );
// void delResource
#endif // S3MEDIAKIT_MANAGERHOOK_H
