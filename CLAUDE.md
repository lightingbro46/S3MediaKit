# CLAUDE.md

Guidance for Claude Code when working in this repository. `AGENTS.md` is the
vendor-neutral version of the same rules; keep the two in sync when either changes.

## What this project is

S3MediaKit is a C++11 streaming media server derived from ZLMediaKit, extended with a
VMS/NVR layer. It serves RTSP, RTMP, HLS, HTTP-FLV, FMP4, WebRTC, GB28181/RTP and SRT,
records to a time-indexed file store, tiers recordings to NAS/S3, manages cameras and
users in SQLite, and synchronizes state across media-server nodes. It also ships a C API
(`api/`), Go/CGo bindings (`golang/`) and an Android SDK (`Android/`).

Three namespaces, three responsibilities — do not blur them:

| Namespace | Lives in | Owns |
|---|---|---|
| `toolkit` | `3rdpart/S3ToolKit/` | Pollers, timers, thread pools, sockets, logging, SQLite primitives |
| `mediakit` | `src/`, `ext-codec/` | Protocol sessions, tracks/frames, muxers, recording, the media pipeline |
| `managerkit` | `manager/` | Cameras, speakers, storage/tiering, users/auth, SQLite entities, cluster sync |

`server/` adapts those to process lifecycle, the REST API and webhooks. `api/` is a
frozen C ABI surface — never leak C++ types through it.

## Repository map

| Path | Responsibility |
|---|---|
| `src/` | Core `mediakit`: `Rtsp/ Rtmp/ Rtp/ Http/ TS/ FMP4/ Record/ Codec/ Extension/ Player/ Pusher/ Motion/ Onvif/ Transcode/` |
| `ext-codec/` | Codec plugins (H264/H265/AAC/G711/Opus/…) registered through `src/Extension/Factory.cpp` |
| `ext-plugin/` | Vendor device SDK adapters behind `IDevice.h` / `DeviceFactory` |
| `manager/` | `Camera/ Speaker/ Control/ Local/ Server/ Storage/ User/ Extension/ Common/` (see the architecture skill) |
| `server/` | `main.cpp`, `WebApi*.cpp`, `WebHook.cpp`, `Manager.cpp`, `Process.cpp`, `FFmpegSource.cpp` |
| `api/` | Public C headers (`mk_*.h`) + implementation; consumed by CGo and Android |
| `migration/` | `updates/` = ESC db, `mserver_updates/` = MediaServer db; numbered SQL, applied in order |
| `tests/` | `unit/` (GTest, via ctest), `integration/tier_storage/`, `performance/`, legacy demo programs at top level |
| `conf/` | Runtime `config.ini` and configuration docs |
| `docs/` | Feature/API documentation, mostly Vietnamese — read before designing a feature change |
| `.claude/skills/` | Claude-loadable skills for this repo |
| `.agents/skills/`, `.codex/skills/` | Shared lifecycle skills and their reference material |

## Hard rules

**C++11 only.** No `std::optional`, `std::filesystem`, structured bindings,
`std::make_unique`, `weak_from_this()`, generic lambdas, or `if constexpr`. Use the
project's `managerkit::Optional<T>` (`manager/Storage/DbSchema.h`) instead of
`std::optional`.

**Never capture raw `this` in a callback.** Timers, `doDelayTask`, poller tasks, async
invokers and socket callbacks outlive their owner. Always:

```cpp
std::weak_ptr<MyClass> weak_self = shared_from_this();   // class : std::enable_shared_from_this<MyClass>
poller->async([weak_self]() {
    auto self = weak_self.lock();
    if (!self) return;
    self->doWork();
});
```

Singleton managers are the one exception — call `ClassName::Instance()` inside the callback.

**Thread affinity is real.** Sessions and media objects belong to one `EventPoller` and must
not be touched from another thread. Blocking work (disk, SQLite, S3, SOAP/ONVIF, ffmpeg) goes
to a `managerkit::BlockingExecutors` pool — `api()`, `device()`, `discovery()` or `ffmpeg()` —
**not** to `WorkThreadPool`, whose threads are pollers running timers and ordered queues.
REST handlers dispatch through `server/ApiWorkDispatcher.h`. Never pick a thread from request
data: a function is either blocking or async, never both depending on its input. Crossing any
of these lines is the most common source of bugs here — see
[docs/threading-blocking-work.md](docs/threading-blocking-work.md).

**`CMAKE_INCLUDE_CURRENT_DIR` is `OFF`.** Includes must be explicit and correct.

**Config defaults** are registered with `onceToken` + `mINI::Instance()`; read them with
`GET_CONFIG`. **Logging** uses `TraceL/DebugL/InfoL/WarnL/ErrorL`. **Errors** use
`CHECK`/`CHECK_RET` — no bare `assert()`/`abort()` in library code.

## Working rules

- Read `git status --short` and the existing diff first; preserve unrelated user changes.
- Search with `rg` before editing. Follow the nearest existing pattern instead of
  inventing an abstraction.
- Keep the change scoped to the request. Do not reformat or rewrite unrelated files.
- Never put secrets, tokens, keys or production data into code, logs, patches or replies.
- No destructive git/filesystem commands unless the user names the exact target.
- Report what you actually built/ran and its result. Do not claim completion otherwise.

## Build and test

Reuse the configured build when one exists (`build/` is Debug, AWS SDK on, sync on):

```bash
cmake --build build -j2                      # incremental; the full build is slow
```

Fresh Debug configure. FFmpeg (`3rdpart/FFmpeg`, n7.1.5; 4.4 still builds — the code guards with `FF_CODEC_VER_7_1`) and the AWS SDK (`3rdpart/aws-sdk-cpp`) are
submodules that CMake builds once into `release/linux/3rdpart/` (shared by every build dir; the
first build is long). FFmpeg libraries carry the `_s3` suffix and are copied next to `MediaServer`
(RUNPATH `$ORIGIN`). `-DENABLE_BUNDLED_FFMPEG=OFF` falls back to the system FFmpeg:

```bash
git submodule update --init --recursive --depth 1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_AWS_SDK=ON
cmake --build build -j"$(nproc)"
```

Tests need their own build directory because `ENABLE_TESTS` is `OFF` in `build/`:

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_AWS_SDK=ON
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure -R unit.
```

Build a single target rather than everything when you can. Notable CMake switches:
`ENABLE_MANAGER`, `ENABLE_TIER_STORAGE`, `ENABLE_SYNC_CHANGES`, `ENABLE_WEBRTC`,
`ENABLE_MOTION`, `ENABLE_AWS_SDK`, `ENABLE_TESTS`, `ENABLE_ASAN`. `ENABLE_SYNC_CHANGES`
also defines the `ENABLE_SYNC_CHANGES` macro — code guarded by it is absent from
sync-off builds (`build-sync-off/`).

## Invariants worth memorizing

- Persistence layering: `*Repository` = SQL only; `*Imp` = validation, defaults, IDs,
  timestamps, domain decisions; managers = orchestration. Production code calls `*Imp`.
- Schema changes are **new** numbered migration files. Never edit an applied migration.
- SQLite transactions: `beginWriteTxn()` (BEGIN IMMEDIATE) for any transaction that may write,
  even if it reads first; `beginReadTxn()` only for read-only snapshots. A deferred transaction
  that reads then writes fails with "database is locked" when another connection commits meanwhile.
- REST handlers use the `API_ARGS_*` macros, `ApiErrCode::CODE_*`, and one `invoker(...)`
  call per async request. Internal sync APIs intentionally have a different trust boundary.
- `Track::clone()` copies codec metadata only — never ring buffers, listeners or
  dispatcher state.
- Never bypass `MultiMediaSourceMuxer` to push straight into one protocol's output.

## Skills

Load the skill that matches the task; read its `references/` only once the task enters
that topic.

| Skill | Use when |
|---|---|
| `s3mediakit-architecture` | Orienting in the codebase, tracing a flow, deciding which module owns a change |
| `s3mediakit-development` | Writing or changing C++ in `src/`, `manager/`, `server/`, `api/` |
| `s3mediakit-api` | REST endpoints, `WebApi*` registration, auth guards, error codes |
| `s3mediakit-database` | SQLite schema, migrations, entities, repositories, cluster sync |
| `s3mediakit-media-codec` | Codecs, tracks/frames, packetization, muxers, protocol conversion |
| `s3mediakit-testing` | Adding/running unit tests, tier-storage integration tests |
| `export-feature` | `/export-feature <start-commit>`: package commits to `HEAD` via `tools/git-patch/export-feature.sh` and list the changed features |

`.agents/skills/` holds the generic lifecycle playbooks (requirement-analysis,
codebase-exploration, architecture-design, implementation-planning, implementation,
test-design, code-review, bug-investigation) with deep reference notes on C++ review,
concurrency, ffmpeg and crash/performance debugging. Read them directly when a task calls
for that discipline — they are shared with other agent runtimes and are not duplicated here.

## Finishing a task

End implementation work with: files changed, commands run and their result, known
limitations, and remaining follow-up. For reviews, lead with findings ordered by
severity, each with file/line evidence.

Commit messages in this repository are written in Vietnamese; match the surrounding style.
