#include "WebRtcSignalingMsg.h"

namespace mediakit {
namespace Rtc {

// WebRTC Signaling message key name and value constant definition
const char* const CLASS_KEY = "class";
const char* const CLASS_VALUE_REQUEST = "request";
const char* const CLASS_VALUE_INDICATION = "indication";        // Indication type, no response required
const char*const CLASS_VALUE_ACCEPT = "accept";                 //As a response to CLASS_VALUE_REQUEST
const char*const CLASS_VALUE_REJECT = "reject";                 //As a response to CLASS_VALUE_REQUEST
const char* const METHOD_KEY = "method";
const char* const METHOD_VALUE_REGISTER = "register";           // register
const char* const METHOD_VALUE_UNREGISTER = "unregister";       // unregister
const char* const METHOD_VALUE_CALL = "call";                   // call(Fetch or push streams)

const char* const METHOD_VALUE_BYE = "bye";                     // hang up
const char* const METHOD_VALUE_CANDIDATE = "candidate";
const char* const TRANSACTION_ID_KEY = "transaction_id";        // Message id, each message has a unique id
const char* const ROOM_ID_KEY = "room_id";
const char* const GUEST_ID_KEY = "guest_id";                    // Each independent session has a unique guest_id
const char* const SENDER_KEY = "sender";
const char* const TYPE_KEY = "type";
const char* const TYPE_VALUE_PLAY = "play";                     // play stream
const char* const TYPE_VALUE_PUSH = "push";                     // push stream
const char* const REASON_KEY = "reason";
const char* const CALL_VHOST_KEY = "vhost";
const char* const CALL_APP_KEY = "app";
const char* const CALL_STREAM_KEY = "stream";
const char* const SDP_KEY = "sdp";

const char* const ICE_SERVERS_KEY = "ice_servers";
const char* const CANDIDATE_KEY = "candidate";
const char* const URL_KEY = "url";
const char* const UFRAG_KEY = "ufrag";
const char* const PWD_KEY = "pwd";

} // namespace Rtc
} // namespace mediakit
