# S3MediaKit – Agent Instructions

S3MediaKit is a high-performance C++11 streaming media server (derived from ZLMediaKit). It supports RTSP/RTMP/HLS/WebRTC/GB28181 and more. A Go CGo binding and Android SDK are also provided.

## Build

> Full build guide: [docs/README.md](docs/README.md)

### Prerequisites (Ubuntu 22.04+)
```bash
sudo apt-get install -y build-essential cmake git libssl-dev protobuf-compiler \
    libprotobuf-dev libsqlite3-dev libcurl4-openssl-dev libsrtp2-dev gcc g++ gdb ffmpeg pkg-config
```

### ⚠️ AWS SDK must be built first if `ENABLE_AWS_SDK=ON`
Install to `3rdpart/aws-sdk/bin/`. See [docs/README.md](docs/README.md#build-aws-sdk).

### Configure & Build
```bash
mkdir -p build && cd build

# Debug (default)
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_AWS_SDK=ON

# Release
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_WEBRTC=false -DENABLE_FFMPEG=false \
         -DENABLE_TESTS=false -DENABLE_API=false -DENABLE_AWS_SDK=ON

make -j$(nproc)
```

Key CMake options: `ENABLE_AWS_SDK`, `ENABLE_WEBRTC`, `ENABLE_FFMPEG`, `ENABLE_MP4`, `ENABLE_HLS`, `ENABLE_SRT`, `ENABLE_MANAGER`, `ENABLE_MOTION`, `ENABLE_TESTS`, `ENABLE_API`.  
`-DENABLE_DEBUG` is defined automatically in Debug builds.

### Docker
```bash
sh build_docker_images.sh [-t build|push] [-m Debug|Release] [-v <version>]
```

## Project Layout

| Path | Purpose |
|------|---------|
| `src/` | Core mediakit library (`s3mediakit` static lib). Subdirs by protocol/feature: `Codec/`, `Http/`, `Rtsp/`, `Rtmp/`, `Rtp/`, `Record/`, `HLS/`, etc. |
| `server/` | `MediaServer` executable – `WebApi*.cpp`, `WebHook.cpp`, `Manager.cpp` |
| `manager/` | `managerkit` namespace – `Camera/`, `Storage/`, `Control/`, `User/`, `Local/`, `Server/`, `Common/` |
| `ext-codec/` | Extended codec implementations (H264/H265/AAC/G711/OPUS/VP8/VP9/AV1/…) |
| `ext-plugin/` | SDK plugin entry point |
| `api/` | C API SDK (`mk_*.h` headers in `api/include/`) |
| `3rdpart/` | Vendored libs: `S3ToolKit/`, `jsoncpp/`, `jwt-cpp/`, `media-server/`, `onvif/`, `aws-sdk/` |
| `webrtc/` | WebRTC support (compiled with `ENABLE_WEBRTC`) |
| `srt/` | SRT support (compiled with `ENABLE_SRT`) |
| `golang/` | Go CGo bindings wrapping the C API |
| `Android/` | Android Gradle project |
| `migration/` | SQLite schema migration SQL files |
| `conf/` | `config.ini` and runtime configuration |
| `docs/` | API integration docs (Vietnamese) |
| `tests/` | Stand-alone test/example executables |

## Namespaces & Key Abstractions

- **`toolkit`** – from `3rdpart/S3ToolKit/`: event poller, thread pool, TCP/UDP server/client, timer (`toolkit::Timer::Ptr`), logger, INI config (`mINI::Instance()`), SQLite pool (`toolkit::SqlitePool`)
- **`mediakit`** – core streaming logic, codec, protocol sessions
- **`managerkit`** – camera management, storage tiering, user auth, sync

## Coding Conventions

- **C++11 only** – no `std::optional`, no `std::filesystem`, no structured bindings. Use the project's custom `Optional<T>` (defined in `manager/Storage/DbSchema.h`).
- **No raw `this` in async callbacks** – always capture via `weak_from_this()` and lock before accessing members. See repo memory for threading pitfalls.
- **Singletons** – use `ClassName::Instance()` pattern (not `getInstance()`).
- **Smart pointers** – prefer `std::shared_ptr`/`std::weak_ptr`; type aliases as `using Ptr = std::shared_ptr<ClassName>`.
- **`enable_shared_from_this`** – required for classes that schedule async tasks referencing `this`.
- **Config keys** – declare with `onceToken` in the relevant namespace and register defaults via `mINI::Instance()[key] = default_value`.
- **Database** – SQLite via `toolkit::SqlitePool`; schema changes go in `migration/updates/` as numbered SQL files and registered in `MigrationHistory`.
- **HTTP API** – uses `CHECK_AUTH_TOKEN()` guard (JWT). Sync/internal APIs intentionally omit auth and must be network-isolated.
- **Protocol conversion** – handled inside `src/`; do not duplicate codec logic in `ext-codec/`.

## Thread Safety Notes

- Avoid capturing raw `this` in `doDelayTask`/timer callbacks in `manager/` and `server/`; use `weak_from_this()+lock()` before any member access.
- Singleton managers with timers: prefer calling `Instance().method()` inside the lambda instead of capturing `this`.

## API Docs

- REST API & web hook reference: see `docs/` (Vietnamese)
- Storage tiering: [docs/api-storage-tiering.md](docs/api-storage-tiering.md)
- Multi-node DB sync: [docs/api-sync.md](docs/api-sync.md)
- Bookmark: [docs/api-bookmark.md](docs/api-bookmark.md)
- PTZ preset: [docs/api-ptz-preset.md](docs/api-ptz-preset.md)
- Timeline thumbnail: [docs/api-timeline-thumbnail.md](docs/api-timeline-thumbnail.md)
- Performance tuning: [conf/readme.md](conf/readme.md)

## Deployment

- Docker Compose: `docker-compose.yml` (image `3spro/3spro-mserver`)
- K8s hints: [k8s_readme.md](k8s_readme.md)
- Config override: mount custom `config.ini` into `/opt/media/conf/`
- TLS cert: replace `tests/default.pem`

## Common Pitfalls

- **AWS SDK prerequisite**: build and install to `3rdpart/aws-sdk/bin/` before running CMake. Forgetting this causes missing library errors.
- `CMAKE_INCLUDE_CURRENT_DIR` is `OFF` – do not rely on implicit current-directory includes.
- `ENABLE_DEBUG` is auto-defined for Debug builds; do not define it manually.
- Go bindings require the `mk_mediakit.h` C API headers and a built shared library.
