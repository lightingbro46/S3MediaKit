---
name: s3mediakit-api
description: "Use when adding, changing, reviewing, or debugging S3MediaKit MediaServer REST endpoints, WebApi registration, JWT/secret/cluster authorization, request validation, response envelopes, or API error codes."
---

# S3MediaKit REST API

Read the relevant existing `server/WebApi*.cpp`, `server/WebApi.h`, and `server/WebApiErrCode.h` before editing. Combine this skill with `code-review` for endpoint reviews.

## Endpoint workflow

1. Establish URL domain (`/media/mserver/...` or `/media/esc/...`), HTTP method, request body shape, auth/trust boundary, permission, sync/async behavior, and target registration group.
2. Reuse an existing `registerXxxApis()` group when possible. For a new group, create `WebApiXxx.cpp`, declare the function in `WebApi.h`, and call it from `installWebApi()` in `WebApi.cpp`.
3. Choose the handler macro accurately: `API_ARGS_MAP` for synchronous flat parameters, `API_ARGS_MAP_ASYNC` for flat parameters with asynchronous completion, and the corresponding `API_ARGS_JSON` variants for nested JSON bodies.
4. For JWT endpoints, keep guard order: `CHECK_AUTH_TOKEN()`, permission guard, then `CHECK_ARGS_(...)`. Use the project async authorization macros for device/cluster checks.
5. Set `val["data"]` for success. Async handlers call `invoker(...)` exactly once and return after every `RETURN_API_RESPONSE(...)`.
6. Use `ApiErrCode::CODE_*` from `WebApiErrCode.h`; add a domain-appropriate error code only when an existing code cannot express the failure.
7. Test auth, permission, required parameters, success, error, async timeout/cancellation, and response shape as applicable.

## Trust boundaries

Internal sync APIs may intentionally omit JWT because they rely on network isolation and cluster authorization. Do not add or remove guards without tracing the existing endpoint family and threat model.

See [api-patterns.md](references/api-patterns.md) for handler examples and guard details.
