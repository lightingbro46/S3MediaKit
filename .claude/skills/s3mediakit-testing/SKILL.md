---
name: s3mediakit-testing
description: "Use when adding, building or running S3MediaKit tests — GoogleTest unit suites under tests/unit, ctest invocation, coverage gates, or the tier-storage/MinIO integration tests. Also use when deciding what test a change needs."
---

# S3MediaKit testing

GoogleTest + CTest. The top-level `tests/*.cpp` programs are legacy demos and benchmarks,
built only with `ENABLE_LEGACY_TESTS=ON` — do not add new coverage there.

## Test build

`build/` has `ENABLE_TESTS=OFF`, so tests need their own directory:

```bash
cmake -S . -B build-test \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DENABLE_LEGACY_TESTS=OFF \
  -DENABLE_COVERAGE=OFF \
  -DENABLE_AWS_SDK=OFF
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure
```

`ENABLE_AWS_SDK=OFF` keeps the test build fast; turn it on only when the change touches
tier/object storage. Narrow the run while iterating:

```bash
cmake --build build-test --target unit_storage -j"$(nproc)"
ctest --test-dir build-test --output-on-failure -R '^unit\.storage'
ctest --test-dir build-test -N            # list without running
```

Requires `libgtest-dev` (GTest ≥ 1.10). GoogleMock is optional and linked when present.

## Adding a unit test

Suites live in `tests/unit/<area>/` — `common codec console extension http manager onvif
record rtcp rtmp rtsp server storage`. Register in `tests/unit/CMakeLists.txt`:

```cmake
add_s3_unit_test(unit_<area> <area>/test_thing.cpp)
target_link_libraries(unit_<area> PRIVATE S3MediaKit::ext-codec)   # only if needed
```

`add_s3_unit_test` links `GTest::gtest_main` and `S3MediaKit::MediaKit`, adds `manager/` to
the include path, and discovers the cases into ctest under the `unit.` prefix with a **15 s
timeout** — a test that needs real sleeping or a network peer does not belong here. Targets
that need managerkit link `S3MediaKit::ManagerKit` explicitly and some (e.g. `unit_storage`)
also register an aggregate `unit.<name>_all` test; follow whichever pattern the neighbouring
target uses.

Prefer adding a case to the existing suite for the area over creating a new target.

## What to test

Test observable behavior, not coverage percentage. For a typical change:

- Parsing/format code → malformed, truncated and boundary input, not just the happy path.
- Storage entities → round-trip through `*Imp`, defaults, validation rejection, and any
  state transition the change introduces.
- Codec/protocol → readiness, config frames, fragmentation, timestamp jumps.
- Bug fix → a regression test that fails before the fix.

Concurrency and lifetime bugs are usually better caught by `-DENABLE_ASAN=ON` plus a
targeted run than by a timing-dependent test.

## Coverage

```bash
cmake -DBUILD_DIR="$PWD/build-test" -DCOVERAGE_THRESHOLD=40 -P cmake/RunCoverage.cmake
```

Configure with `-DENABLE_COVERAGE=ON` first. Produces Cobertura XML at
`build-test/coverage.xml` and HTML under `build-test/coverage-html/`; exits non-zero below
the threshold. JUnit XML for CI: `cmake -DBUILD_DIR="$PWD/build-test" -P cmake/RunTests.cmake`.

Full details: `docs/unit-testing.md`.

## Tier-storage integration tests

Sources: `tests/integration/tier_storage/` (`test_tier_storage_manager.cpp`,
`test_tier_object_storage.cpp`, `test_object_storage_metrics.cpp`). They need an explicitly
authorized **local MinIO** endpoint:

- Required: `TIER_TEST_S3_ENDPOINT`, `TIER_TEST_S3_ACCESS_KEY`, `TIER_TEST_S3_SECRET_KEY`.
- Optional: `TIER_TEST_S3_BUCKET` (default `s3mediakit-tier-test`).

Never print, log, commit or paste these values. **Stop and ask** if the endpoint or bucket
could be production.

Run via the existing script, which health-checks `/minio/health/live`, builds
`unit_storage integration_tier_storage_minio integration_tier_storage_manager`, and runs the
three ctest selections:

```bash
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

The manager suite runs every scenario in a fresh child process with a temporary SQLite
database and filesystem root; restart recovery uses separate prepare/verify processes sharing
only that temp database. The MinIO suite writes under a unique `agent-runs/<pid>` object
prefix — on failure inspect only that prefix, never delete the bucket or unrelated objects.

Report unit, manager-integration and MinIO results **separately**. A skipped MinIO run is not
a pass — say it was skipped.

Coverage matrix and scenarios: `.codex/skills/tier-storage-testing/references/scenarios.md`,
`docs/tier-storage-integration-testing.md`.
