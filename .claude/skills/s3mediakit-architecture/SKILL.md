---
name: s3mediakit-architecture
description: "Use when orienting in the S3MediaKit codebase — understanding how the media server is structured, which module or namespace owns a behavior, how a stream/recording/API/sync flow travels end to end, or where to put a new feature. Load this before designing or implementing a cross-module change, and before answering 'where does X live?'."
---

# S3MediaKit architecture

Read this before guessing where code belongs. Verify with `rg` rather than assuming — the
repository is large and several subsystems have near-identical names in different layers.

## The four layers

```
             server/          process lifecycle, REST (WebApi*), webhooks, Manager.cpp glue
                |
   +------------+------------+
   |                         |
manager/  (managerkit)     src/ + ext-codec/  (mediakit)
cameras, speakers,         sessions, tracks/frames, muxers,
storage & tiering,         RTSP/RTMP/RTP/HLS/FMP4/WebRTC, recording
users/auth, SQLite,
cluster sync
   |                         |
   +------------+------------+
                |
      3rdpart/S3ToolKit/  (toolkit)
      EventPoller, WorkThreadPool, Socket, Timer, Logger, SqlitePool
```

`api/` sits beside `server/` as a frozen C ABI over `mediakit` for CGo (`golang/`) and the
Android SDK. `ext-plugin/` adapts vendor device SDKs behind `IDevice.h`.

Ownership rules that are easy to get wrong:

- `toolkit` supplies concurrency and infrastructure — it never holds media or device policy.
- Protocol conversion belongs in `src/`. `ext-codec/` holds codec-specific packetization
  only; do not re-implement in `ext-codec/` what `src/` already owns.
- `managerkit` decides *what* to record, store, tier and sync. `mediakit` decides *how*
  bytes move. Storage policy in `src/` and packetization in `manager/` are both smells.
- `server/Manager.cpp` is the adapter between the two — JSON shaping, option mapping, and
  wiring of managerkit objects into the media pipeline live there, not in `manager/`.

## Where things live

Start from [module-map.md](references/module-map.md) when you need the per-directory
breakdown of `src/`, `manager/`, `server/` and `tests/`.

Quick answers:

| Question | Look at |
|---|---|
| How does a stream get published/consumed? | `src/Common/MediaSource.*`, `MediaSink.*`, `MultiMediaSourceMuxer.*` |
| How is a codec registered? | `src/Extension/Factory.cpp`, `src/Extension/Track.h`, `ext-codec/` |
| How does a camera become a stream? | `manager/Camera/StreamSource.*`, `StreamSink.*`, `GenericRtspCameraImp.*`, `CameraManager.*` |
| How are recordings written/read? | `manager/Local/Time*.{h,cpp}` (TimeRecorder/TimeFile/TimeQuery/TimeMuxer), `src/Record/` |
| How does tiering to NAS/S3 work? | `manager/Local/Tier*.{h,cpp}`, `manager/Storage/TieringJob.h`, `docs/storage-tiering-flow.md` |
| Where is an entity persisted? | `manager/Storage/<Entity>.h` (Repository + Imp in one header) |
| How do nodes stay in sync? | `manager/Extension/SyncManager.*`, `TableSyncHandler.h`, `manager/Storage/TransactionLog.*`, `docs/api-sync.md` |
| Where is an endpoint defined? | `server/WebApi*.cpp`, registered from `installWebApi()` in `server/WebApi.cpp` |
| Who watches system health? | `manager/Server/*Monitor.*`, started by `installGlobalMonitor()` in `server/Manager.cpp` |

## Execution model

This is the part that breaks people. See [data-flows.md](references/data-flows.md) for
worked end-to-end traces.

- **`EventPoller`** — one thread per poller, a pool sized to the CPU. A `Session`, a
  `MediaSource` and its muxer are bound to one poller and must only be touched from it.
  Cross-thread hand-off goes through `poller->async()` / `async_first()`.
- **`WorkThreadPool`** — for anything blocking: disk I/O, SQLite, AWS S3, ffmpeg,
  ONVIF/HTTP calls to devices. Never block a poller thread.
- **Timers** (`Timer`, `doDelayTask`) fire on a poller. Capture a `weak_ptr`, never `this`.
- **Async REST handlers** hold an `HttpResponseInvoker`; the response may be produced on a
  different thread than the request. Call it exactly once.

The C++11 lifetime pattern used throughout:

```cpp
class Foo : public std::enable_shared_from_this<Foo> { ... };

std::weak_ptr<Foo> weak_self = shared_from_this();
timer = std::make_shared<Timer>(1.0f, [weak_self]() {
    auto self = weak_self.lock();
    if (!self) return false;      // owner gone: stop the timer
    self->tick();
    return true;
}, poller);
```

Singletons (`GlobalMonitor::Instance()`, `mINI::Instance()`, `SyncManager::Instance()`, …)
may be referenced directly inside a callback — they outlive the task.

## Configuration and features

- Defaults are registered with `onceToken` writing into `mINI::Instance()`; see the top of
  `server/main.cpp` and `src/Common/config.cpp` for the pattern. Read with `GET_CONFIG`.
- Runtime config is `conf/config.ini`; `docs/` documents features (mostly Vietnamese).
- Large features are compile-time gated: `ENABLE_MANAGER`, `ENABLE_TIER_STORAGE`,
  `ENABLE_SYNC_CHANGES`, `ENABLE_WEBRTC`, `ENABLE_MOTION`, `ENABLE_AWS_SDK`. Code behind a
  gate must compile out cleanly — check both states before claiming a build is healthy.

## Before you change anything

1. Name the layer that owns the behavior; if two layers seem to, the boundary above decides.
2. `rg` for the nearest existing implementation and its test, and follow it.
3. Check whether the path is inside a compile-time gate.
4. Check thread affinity of every object you touch.
5. Then load the task-specific skill: `s3mediakit-development`, `s3mediakit-api`,
   `s3mediakit-database`, `s3mediakit-media-codec`, or `s3mediakit-testing`.
