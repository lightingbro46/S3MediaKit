---
name: s3mediakit-api
description: "Use when adding, changing, reviewing or debugging S3MediaKit MediaServer REST endpoints — WebApi registration, the API_ARGS_* handler macros, JWT/secret/device authorization guards, request validation, response envelopes, ApiErrCode values, or async invoker handling."
---

# S3MediaKit REST API

Read the endpoint family you are touching before editing: the relevant `server/WebApi*.cpp`,
plus `server/WebApi.h` (macros, guards) and `server/WebApiErrCode.h` (codes).

## Endpoint groups

`installWebApi()` in `server/WebApi.cpp` calls every `registerXxxApis()`. Existing groups and
their files:

| Group | File |
|---|---|
| `registerConfigurationApis` | `WebApiConfig.cpp` |
| `registerMonitorApis` | `WebApiMonitor.cpp` |
| `registerExtractionApis` | `WebApiExtract.cpp` |
| `registerMotionDetectionApis` | `WebApiMotion.cpp` |
| `registerPlaybackApis` | `WebApiPlayback.cpp` |
| `registerBookmarkApis` | `WebApiBookmark.cpp` |
| `registerSyncDbApis` | `WebApiSyncDb.cpp` |
| `registerControlApis` | `WebApiControl.cpp` |
| `registerStorageApis` | `WebApiStorage.cpp` |
| `registerHistoricalSDCardSyncApis` | `WebApiSdCardSync.cpp` |

Add to an existing group when the endpoint fits it. A new group means: new `WebApiXxx.cpp`,
declaration in `WebApi.h`, and a call added to `installWebApi()`.

## Handler macros

| Macro | Use for |
|---|---|
| `API_ARGS_MAP` | flat query/form parameters, synchronous response |
| `API_ARGS_MAP_ASYNC` | flat parameters, response produced later via `invoker` |
| `API_ARGS_JSON` | nested JSON body, synchronous |
| `API_ARGS_JSON_ASYNC` | nested JSON body, asynchronous |
| `API_ARGS_STRING` / `_ASYNC` | raw string body |

`API_ARGS_VALUE` forwards `sender, headerOut, allArgs, val` when you delegate to a helper.

## Guard order

Keep this order — later guards read values the earlier ones inject:

```cpp
CHECK_AUTH_TOKEN();          // JWT → allArgs["_jwt_token"], "_user_id", "_user_name", "_project_id"
CHECK_USER_AUTHOR("id");     // or a group-specific permission check
CHECK_ARGS_("field", ...);   // required parameters, throws with CODE_INVALID_ARGS
```

- `CHECK_AUTH_TOKEN()` is a no-op when `Manager::kEnableAuthorize` is off; it defines
  `enable_authorize` and `token_cache`, both of which later guards use.
- `CHECK_USER_DEVICE_AUTHOR_ASYNC(device_id, cb)` is the async device check. It broadcasts
  `kBroadcastDeviceAccess` and runs `cb` only on success. **Do not throw inside its scope** —
  the surrounding hook scope swallows exceptions; use `RETURN_API_RESPONSE` instead.
- `CHECK_SECRET()` is the legacy secret + IP-allowlist path, used by admin/local endpoints.
- `CHECK_ARGS` (no underscore) throws without a specific code; prefer `CHECK_ARGS_`.

## Responses

- Success: fill `val["data"]`. The framework supplies `code`/`msg`.
- Failure: `RETURN_API_RESPONSE(ApiErrCode::CODE_*, msg)` — it sets `code`/`msg`, maps to an
  HTTP status and calls `invoker`. **Return immediately after it** in async handlers.
- Every async handler calls `invoker(...)` **exactly once** on every path, including error and
  early-exit paths. A missed call hangs the client until timeout; a double call is a bug.
- Use an existing `ApiErrCode::CODE_*`. Add a new one only when no existing code can express
  the failure, and give it a default message in `WebApiErrCode.cpp`.

## Trust boundaries

Internal cluster/sync endpoints (`WebApiSyncDb.cpp` and friends) deliberately skip JWT: they
rely on network isolation plus cluster authorization. Do not add or remove a guard on one
endpoint without tracing its whole family and the threat model it was written against. Say so
explicitly in your report if you change one.

## Threading

Handlers run on the HTTP session's poller; the `_ASYNC` suffix only defers the response, it
does not move work off that thread. Anything blocking — SQLite, disk, S3, ffmpeg, device calls
— must be dispatched through `server/ApiWorkDispatcher.h` (`dispatchApiWork`, `runApiWork`,
`runApiCallbackWork`), which runs it on `BlockingExecutors::api()` and answers from the request
poller. Capture request values by value (never `allArgs`/`val`/`headerOut` by reference, never
raw `this`). Never decide from request data whether to dispatch. Heavy groups take an
`apiWorkLimit("<group>")` for backpressure (503 `CODE_SERVER_BUSY`).
See `docs/threading-blocking-work.md`.

## Checklist before finishing

Auth rejected / permission denied / missing required parameter / success shape /
domain error codes / async timeout and cancellation / exactly-one-invoker on every branch.
Update `docs/api-*.md` when the contract changes.

Handler examples and guard details: `.codex/skills/s3mediakit-api/references/api-patterns.md`.
