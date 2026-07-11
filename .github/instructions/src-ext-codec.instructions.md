---
description: "Use when writing code in src/ or ext-codec/: adding a new codec plugin, implementing RTP/RTMP packetization, creating Track or Frame subclasses, using the broadcast/event system, adding config keys, or working with MediaSource/MediaSink. Covers Factory registration, CodecPlugin, CODEC_MAP, RtpCodec, RtmpCodec, Track::clone, GET_CONFIG, NOTICE_EMIT, and CHECK macros."
applyTo: "{src,ext-codec}/**"
---

# src/ & ext-codec/ Conventions

All code uses `namespace mediakit`. Standard includes at file top:
```cpp
using namespace std; using namespace toolkit;
```

## Adding a New Codec — Checklist

A complete codec implementation spans `ext-codec/` and touches `src/Extension/Frame.h`.

### 1. Register the codec ID

Append to `CODEC_MAP` in [src/Extension/Frame.h](../../src/Extension/Frame.h):
```cpp
XX(CodecMyCodec, TrackVideo/*or Audio*/, <next_int>, "MyCodec", PSI_STREAM_RESERVED, MOV_OBJECT_NONE)
```
The integer value must be unique and sequential. `CODEC_MAP` drives `CodecId` enum, SDP string lookup, and MOV/MPEG mapping.

### 2. Create the Track class (`ext-codec/MyCodec.h/.cpp`)

```cpp
class MyCodecTrack : public VideoTrack { // or AudioTrack
public:
    using Ptr = std::shared_ptr<MyCodecTrack>;
    CodecId getCodecId() const override { return CodecMyCodec; }
    bool ready() const override;              // true when config data is available
    Track::Ptr clone() const override;        // ONLY copy codec metadata — never copy ring/dispatcher
    Sdp::Ptr getSdp(uint8_t pt) const override;
    toolkit::Buffer::Ptr getExtraData() const override;   // for MP4/RTMP config
    void setExtraData(const uint8_t *, size_t) override;  // parse incoming config
    bool inputFrame(const Frame::Ptr &) override;         // split & dispatch sub-frames
};
```

**Critical:** `clone()` must NOT copy `_ring`, listener lists, or any `FrameDispatcher` state. Only copy codec parameters (`_sps`, `_width`, etc.).

### 3. Create the Frame type

```cpp
// Zero-copy variant (references parent buffer):
using MyCodecFrame = MyCodecFrameHelper<FrameImp>;
using MyCodecFrameNoCacheAble = MyCodecFrameHelper<FrameFromPtr>;
```
Override `keyFrame()`, `configFrame()`, `dropAble()`, `decodeAble()` based on NAL/header type.

### 4. RTP packetizer/depacketizer (`ext-codec/MyCodecRtp.h/.cpp`)

```cpp
class MyCodecRtpDecoder : public RtpCodec {
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos) override;
    // → parse, reassemble, dispatch Frame via FrameDispatcher::inputFrame()
};

class MyCodecRtpEncoder : public RtpCodec {
    bool inputFrame(const Frame::Ptr &frame) override;
    // → packetize into RtpPacket, call RtpInfo helpers, write via _ring->write()
};
```
If RTP packetization is trivial, use `CommonRtpEncoder` / `CommonRtpDecoder(CodecMyCodec, max_size)` instead of implementing from scratch.

### 5. RTMP codec (`ext-codec/MyCodecRtmp.h/.cpp`) — if needed

```cpp
class MyCodecRtmpDecoder : public RtmpCodec {
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;
};
class MyCodecRtmpEncoder : public RtmpCodec {
    bool inputFrame(const Frame::Ptr &frame) override;
    void flush() override;             // output any buffered frames
    void makeConfigPacket() override;  // emit codec config RTMP packet
};
```

### 6. Define the plugin and register it

At the bottom of `ext-codec/MyCodec.cpp`, inside an anonymous namespace:
```cpp
namespace {
CodecId getCodec() { return CodecMyCodec; }
Track::Ptr getTrackByCodecId(int, int, int) { return std::make_shared<MyCodecTrack>(); }
Track::Ptr getTrackBySdp(const SdpTrack::Ptr &t) { /* parse fmtp */ return std::make_shared<MyCodecTrack>(); }
RtpCodec::Ptr getRtpEncoderByCodecId(uint8_t pt) { return std::make_shared<MyCodecRtpEncoder>(); }
RtpCodec::Ptr getRtpDecoderByCodecId() { return std::make_shared<MyCodecRtpDecoder>(); }
RtmpCodec::Ptr getRtmpEncoderByTrack(const Track::Ptr &t) { return std::make_shared<MyCodecRtmpEncoder>(t); }
RtmpCodec::Ptr getRtmpDecoderByTrack(const Track::Ptr &t) { return std::make_shared<MyCodecRtmpDecoder>(t); }
Frame::Ptr getFrameFromPtr(const char *d, size_t len, uint64_t dts, uint64_t pts) {
    return std::make_shared<MyCodecFrame>((char *)d, len, dts, pts, 0);
}
} // namespace
CodecPlugin mycodec_plugin = { getCodec, getTrackByCodecId, getTrackBySdp,
    getRtpEncoderByCodecId, getRtpDecoderByCodecId,
    getRtmpEncoderByTrack, getRtmpDecoderByTrack, getFrameFromPtr };
```

In `src/Extension/Factory.cpp`, add:
```cpp
REGISTER_CODEC(mycodec_plugin);
```

## Config Keys (src/ namespace style)

Config namespaces live in `src/Common/config.h` (`General`, `Protocol`, `Rtp`, `Record`, `Hls`, `RtpProxy`). For new keys in `src/`, add them to the appropriate namespace there. For keys in `server/` or `manager/` source files, declare them locally (see their respective instructions).

Read a config value:
```cpp
GET_CONFIG(int, myVar, MyNs::kMyKey);  // caches and auto-converts
```

For live-reload support:
```cpp
LISTEN_RELOAD_KEY(myVar, MyNs::kMyKey, [&]() { /* act on change */ });
```

## Broadcast Events

Events are declared as `extern const std::string kBroadcastXxx` + a `#define BroadcastXxxArgs` macro in `src/Common/config.h`. **Do not define new broadcast events inside `src/` source files** — declare them in `config.h`/`config.cpp`.

```cpp
// Emit
auto has_listeners = NOTICE_EMIT(BroadcastXxxArgs, Broadcast::kBroadcastXxx, arg1, arg2, invoker);
if (!has_listeners) { /* no one listening — handle the fallback */ }

// Listen
NoticeCenter::Instance().addListener(tag, Broadcast::kBroadcastXxx, [](BroadcastXxxArgs) { ... });
```

Invoker-style events (auth callbacks, seek, etc.) pass a lambda as the last argument; the listener must call it to continue the flow.

## MediaSink / MediaSource Patterns

- `MediaSink::inputFrame()` does **not** dispatch until `Track::ready()` returns true. Do not expect immediate output from frames submitted before config frames (SPS/PPS/AAC-CFG) arrive.
- `MultiMediaSourceMuxer` drives all protocol outputs simultaneously; never bypass it to push frames directly into protocol-specific muxers.
- Subclass `MediaSourceEvent` (or `MediaSourceEventInterceptor` for pass-through) to override only specific events without re-implementing all virtuals.
- `Track::clone()` is used by `MediaSink::addTrack()` — it clones metadata to set up the downstream sink. Copying dispatcher state in `clone()` corrupts the data flow.

## Error / Assertion Guards

```cpp
CHECK(condition);          // throws AssertFailedException with file/line — use for invariants
CHECK_RET(condition);      // logs WarnL and returns void — use for recoverable checks in void functions
```

Never use bare `assert()` or `abort()` in library code.
