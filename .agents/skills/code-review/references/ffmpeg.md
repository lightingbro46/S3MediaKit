# FFmpeg Code Review Reference

Use this reference when reviewing code that uses FFmpeg libraries, packet/frame processing, decoding, encoding, filtering, muxing, demuxing, or timestamp manipulation.

## Main review areas

1. object ownership
2. packet/frame lifetime
3. timestamp correctness
4. time-base conversion
5. codec/context lifecycle
6. muxer/demuxer contracts
7. error handling
8. reconnect/discontinuity behavior
9. performance

## AVPacket ownership

Review use of:

- `av_packet_alloc`
- `av_packet_free`
- `av_packet_ref`
- `av_packet_clone`
- `av_packet_move_ref`
- `av_packet_unref`

Important rule:

`AVPacket` structure ownership and referenced buffer ownership are separate concerns.

Check whether packet data remains valid after:

- callback returns
- queue insertion
- source packet is unref'd
- demuxer reads next packet
- packet is moved

If an `AVPacket` crosses an asynchronous boundary, verify the receiver owns a valid reference.

Risky pattern:

```cpp
queue.push(packet);
av_packet_unref(&packet);
```

if `queue.push` performs a shallow copy without `av_packet_ref`.

## AVFrame ownership

Review:

- `av_frame_alloc`
- `av_frame_free`
- `av_frame_ref`
- `av_frame_clone`
- `av_frame_move_ref`
- `av_frame_unref`

If a frame crosses threads or is retained beyond the callback, ensure the referenced buffers remain valid.

Inspect hardware frames carefully because ownership may involve hardware frame contexts.

## Codec context lifecycle

Check:

- correct allocation
- parameters copied before open
- codec opened once per intended lifecycle
- flush behavior
- close/free on all paths
- reconnect/reconfiguration handling

Do not recreate codec contexts per frame or packet.

## Decode API

For send/receive APIs:

```cpp
avcodec_send_packet
avcodec_receive_frame
```

Check:

- `EAGAIN` handling
- draining/flush
- EOF
- packet/frame reuse
- error propagation

Likewise for encoding:

```cpp
avcodec_send_frame
avcodec_receive_packet
```

Do not assume one input always produces exactly one output.

## Time bases

Always identify which time base each timestamp belongs to.

Typical time bases:

- input stream `AVStream::time_base`
- codec context time base
- encoder time base
- output stream time base
- custom internal time base

Review every conversion using:

```cpp
av_rescale_q
av_rescale_q_rnd
```

Never compare timestamps from different time bases directly.

## PTS / DTS

Understand:

- PTS = presentation timestamp
- DTS = decode timestamp

Review:

- monotonic DTS requirement for muxer
- B-frame reordering
- missing timestamps
- negative timestamps
- reconnect discontinuities
- timestamp offsets
- wraparound where relevant

Do not force:

```cpp
pkt.dts = pkt.pts;
```

unless codec/order semantics guarantee it.

Do not repair non-monotonic DTS with arbitrary `+1` without understanding the source discontinuity.

## Duration

Check whether packet/frame duration is:

- provided
- inferred correctly
- converted to correct time base

Incorrect duration can cause playback timing and segment-boundary issues.

## `AV_NOPTS_VALUE`

Treat `AV_NOPTS_VALUE` explicitly.

Do not rescale or subtract it as if it were a normal timestamp.

## Reconnect behavior

For live RTSP/media reconnect, inspect whether the following state must reset or rebase:

- first PTS/DTS
- timestamp offset
- last output DTS
- frame counters
- discontinuity flags
- decoder state
- muxer state
- segment state

A new source session may restart timestamps from zero or another epoch.

The output timeline must have a deliberate policy:

- preserve continuous timeline
- mark discontinuity
- restart session/output
- rebase timestamps

## Muxing

Review:

- stream index
- codec parameters
- output stream time base
- `av_packet_rescale_ts`
- header/trailer lifecycle
- interleaving
- monotonic DTS
- error handling

Check the difference between:

```cpp
av_write_frame
av_interleaved_write_frame
```

and whether the chosen behavior matches the muxer.

## Demuxing

Review:

- `av_read_frame` errors
- EOF vs transient network error
- reconnect policy
- packet unref on every iteration
- stream selection
- probe/open timeout
- interrupt callback where required

## Filters

For `libavfilter` inspect:

- filter graph creation frequency
- graph reconfiguration
- frame ownership
- pixel format negotiation
- time base
- frame rate
- buffer source/sink errors

Do not rebuild expensive filter graphs per frame.

## Pixel formats

Check:

- input pixel format
- output pixel format
- color range
- color space
- hardware/software frame compatibility

Be careful with legacy full-range `yuvj*` formats and color-range metadata.

## Audio/video synchronization

Inspect:

- separate stream time bases
- clock source
- timestamp origin
- resampling effects
- decoder delay
- encoder delay

Do not compare audio and video timestamps without converting to a common time base.

## Memory and buffering

Check:

- packet/frame leaks
- retained references causing memory growth
- queue depth
- frame pools
- conversion buffers
- `sws`/`swr` context reuse

Avoid allocating conversion contexts per frame.

## swscale / swresample

Reuse:

- `SwsContext`
- `SwrContext`

when format parameters are stable.

Recreate only when format/layout/rate actually changes.

## FFmpeg logging

Avoid per-packet high-severity logs in live streams.

Include:

- stream/camera identifier
- FFmpeg error string
- relevant timestamp/state

Convert error codes with:

```cpp
av_strerror
```

or equivalent project utility.

## Error checking

FFmpeg APIs commonly return negative error codes.

Do not ignore them.

Pay special attention to:

- open input
- find stream info
- codec open
- send/receive
- filter push/pull
- write header
- write packet
- write trailer

## Performance review

Hot-path concerns include:

- packet/frame cloning
- pixel conversion
- software decode
- software encode
- filter graphs
- allocations
- memcpy
- locks
- logging

Distinguish unavoidable codec work from accidental overhead.

## Review findings to prioritize

HIGH:

- packet/frame lifetime invalid across async boundary
- incorrect timestamp rescaling
- reconnect creates timestamp regression
- ignored FFmpeg error causing invalid state
- decoder/encoder misuse
- leak in long-running live stream

MEDIUM:

- unnecessary packet/frame copy
- context recreated repeatedly
- excessive logging
- weak discontinuity handling
