---
description: "Use when writing code in server/: adding REST API endpoints, registering handlers, checking auth, returning error codes, wiring new WebApi*.cpp files, or handling HTTP request/response in MediaServer. Covers api_regist, CHECK_AUTH_TOKEN, CHECK_ARGS_, RETURN_API_RESPONSE, ApiErrCode, and the API_ARGS_MAP/JSON patterns."
applyTo: "server/**"
---

# server/ – WebApi & MediaServer Conventions

All code in `server/` uses:
```cpp
using namespace std; using namespace toolkit; using namespace mediakit; using namespace managerkit;
```

## Endpoint Registration

Group related endpoints in a dedicated `WebApiXxx.cpp` file. Each file exposes one `void registerXxxApis()` function containing `api_regist()` calls.

**Wiring checklist for a new group:**
1. Create `server/WebApiXxx.cpp` (use an existing file as template)
2. Declare `void registerXxxApis();` forward in `WebApi.h`
3. Call `registerXxxApis()` inside `installWebApi()` in `WebApi.cpp`

URL prefixes:
- `/media/mserver/...` — MediaServer-facing APIs (auth required)
- `/media/esc/...` — ESC (Edge Storage Controller) APIs

## Choosing the Right Handler Signature

| Macro | Body type | Async? | Use when |
|-------|-----------|--------|---------|
| `API_ARGS_MAP` | flat key-value | No | Simple sync query |
| `API_ARGS_MAP_ASYNC` | flat key-value | Yes | Async work (I/O, timers) |
| `API_ARGS_JSON` | `Json::Value` | No | Nested JSON body, sync |
| `API_ARGS_JSON_ASYNC` | `Json::Value` | Yes | Nested JSON body, async |

```cpp
// Sync — framework calls invoker(200,...) after return
api_regist("/media/mserver/foo", [](API_ARGS_MAP) {
    CHECK_AUTH_TOKEN();
    val["data"] = someValue;
});

// Async — you call invoker() explicitly
api_regist("/media/mserver/bar", [](API_ARGS_MAP_ASYNC) {
    CHECK_AUTH_TOKEN();
    CHECK_ARGS_("id");
    doSomethingAsync([val, invoker, headerOut]() mutable {
        val["data"] = result;
        invoker(200, headerOut, val.toStyledString());
    });
});
```

## Auth & Permission Guards (order matters)

```cpp
CHECK_AUTH_TOKEN();                                 // 1. Always first — validates JWT
CHECK_USER_PERMISSION(PERMISSION_CODE);             // 2. Check feature permission
CHECK_ARGS_("param1", "param2");                    // 3. Validate required params
```

- `CHECK_AUTH_TOKEN()` populates `allArgs["_jwt_token"]`, `allArgs["_user_id"]`, `allArgs["_user_name"]`, `allArgs["_project_id"]`.
- For per-device resource authorization (async): use `CHECK_USER_DEVICE_AUTHOR_ASYNC(deviceId, callback)` after `CHECK_AUTH_TOKEN()`.
- For internal cluster-to-cluster calls: use `CHECK_CLUSTER_AUTHOR_ASYNC(callback)` — no JWT required.
- **Sync/internal APIs** (e.g., `/media/esc/sync/*`) intentionally **omit** `CHECK_AUTH_TOKEN()` — they are protected by network isolation only. Do NOT add auth guards to these.

Permission code constants (defined in `User/UserSessionCache.h`):
`LIVE_VIEW_PERMISSION_CODE`, `PLAYBACK_PERMISSION_CODE`, `PTZ_CONTROL_PERMISSION_CODE`, `ADD_CAMERA_PERMISSION_CODE`, `EXTRACT_PERMISSION_CODE`, `READ_MSERVER_PERMISSION_CODE`, `MODIFY_MSERVER_PERMISSION_CODE`, `READ_BOOKMARK_PERMISSION_CODE`, `MODIFY_BOOKMARK_PERMISSION_CODE`.

## Response Pattern

**Success:**
```cpp
val["data"] = result;                            // sync: just set val, framework calls invoker
val["data"] = result;
invoker(200, headerOut, val.toStyledString());    // async: call invoker manually
```

**Error mid-handler (non-throw path):**
```cpp
RETURN_API_RESPONSE(ApiErrCode::CODE_DEVICE_NOT_FOUND, "Device not found");
return;   // always add return after RETURN_API_RESPONSE in async handlers
```

**Throw for well-known error types:**
```cpp
throw InvalidArgsException("msg");          // → 400
throw AuthException("msg");                 // → 401
throw ApiRetException("msg", code, 500);    // → custom status
```
Thrown exceptions are caught by the framework; prefer throws for early exits in sync handlers.

## Error Codes

Always use `ApiErrCode` enum values from `server/WebApiErrCode.h` — **never** raw integers.

**Adding a new error code:** append to the `API_ERROR_CODE_MAP(XX)` macro in `WebApiErrCode.h`:
```cpp
XX(CODE_MY_NEW_ERROR, "Human readable message", 404, 9XXNNN)
```
- HTTP status (3rd column): 200/400/401/403/404/409/429/500
- Custom code (4th column): follow the `9XXnnn` domain grouping already established (e.g., 905xxx for device errors, 914xxx for storage tier errors)

## Standard Response Envelope

The framework always wraps responses in:
```json
{ "code": 0, "msg": "", "data": { ... } }
```
You only set `val["data"]`. The `code` and `msg` fields are filled by `RETURN_API_RESPONSE` or the framework.

## Reading Params

```cpp
string id     = allArgs["id"];          // returns "" if missing
int limit     = allArgs["limit"];       // auto-converted via variant
bool flag     = allArgs["flag"];
int64_t ts    = allArgs["timestamp"];
```
For JSON body handlers (`API_ARGS_JSON`), `allArgs["key"]` returns a `Json::Value`.

## Related Files

- `server/WebApi.h` — all macros (`CHECK_AUTH_TOKEN`, `CHECK_ARGS_`, `RETURN_API_RESPONSE`, `API_ARGS_*`, `api_regist` overloads)
- `server/WebApiErrCode.h` — full `API_ERROR_CODE_MAP` and `ApiErrCode` enum
- `server/WebHook.h` / `WebHook.cpp` — event hook registrations (separate from REST API)
- `server/Manager.h` — `Manager::k*` config key constants
