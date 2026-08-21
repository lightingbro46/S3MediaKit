# FFmpeg / Media Debugging Reference

Use this reference for:

- non-monotonic DTS
- missing PTS/DTS
- A/V sync
- corrupt stream
- decode errors
- muxer errors
- playback discontinuity
- reconnect issues
- HLS/MP4 timestamp problems
- black video after remux/export

## Start with evidence

Collect:

- input protocol
- codec
- resolution
- frame rate
- stream time base
- packet timestamps
- reconnect boundary
- exact FFmpeg error
- whether the problem exists in source, internal processing, or output

Do not start by modifying timestamps.

## Basic ffprobe inspection

Stream metadata:

```bash
ffprobe -v error \
  -show_streams \
  -show_format \
  input.ts
```

Video packets:

```bash
ffprobe -v error \
  -select_streams v:0 \
  -show_packets \
  -show_entries packet=pts,pts_time,dts,dts_time,duration,duration_time,flags,pos,size \
  -of csv \
  input.ts
```

Frames:

```bash
ffprobe -v error \
  -select_streams v:0 \
  -show_frames \
  -show_entries frame=pts,pts_time,pkt_dts,pkt_dts_time,best_effort_timestamp,best_effort_timestamp_time,pict_type,key_frame \
  input.ts
```

Use packet inspection for muxing/timestamp problems and frame inspection for decode/display ordering.

## Non-monotonic DTS

Error patterns may include:

```text
Non-monotonous DTS in output stream
```

Investigation:

1. find first output packet where DTS decreases
2. identify its source packet
3. compare time bases
4. inspect reconnect/discontinuity boundary
5. inspect rescaling
6. inspect B-frame reorder
7. inspect timestamp offset logic

Do not blindly apply:

```cpp
if (dts <= lastDts)
    dts = lastDts + 1;
```

This may hide a broken timeline and distort duration/sync.

## PTS vs DTS

For codecs with frame reordering:

```text
DTS != PTS
```

can be valid.

Inspect decode/display order.

Do not force equality unless codec/output constraints justify it.

## Time-base debugging

Record all relevant time bases:

```text
input stream time_base
decoder time_base
internal time_base
encoder time_base
output stream time_base
```

For every transformation, write down:

```text
value
source time base
destination time base
conversion function
```

Typical correct conversion:

```cpp
av_rescale_q(value, src_tb, dst_tb)
```

Packet timestamps may also be converted with:

```cpp
av_packet_rescale_ts(...)
```

## AV_NOPTS_VALUE

Check explicitly before arithmetic or rescaling.

Bad:

```cpp
offset = pkt.pts - firstPts;
```

when either timestamp may be `AV_NOPTS_VALUE`.

## Reconnect

Record packets around reconnect:

```text
last packets before disconnect
first packets after reconnect
```

Compare:

- PTS
- DTS
- duration
- keyframe
- time base

Possible source behaviors after reconnect:

- timestamp continues
- timestamp resets to zero
- timestamp jumps backward
- timestamp jumps forward
- clock origin changes

Choose a deliberate output policy:

- new session
- discontinuity
- timestamp rebasing
- preserve global timeline

## HLS

Inspect:

- segment boundaries
- keyframe placement
- media sequence
- discontinuity markers
- EXTINF durations
- timestamp continuity

If reconnect causes timeline discontinuity, determine whether `EXT-X-DISCONTINUITY` or segmenter restart behavior is required.

## MP4

Black video or invalid playback after remux/export can involve:

- missing codec extradata
- wrong stream parameters
- incorrect timestamps
- non-keyframe start
- HEVC parameter sets
- incomplete MP4 finalization

Inspect:

```bash
ffprobe -show_streams output.mp4
```

Check codec parameters and start timestamps.

## Decode errors

Collect the exact error and packet position.

Inspect:

- packet corruption
- missing keyframe
- missing parameter sets
- decoder state after reconnect
- codec extradata
- packet ordering

For H.264/H.265 inspect whether SPS/PPS/VPS are available when decoder/session restarts.

## A/V sync

Convert audio/video timestamps to a common clock.

Compare:

```text
video pts in seconds
audio pts in seconds
```

Inspect:

- resampling
- encoder delay
- missing packets
- independent reconnect behavior
- timestamp origin

## Packet ownership

If corruption appears only under load or asynchronously, investigate ownership.

A packet queued after callback return must hold a valid buffer reference.

Use `av_packet_ref` or equivalent ownership pattern.

## FFmpeg error strings

Convert numeric errors to readable strings.

Example:

```cpp
char errbuf[AV_ERROR_MAX_STRING_SIZE];
av_strerror(ret, errbuf, sizeof(errbuf));
```

Logs should include the operation and stream identifier.

## Minimal reproduction

Prefer reproducing outside the full VMS when possible.

Examples:

```bash
ffmpeg -rtsp_transport tcp -i <url> -c copy out.ts
```

or remux recorded input:

```bash
ffmpeg -i input.ts -c copy output.mp4
```

If standalone FFmpeg reproduces the issue, source/input may be responsible.

If standalone works but application fails, inspect application timestamp/ownership logic.

## Compare boundaries

For intermittent failures capture a narrow window:

```text
5-20 packets before failure
failure packet
5-20 packets after failure
```

Large dumps can hide the actual discontinuity.

## Root-cause examples

Symptom:

```text
non-monotonic DTS
```

Trigger:

```text
RTSP reconnect
```

Root cause:

```text
last output DTS retained across source session reset,
while new source timestamps restart from zero
```

Regression test should simulate exactly that transition.
