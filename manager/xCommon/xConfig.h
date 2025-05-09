#ifndef COMMON_XCONFIG_H
#define COMMON_XCONFIG_H

#include "Util/NoticeCenter.h"
#include "Util/mini.h"
#include "Util/onceToken.h"
#include <functional>

namespace managerkit {

class ResourceOption;

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

namespace xGeneral {
// Trigger kBroadcastResourceNoneReader event only after the resource has been unwatched for a certain period of time
// Default to trigger kBroadcastResourceNoneReader event after 5 seconds of no viewers
extern const std::string kResourceNoneReaderDelayMS;
// Resource registration timeout, after receiving the player's request, if the related resource is not found, the server will wait for a certain period of time,
// If the related stream is registered within this time, the server will immediately respond to the player that the playback is successful,
// Otherwise, it will wait for a maximum of kMaxResourceWaitTimeMS milliseconds and then respond to the player that the playback failed
extern const std::string kMaxResourceWaitTimeMS;
// Enable resource viewer count change event broadcast, set to 1 to enable, set to 0 to disable
extern const std::string kBroadcastResourceCountChanged;
} // namespace xGeneral 

//////////Resource Server Configuration///////////
namespace xServerOption {

} // namespace xServerOption

//////////Resource Camera Configuration///////////
namespace xCameraOption {

} // namespace xCameraOption

//////////Resource Local Configuration///////////
namespace xLocalOption {

} // namespace xLocalOption

//////////Resource Storage Configuration///////////
namespace xStorageOption {

} // namespace xStorageOption

//////////Resource User Configuration///////////
namespace xUserOption {

} // namespace xUserOption

/**
 * Resource player related settings name,
 * These settings are not used in the configuration file
 * Only used to set a specific player instance
 */
namespace xClient {
    
} // namespace xClient

} // namespace managerkit

#endif // COMMON_XCONFIG_H
