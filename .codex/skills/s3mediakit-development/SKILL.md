---
name: s3mediakit-development
description: "Use when developing S3MediaKit C++ code, navigating its architecture, changing manager/server/src/api behavior, configuring or building the project, or debugging an issue that crosses its media-server modules."
---

# S3MediaKit development

Use this skill with the shared workflow for changes in this repository. Keep the project C++11-compatible and follow existing ownership/thread-affinity patterns.

## Locate the right layer

- `src/`: `mediakit` protocol sessions, media sources/sinks, codec-independent pipeline, RTSP/RTMP/RTP/HLS/recording.
- `ext-codec`: codec-specific plugins and packetization extensions; do not duplicate conversion logic already owned by `src/`.
- `manager/`: `managerkit` camera/device lifecycle, storage, auth, SQLite entities/repositories, cluster sync.
- `server`: process lifecycle, `WebApi*.cpp`, webhooks, MediaServer integration.
- `api`: stable C API surface; check header compatibility and CGo consumers.
- `3rdpart/S3ToolKit`: pollers, timers, thread pools, networking, logging, SQLite primitives.

## Development rules

1. Find the nearest implementation and test before introducing a new abstraction.
2. Preserve thread affinity: sessions and media objects generally belong to an `EventPoller`; blocking work belongs on `WorkThreadPool`.
3. For callbacks in C++11, use `std::enable_shared_from_this<T>`, `std::weak_ptr<T> weak_self = shared_from_this()`, then `lock()` inside the callback. Never capture raw `this`.
4. Register configuration defaults with `onceToken`; use `GET_CONFIG` and existing reload mechanisms.
5. Use project logger macros (`TraceL`, `DebugL`, `InfoL`, `WarnL`, `ErrorL`) and preserve error propagation.
6. Update the nearest tests/docs when behavior or public configuration changes.

## Build

Prefer an existing configured build:

```bash
cmake --build build -j2
```

For a fresh Debug configure, ensure the AWS SDK prerequisite exists when enabled. See [architecture.md](references/architecture.md) for feature boundaries and validation targets.
