---
name: s3mediakit-media-codec
description: "Use when adding or changing S3MediaKit codecs, tracks, frames, RTP/RTMP packetization, codec plugin registration, MediaSource/MediaSink behavior, muxers, or protocol conversion in src/ and ext-codec/."
---

# S3MediaKit media and codec work

Before editing, trace the whole path: input packet → demuxer → `Frame` → `Track` →
`MediaSink`/dispatcher → `MultiMediaSourceMuxer` → each output protocol. Most codec bugs are
boundary bugs, not codec bugs.

## Where the change belongs

- `src/` — codec-independent pipeline and protocol behavior: `Common/MediaSource.*`,
  `MediaSink.*`, `MultiMediaSourceMuxer.*`, `Stamp.*`, `Extension/{Frame,Track,Factory}.*`,
  and the per-protocol directories (`Rtsp/ Rtmp/ Rtp/ Http/ TS/ FMP4/ Mjpeg/`).
- `ext-codec/` — one plugin per codec: track/frame metadata, config (SPS/PPS/VPS,
  AudioSpecificConfig…) parsing, and RTP/RTMP adapters for that codec only.

If you are writing `if (codec == CodecH265)` in `src/`, the logic probably belongs in the
plugin. If you are re-deriving muxing rules in `ext-codec/`, it probably belongs in `src/`.

## Adding or changing a codec

1. Read `src/Extension/Frame.h`, `src/Extension/Track.h`, `src/Extension/Factory.cpp` and the
   nearest existing plugin end to end.
2. Update the codec map and register through the existing factory mechanism — do not add a
   parallel registry.
3. Implement track metadata and config parsing from the real bitstream; set key-frame,
   config-frame, drop and decode flags according to the actual format, not by guessing.
4. Add RTP/RTMP adapters only when the packetization rules genuinely differ from an existing
   helper (`CommonRtp`/`CommonRtmp` cover the simple cases).
5. `Track::clone()` copies **codec metadata only** — never ring buffers, listeners,
   dispatchers or reader state. This is a recurring source of leaks and crosstalk.
6. Preserve frame ownership and the zero-copy assumptions of `FrameImp`/`Buffer`; do not
   copy a payload just to make a signature convenient.

## Pipeline rules

- Never bypass `MultiMediaSourceMuxer` to push into one protocol's source directly — other
  outputs, recording and hooks all hang off the muxer.
- `MediaSink` may withhold output until every `Track::ready()` — i.e. until config frames
  have arrived. Code that assumes immediate output will race on slow devices.
- Timestamps go through `Stamp` for correction; do not hand-roll wrap/jump handling.
- `CHECK`/`CHECK_RET` and the project loggers for errors; no bare `assert()`/`abort()` in
  library code.
- Media objects belong to their `EventPoller`. Decode/transcode work that blocks still runs on
  `WorkThreadPool` today; everything else blocking has moved to `managerkit::BlockingExecutors`
  (see `docs/threading-blocking-work.md`). Do not add new blocking work to a poller.

## Verify

Codec readiness, malformed and truncated input, fragmented/reordered packets, timestamp
jumps and wraps, keyframe and config-frame handling, flush/EOF, and each downstream muxer
(RTSP, RTMP, HLS, FMP4, TS, MP4 record). Relevant unit suites: `tests/unit/codec/`,
`tests/unit/rtmp/`, `tests/unit/rtsp/`, `tests/unit/rtcp/`, `tests/unit/record/`,
`tests/unit/extension/`.

Detailed checklist: `.codex/skills/s3mediakit-media-codec/references/codec-checklist.md`.
