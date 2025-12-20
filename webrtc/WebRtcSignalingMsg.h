#ifndef S3MEDIAKIT_WEBRTC_SIGNALING_MSG_H
#define S3MEDIAKIT_WEBRTC_SIGNALING_MSG_H

#include "server/WebApi.h"

namespace mediakit {
namespace Rtc {

#define SIGNALING_MSG_ARGS const HttpAllArgs<Json::Value>& allArgs

// WebRTC Signaling message key name and value constant declaration
extern const char* const CLASS_KEY;
extern const char* const CLASS_VALUE_REQUEST;
extern const char* const CLASS_VALUE_INDICATION;        // Indication type, no response required
extern const char* const CLASS_VALUE_ACCEPT;            // As a response to CLASS_VALUE_REQUEST
extern const char* const CLASS_VALUE_REJECT;            // As a response to CLASS_VALUE_REQUEST
extern const char* const METHOD_KEY;
extern const char* const METHOD_VALUE_REGISTER;         // register
extern const char* const METHOD_VALUE_UNREGISTER;       // unregister
extern const char* const METHOD_VALUE_CALL;             // call(Fetch or push streams)

extern const char* const METHOD_VALUE_BYE;              // hang up
extern const char* const METHOD_VALUE_CANDIDATE;
extern const char* const TRANSACTION_ID_KEY;            // Message id, each message has a unique id
extern const char* const ROOM_ID_KEY;
extern const char* const GUEST_ID_KEY;                 // Each independent session has a unique guest_id
extern const char* const SENDER_KEY;
extern const char* const TYPE_KEY;
extern const char* const TYPE_VALUE_PLAY;              // play stream
extern const char* const TYPE_VALUE_PUSH;              // push stream
extern const char* const REASON_KEY;
extern const char* const CALL_VHOST_KEY;
extern const char* const CALL_APP_KEY;
extern const char* const CALL_STREAM_KEY;
extern const char* const SDP_KEY;

extern const char* const ICE_SERVERS_KEY;
extern const char* const CANDIDATE_KEY;
extern const char* const URL_KEY;
extern const char* const UFRAG_KEY;
extern const char* const PWD_KEY;

} // namespace Rtc
} // namespace mediakit
//

#endif //S3MEDIAKIT_WEBRTC_SIGNALING_PEER_H
