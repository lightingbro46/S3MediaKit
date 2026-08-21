---
name: s3mediakit-media-codec
description: "Use when adding or changing S3MediaKit codecs, tracks, frames, RTP/RTMP packetization, codec plugin registration, media-source/sink behavior, or protocol conversion in src/ and ext-codec/."
---

# S3MediaKit media and codec development

Trace the full path from input packet/frame to track, dispatcher, muxer, and output protocol before editing. Reuse existing codec helpers when the packetization rules match.

## Codec workflow

1. Determine whether the change belongs in `src/` (core protocol/media behavior) or `ext-codec/` (codec plugin implementation).
2. Inspect `src/Extension/Frame.h`, `src/Extension/Factory.cpp`, nearby codec implementations, and existing unit tests.
3. For a new codec, update the codec map, implement track/frame metadata and config parsing, add RTP/RTMP adapters only when required, and register the plugin through the existing factory mechanism.
4. Ensure `Track::clone()` copies codec metadata only; never copy ring buffers, listeners, or dispatcher state.
5. Preserve frame ownership and zero-copy assumptions. Mark key/config/drop/decode behavior according to the actual bitstream format.
6. Verify codec readiness, malformed input, fragmented packets, timestamps, keyframes, config frames, flush behavior, and downstream muxer compatibility.

## Pipeline rules

- Do not bypass `MultiMediaSourceMuxer` to push directly into a protocol-specific output.
- `MediaSink` output may wait until `Track::ready()` receives codec configuration.
- Use `CHECK`/`CHECK_RET` conventions and project logging; do not introduce bare `assert()`/`abort()` in library code.

See [codec-checklist.md](references/codec-checklist.md).
