# WebRTC instructions for use

## WebRTC architecture

### 1. SFU pattern architecture (WHIP/WHEP)

SFU mode relays media streams through a server, supporting multiplexing and transcoding:

```
                    WebRTC SFU 模式 (WHIP/WHEP)
                         
     Push end (WHIP)                                     Pull end (WHEP)
   +----------------+                                 +-----------------+
   |   Encoder      |                                 |   Player        |
   |  (Browser/S3M) |                                 |  (Browser/S3M)  |
   +----------------+                                 +-----------------+
          |                                               |
          | WHIP Protocol                                 | WHEP Protocol
          | (WebRTC ingest)                               | (WebRTC playback)
          |                                               |
          v                                               v
    +-------------------------------------------------------------------+
    |                          S3MediaKit Server                        |
    +-------------------------------------------------------------------+
    - WHIP: WebRTC-HTTP Ingestion Protocol (Push streaming)
    - WHEP: WebRTC-HTTP Egress Protocol (Pull flow)
```

### 2. P2P model architecture

P2P mode allows direct connections between clients, reducing server load:
Custom signaling protocol based on Websocket

```
                    WebRTt WC P2P model
                         
    client A                                      client B
   +------------+                                +-------------+
   | Browser/S3M|                                | Browser/S3M |
   +------------+                                +-------------+
        |                                               |
        | 1. Signaling exchange (SDP Offer/Answer)      |
        | 2. ICE Candidate exchange                     |
        +-----------------------+-----------------------+
        |                       |                       |
        |        +------------------------------+       |
        |        | S3MediaKit Server            |       |
        |        | Signaling Server (WebSocket) |       |
        |        | STUN server                  |       |
        |        | TURN server                  |       |
        |        +------------------------------+       |
        |                                               |
        +-----------------------------------------------+
                    Direct P2P connection
```

## HTTP API interface

### 1. WebRTC room management

#### `/index/api/addWebrtcRoomKeeper`
Add WebRTC to the specified signaling server to maintain room connections in the signaling server.

**Request parameters:**
-`secret`: interface access key
-`server_host`: Signaling server host address
-`server_port`: signaling server port
-`room_id`: Room ID, the signaling server will check the uniqueness of the ID

#### `/index/api/delWebrtcRoomKeeper`  
Delete the specified signaling server.

**Request parameters:**
-`secret`: interface access key
-`room_key`: unique identifier of the room keeper

#### `/index/api/listWebrtcRoomKeepers`
List all signaling servers.

**Request parameters:**
-`secret`: interface access key

### 2. WebRTC room session management

#### `/index/api/listWebrtcRooms`
List all active WebRTC Peer session information.

**Request parameters:**
-`secret`: interface access key

### 3. WebRTC push and pull interfaces

S3MediaKit supports creating WebRTC push and pull streams through the standard stream proxy interface, and supports two signaling modes:

##### `/index/api/addStreamProxy` -WebRTC streaming proxy

WebRTC streaming proxy can be created through this interface, supporting two signaling protocol modes.

**Request parameters:**
-`secret`: interface access key
-`vhost`: virtual host name, default is `__defaultVhost__`
-`app`: application name
-`stream`: stream ID
-`url`: WebRTC source URL, supports two formats

**WebRTC URL 格式:**

1. **WHIP/WHEP mode (SFU)**-Standard HTTP signaling protocol:
   ```
   #HTTP
   webrtc://server_host:server_port/app/stream_id?signaling_protocols=0
   
   # HTTPS (not yet implemented)
   webrtcs://server_host:server_port/app/stream_id?signaling_protocols=0
   ```

2. **WebSocket P2P Mode**-Custom signaling protocol:
   ```
   #WebSocket
   webrtc://signaling_server_host:signaling_server_port/app/stream_id?signaling_protocols=1&peer_room_id=target_room_id
   
   # WebSocket Secure (not yet implemented)
   webrtcs://signaling_server_host:signaling_server_port/app/stream_id?signaling_protocols=1&peer_room_id=target_room_id
   ```

**Request example:**
```bash
# WHIP/WHEP mode streaming
curl -X POST "http://127.0.0.1/index/api/addStreamProxy" \
  -d "secret=your_secret" \
  -d "vhost=__defaultVhost__" \
  -d "app=live" \
  -d "stream=test" \
  -d "url=webrtc://source.server.com:80/live/source_stream?signaling_protocols=0"

# P2P mode streaming
curl -X POST "http://127.0.0.1/index/api/addStreamProxy" \
  -d "secret=your_secret" \
  -d "vhost=__defaultVhost__" \
  -d "app=live" \
  -d "stream=test" \
  -d "url=webrtc://signaling.server.com:3000/live/source_stream??signaling_protocols=1%26peer_room_id=target_room_id"
```

#### `/index/api/addStreamPusherProxy` -WebRTC push proxy (not yet implemented)

Through this interface, you can create a WebRTC push proxy to push existing streams to the WebRTC target server.

**Request parameters:**
-`secret`: interface access key
-`schema`: source stream protocol (such as: rtmp, rtsp, hls, etc.)
-`vhost`: virtual host name
-`app`: application name
-`stream`: source stream ID
-`dst_url`: WebRTC target push URL

**WebRTC push URL format:**

1. **WHIP Mode (SFU)**-Push to a server that supports WHIP:
   ```
   #HTTP
   webrtc://target_server:port/app/stream_id?signaling_protocols=0
   
   # HTTPS (not yet implemented)
   webrtcs://target_server:port/app/stream_id?signaling_protocols=0
   ```

2. **WebSocket P2P mode**-Push to P2P room
   ```
   # WebSocket 
   webrtc://signaling_server:port/app/stream_id?signaling_protocols=1&peer_room_id=target_room
   # WebSocket Secure
   webrtcs://signaling_server:port/app/stream_id?signaling_protocols=1&peer_room_id=target_room
   ```

**Request example:**
```bash
# Push RTSP stream to WHIP server
curl -X POST "http://127.0.0.1/index/api/addStreamPusherProxy" \
  -d "secret=your_secret" \
  -d "schema=rtsp" \
  -d "vhost=__defaultVhost__" \
  -d "app=live" \
  -d "stream=test" \
  -d "dst_url=webrtc://target.server.com:80/live/target_stream?signaling_protocols=0"

# Push RTSP stream to P2P room
curl -X POST "http://127.0.0.1/index/api/addStreamPusherProxy" \
  -d "secret=your_secret" \
  -d "schema=rtsp" \
  -d "vhost=__defaultVhost__" \
  -d "app=live" \
  -d "stream=test" \
  -d "dst_url=webrtc://signaling.server.com:3000/live/room_stream?signaling_protocols=1%26peer_room_id=target_room_id"
```

#### URL parameter description

-`signaling_protocols`: signaling protocol type
  -`0`: WHIP/WHEP mode (default)
    -**Protocol**: Standard WebRTC signaling protocol based on HTTP
    -**Application Scenario**: SFU (Selective Forwarding Unit) mode, suitable for broadcast and multi-person conferences
  -`1`: WebSocket P2P mode
    -**Protocol**: Custom signaling protocol based on WebSocket
    -**Application Scenario**: Point-to-point direct connection, suitable for low-latency calls and private communications
-`peer_room_id`: Target room ID in P2P mode (only required in P2P mode)

### 4. WebRTC proxy player information query

#### `/index/api/getWebrtcProxyPlayerInfo`
Get the connection information and status of the WebRTC proxy player.

**Request parameters:**
-`secret`: interface access key
-`key`: proxy player identifier

## WebRTC related configuration items

In the `[rtc]` configuration section in `config.ini`:

``` ini
[rtc]
#webrtc Signaling server port
signalingPort=3000
#STUN/TURN server port
icePort=3478
#Whether the TURN service is enabled on the STUN/TURN port
enableTurn=1

#TURN service allocation port pool
portRange=50000-65000

#ICE transmission policy: 0=no restriction (default), 1=only supports Relay forwarding, 2=only supports P2P direct connection
iceTransportPolicy=0

#STUN/TURN Service Ice password
iceUfrag=S3MediaKit
icePwd=S3MediaKit
```

## Examples
- [S3m_peerconnection](https://gitee.com/libwebrtc_develop/libwebrtc/tree/feature-S3m/examples/S3m_peerconnection)
A simple example of s3m p2p proxy streaming based on libwebrtc

## Notes

1. **Firewall Configuration**: Make sure the WebRTC related ports are open
  - Signaling port: 3000 (default)
  - STUN/TURN port: 3478 (default)
  - TURN Alloc port range: 50000-65000 (default)

## Functions not yet implemented:
- Security verification of Webrtc signaling service
- Customize the configuration of external STUN/TURN servers
- webrtc proxy push stream