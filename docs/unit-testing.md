# Unit testing

The automated test suite uses GoogleTest and is registered with CTest. The
existing programs under `tests/` are legacy examples and benchmarks; enable
them separately with `ENABLE_LEGACY_TESTS`.

## Local run

GoogleTest must be installed on the build image (`libgtest-dev` on Ubuntu).

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

## Jenkins JUnit report

CMake 3.21 or newer can emit JUnit XML directly:

```bash
cmake -DBUILD_DIR="$PWD/build-test" -P cmake/RunTests.cmake
```

Publish `build-test/test-results/*.xml` with the Jenkins `junit` step. Put the
publisher in `post { always { ... } }` so failed tests are still reported.

```groovy
post {
  always {
    junit testResults: 'build-test/test-results/*.xml',
          allowEmptyResults: false,
          keepLongStdio: true
  }
}
```

## Coverage

Configure with `-DENABLE_COVERAGE=ON`, run the tests, then create reports:

```bash
lcov --capture --directory build-test --output-file build-test/coverage.info
lcov --remove build-test/coverage.info '/usr/*' '*/3rdpart/*' '*/tests/*' \
  --output-file build-test/coverage.filtered.info
genhtml build-test/coverage.filtered.info \
  --output-directory build-test/coverage-html
```

The repository also provides a reproducible gcovr gate. It includes first-party
code under `src/`, `manager/`, `server/`, `ext-codec/`, and `api/`; it excludes
`3rdpart/`, tests, and generated protobuf sources.

```bash
cmake -DBUILD_DIR="$PWD/build-test" \
      -DCOVERAGE_THRESHOLD=40 \
      -P cmake/RunCoverage.cmake
```

This creates Jenkins-compatible Cobertura XML at `build-test/coverage.xml` and
HTML at `build-test/coverage-html/index.html`. The command exits non-zero when
line coverage is below 40%, so it can be used directly as a pipeline quality
gate.
