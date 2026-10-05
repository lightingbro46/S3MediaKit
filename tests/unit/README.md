# Unit test

Bộ unit test dùng GoogleTest (GTest ≥ 1.10, gói `libgtest-dev`) và đăng ký với CTest.
Mọi test có tiền tố `unit.` và nhãn `unit`. Các chương trình `tests/*.cpp` ở cấp trên là
demo/benchmark cũ, không thuộc bộ này.

Tài liệu gốc: [docs/unit-testing.md](../../docs/unit-testing.md).

## 1. Build

`build/` có `ENABLE_TESTS=OFF`, nên test dùng thư mục build riêng `build-test/`:

```bash
cmake -S . -B build-test \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DENABLE_AWS_SDK=ON
cmake --build build-test -j2
```

- Máy ít RAM (~7 GB) nên dùng `-j2`; số job cao dễ bị OOM lúc link.
- `ENABLE_AWS_SDK=OFF` build nhanh hơn, dùng khi thay đổi không đụng tới tier/object storage.
- Một số target chỉ có khi bật cờ tương ứng: `unit_sync_apply` cần `ENABLE_SYNC_CHANGES`,
  `unit_storage_pool_api` và `unit_tier_txn_lock` cần `ENABLE_TIER_STORAGE`,
  `unit_extract_preview` cần `ENABLE_SERVER_LIB`.
- Thêm file `.cpp` mới vào `manager/` thì phải chạy lại bước `cmake -S . -B build-test`
  trước khi build, nếu không sẽ lỗi link.
- File chạy được ghi ra `release/linux/Debug/unit_*` (thư mục output dùng chung giữa các
  thư mục build Debug).

Chỉ build một target khi đang sửa một vùng:

```bash
cmake --build build-test --target unit_storage -j2
```

## 2. Chạy test

```bash
# Toàn bộ unit test
ctest --test-dir build-test --output-on-failure -L unit

# Liệt kê test mà không chạy
ctest --test-dir build-test -N

# Lọc theo tên (regex)
ctest --test-dir build-test --output-on-failure -R '^unit\.storage'

# Chạy song song
ctest --test-dir build-test --output-on-failure -L unit -j2

# Chạy lại các test vừa lỗi
ctest --test-dir build-test --output-on-failure --rerun-failed
```

Chạy trực tiếp một binary để lọc tới từng case GTest:

```bash
release/linux/Debug/unit_storage --gtest_list_tests
release/linux/Debug/unit_storage --gtest_filter='TimeFile*'
release/linux/Debug/unit_storage --gtest_filter='TimeFile*' --gtest_repeat=50 --gtest_break_on_failure
```

Lưu ý:

- Mỗi test có timeout 15–60 s (đặt trong `CMakeLists.txt`).
- Các binary in log codec ra stdout khiến `gtest_discover_tests` không thấy case nào, nên được
  đăng ký thêm thành một test gộp `unit.<tên>_all` chạy cả binary. Khi một test `_all` lỗi,
  xem output để biết case nào hỏng.
- `unit_psia_control`: nhóm `PsiaMockTest` chỉ chạy khi đặt
  `PSIA_MOCK_ADDR=ip:port` (trỏ tới `tests/integration/stress_vms/psia_mock.py`), nếu không
  sẽ skip.
- `console/test_player_restore.js` là test JS cho player, không qua CTest:
  `node tests/unit/console/test_player_restore.js`.

## 3. Xuất báo cáo kết quả (JUnit XML)

Dùng script có sẵn (cần CMake ≥ 3.21):

```bash
cmake -DBUILD_DIR="$PWD/build-test" -P cmake/RunTests.cmake
```

Kết quả: `build-test/test-results/unit.xml`. Lệnh trả mã khác 0 khi có test lỗi.

Tương đương gọi ctest trực tiếp:

```bash
mkdir -p build-test/test-results
ctest --test-dir build-test --output-on-failure -L unit \
  --output-junit "$PWD/build-test/test-results/unit.xml"
```

Báo cáo XML chi tiết từng case của một binary (do GTest sinh):

```bash
release/linux/Debug/unit_storage --gtest_output=xml:build-test/test-results/unit_storage.xml
```

Lưu thêm log đầy đủ của lần chạy:

```bash
ctest --test-dir build-test --output-on-failure -L unit \
  --output-log build-test/test-results/ctest.log
```

Trên Jenkins, publish trong `post { always { ... } }` để vẫn có báo cáo khi test lỗi:

```groovy
post {
  always {
    junit testResults: 'build-test/test-results/*.xml',
          allowEmptyResults: false,
          keepLongStdio: true
  }
}
```

## 4. Báo cáo coverage

Bật instrument coverage rồi build và chạy lại test:

```bash
cmake -S . -B build-test -DENABLE_COVERAGE=ON
cmake --build build-test -j2
find build-test -name '*.gcda' -delete          # xoá số liệu của lần chạy trước
ctest --test-dir build-test -L unit
```

Sinh báo cáo bằng gcovr (đi kèm ngưỡng tối thiểu):

```bash
cmake -DBUILD_DIR="$PWD/build-test" -DCOVERAGE_THRESHOLD=40 -P cmake/RunCoverage.cmake
```

Kết quả:

| File | Nội dung |
|---|---|
| `build-test/coverage.xml` | Cobertura XML cho Jenkins |
| `build-test/coverage-html/index.html` | Báo cáo HTML |

Script chỉ tính mã trong `src/`, `manager/`, `server/` (loại `3rdpart/`, `tests/`, file
`.pb.*`) và trả mã khác 0 khi line coverage dưới ngưỡng.

Nếu dùng lcov thay cho gcovr:

```bash
lcov --capture --directory build-test --output-file build-test/coverage.info
lcov --remove build-test/coverage.info '/usr/*' '*/3rdpart/*' '*/tests/*' \
  --output-file build-test/coverage.filtered.info
genhtml build-test/coverage.filtered.info --output-directory build-test/coverage-html
```

Xong thì tắt lại để build test bình thường không chậm vì `-O0 --coverage`:

```bash
cmake -S . -B build-test -DENABLE_COVERAGE=OFF
```

## 5. Thêm test mới

Đặt file vào thư mục theo vùng (`common codec extension http manager onvif record rtcp rtmp
rtsp server storage`) và đăng ký trong [CMakeLists.txt](CMakeLists.txt):

```cmake
add_s3_unit_test(unit_<vùng> <vùng>/test_xxx.cpp)
target_link_libraries(unit_<vùng> PRIVATE S3MediaKit::ext-codec)   # nếu cần
```

Ưu tiên thêm file vào target sẵn có của vùng đó (`target_sources(...)`). Nếu target link
`ext-codec`, thêm test gộp như các target bên cạnh:

```cmake
add_test(NAME unit.<vùng>_all COMMAND unit_<vùng>)
set_tests_properties(unit.<vùng>_all PROPERTIES LABELS unit TIMEOUT 15)
```

Test cần MinIO/S3 thật nằm ở `tests/integration/tier_storage/`, xem
[docs/tier-storage-integration-testing.md](../../docs/tier-storage-integration-testing.md).
