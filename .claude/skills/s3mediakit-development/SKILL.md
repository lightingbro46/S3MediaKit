---
name: s3mediakit-development
description: "Use when writing or changing C++ in the S3MediaKit repository — code in src/, manager/, server/, api/, ext-codec/ or ext-plugin/ — or when building, configuring or debugging it. Covers the C++11 constraints, lifetime/threading patterns, logging, config, and build commands this project requires."
---

# S3MediaKit development

Load `s3mediakit-architecture` first if you are not sure which layer owns the change.

## C++11 — non-negotiable

`CMAKE_CXX_STANDARD` is 11. These will not compile:

| Forbidden | Use instead |
|---|---|
| `std::optional<T>` | `managerkit::Optional<T>` (`manager/Storage/DbSchema.h`) |
| `std::filesystem` | project file helpers / POSIX calls already used nearby |
| `std::make_unique` | `std::unique_ptr<T>(new T(...))` |
| `weak_from_this()` | `std::weak_ptr<T> w = shared_from_this();` |
| structured bindings, `if constexpr`, generic lambdas, `auto` return deduction | explicit types |
| `std::string_view`, `std::variant`, `std::any` | `const std::string &`, tagged structs |

## Lifetime and threading

1. Any class whose methods are reached from a callback derives from
   `std::enable_shared_from_this<T>` and is held by `shared_ptr`.
2. **Never capture raw `this`** in a timer, `doDelayTask`, `poller->async`, socket callback,
   `HttpResponseInvoker`, or work-pool task:

   ```cpp
   std::weak_ptr<Foo> weak_self = shared_from_this();
   _poller->async([weak_self]() {
       auto self = weak_self.lock();
       if (!self) return;
       self->step();
   });
   ```

   Singletons are the exception — call `Bar::Instance()` inside the lambda instead of
   capturing anything.
3. Respect poller affinity: sessions, `MediaSource`s and muxers belong to one `EventPoller`.
   Hop with `poller->async()`; do not reach across threads to mutate them.
4. Blocking work — disk, SQLite, S3/AWS, ffmpeg, device HTTP/ONVIF — goes to a
   `managerkit::BlockingExecutors` pool (`api()`, `device()`, `discovery()`, `ffmpeg()`),
   not to `WorkThreadPool`: its threads are pollers running timers and ordered queues.
   A blocked poller stalls every stream on that thread. See `docs/threading-blocking-work.md`.
5. Prefer `std::atomic` + `std::atomic_load_explicit` for the small shared flags the code
   already uses this way, rather than adding a mutex on a hot path.

## Project conventions

- Singletons: `ClassName::Instance()`.
- Config: register defaults with `onceToken` into `mINI::Instance()`; read with `GET_CONFIG`.
  New keys belong next to their existing family in `src/Common/config.{h,cpp}` or
  `server/main.cpp`, and should be documented in `conf/`.
- Logging: `TraceL`, `DebugL`, `InfoL`, `WarnL`, `ErrorL`. Never log secrets, tokens, keys,
  passwords or stream credentials.
- Errors: `CHECK` / `CHECK_RET` (`src/Common/macros.h`) and returned status. No bare
  `assert()` or `abort()` in library code.
- Includes are explicit — `CMAKE_INCLUDE_CURRENT_DIR` is `OFF`.
- Compile-time gates (`ENABLE_MANAGER`, `ENABLE_TIER_STORAGE`, `ENABLE_SYNC_CHANGES`,
  `ENABLE_WEBRTC`, `ENABLE_MOTION`, `ENABLE_AWS_SDK`): code inside a gate must compile out
  cleanly, and callers outside must not assume the symbols exist.
- `.clang-format` exists — format only the lines you touch, never the whole file.

## Workflow

1. `git status --short` and read the existing diff; preserve unrelated user changes.
2. `rg` for the nearest implementation and its test. Reuse that pattern; do not invent a
   new abstraction for a first use.
3. Make the smallest change that satisfies the request. No drive-by refactors.
4. Build the affected target, not the world.
5. Update the nearest test and, when behavior or public config changes, the `docs/` page.
6. Re-read the final `git diff` before reporting.

## Build

Reuse the configured build when it exists (`build/` = Debug, `ENABLE_AWS_SDK=ON`,
`ENABLE_SYNC_CHANGES=ON`, `ENABLE_TESTS=OFF`):

```bash
cmake --build build -j2
cmake --build build --target MediaServer -j2   # single target when you can
```

Fresh configure (AWS SDK must already exist under `3rdpart/aws-sdk/bin/`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_AWS_SDK=ON
cmake --build build -j"$(nproc)"
```

`build-sync-off/` is the `ENABLE_SYNC_CHANGES=OFF` variant — use it to confirm gated code
still compiles out. `-DENABLE_ASAN=ON` for memory bugs. Full builds are slow; expect
minutes, and say so rather than silently skipping the build.

For tests see `s3mediakit-testing`. For deeper boundary notes see
`.codex/skills/s3mediakit-development/references/architecture.md`.

## Reporting

State the files changed, the exact build/test commands run and their outcome, anything you
could not verify, and follow-ups. Do not claim a build passed that you did not run.
