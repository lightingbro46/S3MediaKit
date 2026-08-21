# S3MediaKit architecture reference

## Important boundaries

- `toolkit` supplies concurrency and infrastructure; it does not own media protocol policy.
- `mediakit` owns streaming/media behavior; `managerkit` coordinates devices, persistence, auth, and storage policy.
- `server` adapts manager/core behavior to process lifecycle and HTTP APIs.
- `api` is a compatibility boundary. Avoid exposing C++ implementation types through public C headers.

## Common checks

- CMake declares C++11 and `CMAKE_INCLUDE_CURRENT_DIR` is `OFF`.
- AWS SDK is copied/linked from `3rdpart/aws-sdk/bin/` when `ENABLE_AWS_SDK=ON`.
- Relevant tests live under `tests/unit/` and stand-alone programs under `tests/`.
- Runtime configuration is under `conf/`; API and feature docs are under `docs/`.
