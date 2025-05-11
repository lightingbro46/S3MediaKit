#include "Common/xConfig.h"
#include "Resource.h"
#include "Util/logger.h"
#include "Util/util.h"
#include <assert.h>
#include <stdio.h>

using namespace std;
using namespace toolkit;

namespace managerkit {

//////////xBroadcast Name///////////
namespace xBroadcast { 
const string kBroadcastResourceChanged = "kBroadcastResourceChanged";
const string kBroadcastNotFoundResource = "kBroadcastNotFoundResource";    
const string kBroadcastResourceCountChanged = "kBroadcastResourceCountChanged";    
const string kBroadcastResourceNoneReader = "kBroadcastResourceNoneReader";    

}

// xGeneral Configuration Items
namespace xGeneral { 
#define XGENERAL_FIELD "xGeneral."
const string kMaxResourceWaitTimeMS = XGENERAL_FIELD"max_resource_wait_ms";
const string kBroadcastResourceCountChanged = XGENERAL_FIELD "broadcast_resource_count_changed";
const string kResourceNoneReaderDelayMS = XGENERAL_FIELD "resource_none_reader_delay_ms";

static onceToken token([]() {
    mINI::Instance()[kMaxResourceWaitTimeMS] = 15000;
    mINI::Instance()[kBroadcastResourceCountChanged] = 0;
    mINI::Instance()[kResourceNoneReaderDelayMS] = 20000;
});

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

//////////Resource Client Configuration///////////
namespace xClient {
    
} // namespace xClient

} // namespace managerkit
