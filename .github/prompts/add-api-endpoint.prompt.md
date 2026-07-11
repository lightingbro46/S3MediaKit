---
name: add-api-endpoint
description: "Scaffold a new REST API endpoint for the S3MediaKit MediaServer. Generates the api_regist() call with the correct auth guards, CHECK_ARGS_, handler signature, and response pattern — and wires the registration into installWebApi(). Invoke as: /add-api-endpoint"
argument-hint: "Describe the endpoint (path, method, auth, params, sync/async)"
agent: "agent"
---

You are adding a new REST API endpoint to S3MediaKit's MediaServer. Follow the conventions in [server.instructions.md](../../.github/instructions/server.instructions.md) exactly.

## Step 1 — Gather requirements

Ask the user for (or infer from context):

| Field | Options / Notes |
|-------|-----------------|
| **URL path** | `/media/mserver/...` (MediaServer) or `/media/esc/...` (ESC data) |
| **Description** | One sentence — what does it do? |
| **Auth type** | `JWT` (`CHECK_AUTH_TOKEN`) · `secret` (`CHECK_SECRET`) · `cluster` (`CHECK_CLUSTER_AUTHOR_ASYNC`) · `none` (internal/sync API) |
| **Permission code** | Only when auth=JWT: e.g. `READ_MSERVER_PERMISSION_CODE`, `PLAYBACK_PERMISSION_CODE`, etc. |
| **Required params** | Comma-separated list of parameter names |
| **Body format** | `MAP` (flat key-value, default) or `JSON` (nested body) |
| **Async?** | Yes if the handler does I/O, timer work, or calls `invoker` manually |
| **Target file** | Existing `WebApiXxx.cpp` to add to, or a new group name |

If the user has already described the endpoint in their message, extract the fields from it without asking.

## Step 2 — Generate the handler

Produce the `api_regist(...)` call following this template, filled in from Step 1:

```cpp
api_regist("/media/mserver/my/path", [](API_ARGS_MAP_ASYNC) {
    CHECK_AUTH_TOKEN();
    CHECK_USER_PERMISSION(MY_PERMISSION_CODE);
    CHECK_ARGS_("param1", "param2");

    string param1 = allArgs["param1"];
    int    param2 = allArgs["param2"];

    // TODO: implement logic here

    val["data"] = Json::objectValue;  // replace with actual result
    invoker(200, headerOut, val.toStyledString());
});
```

Rules to apply:
- Use `API_ARGS_MAP` (not `_ASYNC`) for purely synchronous handlers — remove `invoker` call and just set `val["data"]`.
- Use `API_ARGS_JSON_ASYNC` when the body is nested JSON — replace `allArgs["key"]` with `allArgs.getArgs()["key"]`.
- `CHECK_AUTH_TOKEN()` must come first — it populates `allArgs["_jwt_token"]` and `allArgs["_user_id"]`.
- `CHECK_USER_PERMISSION(...)` must come immediately after `CHECK_AUTH_TOKEN()`.
- `CHECK_ARGS_("param")` uses the `_` variant (returns `CODE_INVALID_ARGS`) for all new endpoints.
- For async handlers: always add `return;` after every `RETURN_API_RESPONSE(...)` call.
- For cluster/internal APIs: replace the auth block with `CHECK_CLUSTER_AUTHOR_ASYNC(on_access)` pattern.
- For secret-auth APIs: use `CHECK_SECRET()` instead of JWT guards.
- **Never** use bare `API::Success`, `API::NotFound`, etc. as error codes — use `ApiErrCode::CODE_*` from `WebApiErrCode.h`.

## Step 3 — Check for new error codes

If the endpoint needs an error condition not already in `API_ERROR_CODE_MAP`, add an entry following the domain grouping (see [WebApiErrCode.h](../../server/WebApiErrCode.h)):

```cpp
XX(CODE_MY_NEW_ERROR, "Human readable message", 404, 9XXNNN)
```

Where `9XXNNN` follows the existing domain numbering (e.g., `916xxx` if 915 is the last domain used).

## Step 4 — Wire the registration

1. **Existing file**: Add the `api_regist(...)` call inside the existing `registerXxxApis()` function in the correct `WebApiXxx.cpp` file.

2. **New file**: If creating `WebApiXxx.cpp`:
   - Add the file with `#include "WebApi.h"`, `#include "WebApiErrCode.h"`, standard `using namespace` declarations, `namespace managerkit {`, and the `void registerXxxApis() { ... }` function body.
   - Declare `void registerXxxApis();` in [server/WebApi.h](../../server/WebApi.h) alongside the other `void register*Apis()` declarations.
   - Call `managerkit::registerXxxApis();` inside `installWebApi()` in [server/WebApi.cpp](../../server/WebApi.cpp) after the existing registration calls.

## Step 5 — Output summary

After writing the code, print a brief table:

| Item | Value |
|------|-------|
| URL | `/media/mserver/...` |
| Auth | JWT / secret / none |
| Handler | sync / async |
| File | `server/WebApiXxx.cpp` |
| New error codes | Yes / No |
| `installWebApi()` updated | Yes / No |
