# S3MediaKit agent guide

S3MediaKit is a C++11 streaming media server derived from ZLMediaKit. It provides RTSP, RTMP, HLS, WebRTC, GB28181, Go/CGo bindings, and an Android SDK.

## Operating rules

- Read this file, the relevant `README`/docs, and the nearest applicable skill before changing code.
- Inspect `git status --short` and existing diffs first. Preserve unrelated user changes.
- Search before editing with `rg`/`rg --files`; prefer existing patterns and tests over invented abstractions.
- Keep changes scoped to the request. Do not reformat or rewrite unrelated files.
- Never expose secrets, tokens, private keys, or production data in code, logs, patches, or responses.
- Do not run destructive Git/filesystem commands unless the user explicitly authorizes the exact target.
- Do not claim completion without reporting what was checked and the result.

## Language and compatibility

- The project is C++11. Do not use C++14/17/20 features such as `std::optional`, `std::filesystem`, structured bindings, or `weak_from_this()`.
- For asynchronous lifetime control, classes must use `std::enable_shared_from_this<T>` and C++11 code must use:

  ```cpp
  std::weak_ptr<MyClass> weak_self = shared_from_this();
  auto self = weak_self.lock();
  if (!self) return;
  ```

- Never capture raw `this` in timers, `doDelayTask`, poller tasks, or other callbacks. Singleton managers may call `ClassName::Instance()` inside the callback.
- Prefer `std::shared_ptr`/`std::weak_ptr`; use the project's `Optional<T>` instead of `std::optional`.

## Repository map

| Path | Responsibility |
|---|---|
| `src/` | Core `mediakit` library and protocol/media pipeline |
| `server/` | `MediaServer`, REST APIs, hooks, process lifecycle |
| `manager/` | `managerkit`: cameras, storage, auth, sync, database entities |
| `ext-codec/` | Optional codec implementations and codec plugins |
| `api/` | Public C API headers and CGo-facing implementation |
| `3rdpart/S3ToolKit/` | `toolkit`: pollers, timers, networking, logging, SQLite helpers |
| `migration/` | Numbered SQLite migrations for ESC and MediaServer databases |
| `tests/` | Unit, integration, and stand-alone test programs |
| `conf/` | Runtime configuration and configuration documentation |
| `docs/` | API and feature documentation, mainly Vietnamese |
| `.agents/skills/` | Shared engineering lifecycle skills |
| `.codex/skills/` | Shared Git safety and S3MediaKit-specific skills |

## Build and validation

Use the existing configured build when available:

```bash
cmake --build build -j2
```

For a fresh Debug build, AWS SDK must already be installed under `3rdpart/aws-sdk/bin/` when `ENABLE_AWS_SDK=ON`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_AWS_SDK=ON
cmake --build build -j"$(nproc)"
```

Choose focused tests when possible, then run the smallest relevant build or test target. Use `tests/README.md` and `docs/README.md` for project-specific setup.

## Project invariants

- Namespaces: `toolkit`, `mediakit`, and `managerkit` have distinct responsibilities; do not duplicate protocol conversion in `ext-codec/` when it belongs in `src/`.
- Singletons use `ClassName::Instance()`.
- Config defaults are registered with `onceToken` and `mINI::Instance()`.
- Database access uses `toolkit::SqlitePool`/manager repositories. Schema changes are new numbered files under the correct migration directory.
- REST handlers use the existing `WebApi` macros and `ApiErrCode`; authentication and internal sync APIs have different trust boundaries.
- `CMAKE_INCLUDE_CURRENT_DIR` is `OFF`; use explicit, correct includes.

## Skill catalog

The repository skill sources are under `.agents/skills/` and `.codex/skills/`. The directories are local/ignored by default; install or copy them to the Codex skills directory when sharing them across projects.

- Shared lifecycle: `requirement-analysis`, `codebase-exploration`, `architecture-design`, `implementation-planning`, `implementation`, `test-design`, `code-review`, `bug-investigation`.
- Shared safety: `shared-safe-git`.
- S3MediaKit: `s3mediakit-development`, `s3mediakit-api`, `s3mediakit-database`, `s3mediakit-media-codec`.

Load only the skill relevant to the task. Read its `references/` files only when the task enters that topic.

## Completion report

End implementation tasks with a concise summary of changed files, validation commands/results, known limitations, and remaining follow-up. For review tasks, report findings first, ordered by severity, with file/line evidence.
