# Codec checklist

- Codec ID is unique and consistently mapped to SDP/MOV/MPEG names.
- Track `ready()`, `clone()`, extra data, and config parsing are defined.
- Frame key/config/drop/decode flags match the bitstream.
- RTP decoder reassembles and dispatches complete frames; encoder handles MTU/fragmentation.
- RTMP encoder flushes buffered state and emits config packets when needed.
- Plugin factory registration is wired exactly once.
- No dispatcher/ring/listener state is copied by `clone()`.
- Tests cover valid, fragmented, malformed, config, keyframe, timestamp, and flush paths.
