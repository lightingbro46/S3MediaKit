# Tiến độ sửa V1–V7

Nguồn vấn đề: [REPORT-2026-09-20.md](REPORT-2026-09-20.md) — stress test 300 camera /
600 luồng, ngày 2026-09-20.

Kế hoạch đầy đủ (mô tả gốc rễ từng vấn đề, cách sửa, cạm bẫy đã khảo sát):
`/root/.claude/plans/moonlit-drifting-knuth.md`. File này chỉ ghi **trạng thái**, đọc kèm
kế hoạch đó.

Thứ tự đã chốt: **V1 → V6 → V3 → V4 → V7 → V2 → V5**, mỗi vấn đề một commit độc lập.

---

## Bảng trạng thái

| # | Vấn đề | Trạng thái | Commit |
|---|---|---|---|
| — | Bộ stress test + báo cáo | **xong** | `d4c78308` |
| V1 | Segfault khi camera ONVIF thiếu zoom space | **xong** | `d57429e3` |
| V6 | `keepArchivedMaxForAuto` xoá đoạn ghi đầu tiên | **đang dở** — code + test đã viết, chưa chạy được test | chưa commit |
| V3 | Mỗi thao tác ONVIF tốn hai lượt đi-về | chưa bắt đầu | — |
| V4 | PTZ chết khi vài camera ONVIF chậm | chưa bắt đầu | — |
| V7 | `enforceStoragePolicy()` blocking trên poller | chưa bắt đầu | — |
| V2 | `ENABLE_TIER_STORAGE=OFF` không biên dịch | code đã sửa, **nằm trong working tree chưa commit**; còn CI + tài liệu | chưa commit |
| V5 | fd không trả lại sau failover | chưa điều tra | — |

---

## V1 — đã xong (`d57429e3`)

`manager/Control/OnvifDeviceControl.cpp` / `.h`. Tách `pickPTZProfile()` và
`fillPTZSpaces()` thành static member để unit-test được; kiểm null từng vector, từng phần
tử, từng `XRange`/`YRange`.

Năm chỗ crash gốc đều đã bịt: `Profiles[0]` khi rỗng, `PTZConfigurationOptions`/`Spaces`
deref không kiểm, và ba chỗ zoom space nằm dưới guard của vector pan/tilt.

`pickPTZProfile()` sửa thêm một hạn chế cũ: trước đây chỉ xét `Profiles[0]`, camera liệt kê
profile có PTZ ở vị trí thứ hai sẽ mất PTZ.

**Xác minh đã chạy:**

```
cmake --build build -j2                                     → exit 0
./release/linux/Debug/unit_camera_controller                → 8/8 PASSED
```

Năm case mới ở `tests/unit/manager/test_camera_controller.cpp`
(`OnvifPTZSpacesTest.*`, `OnvifPTZProfilePickTest.*`), trong đó
`SurvivesPanTiltOnlyDeviceAndKeepsMoveEnabled` chính là ca segfault.

**Chưa làm:** phần kiểm thử bằng harness — bật `ptz_omit_zoom_spaces` trong
`onvif_fault_spec.json` rồi chạy `run_stress.sh <work> mid onvif`, node phải sống.

---

## V6 — đang dở, nằm trong working tree

### Đã thay đổi

`manager/Local/StoragePolicy.h` / `.cpp` (**mới**) — ba hàm thuần logic trong namespace
`managerkit`:

- `oldestSegmentDate(record_path, key)` — ngày cũ nhất thực có trên đĩa của một luồng
  (`key` = `"<device_id>/<stream_id>"`), 0 nếu chưa có bản ghi. Chỉ đọc **tên thư mục**, không
  mở file, không đụng SQLite. Tên không parse được thành ngày thì bỏ qua.
- `keepTimeThreshold(min_value, max_value, keep_percent)` — ngưỡng xoá, chịu được trường hợp
  hai mốc bị đảo, kẹp `keep_percent` về [0, 100].
- `clampMaxToDisk(record_path, key, max_value, is_auto)` — kẹp mốc Auto xuống sàn trên đĩa.

**Vì sao tách TU riêng chứ không để trong `StorageManager.cpp`:** kế hoạch ban đầu định để
static member của `StorageManager`, nhưng làm vậy thì test tham chiếu tới `StorageManager::`
khiến linker kéo `StorageManager.cpp.o` vào, và file đó phụ thuộc symbol chỉ có trong
MediaServer (`Manager::kSessionExpiryDays`, `addStreamProxy`, `delStreamProxy`,
`delReplayStreamProxy`, `g_ini_file`). `unit_storage` link `ManagerKit` chứ không link
`server/`, nên hỏng link. TU riêng chỉ kéo theo `StrUtil.cpp` và `toolkit::File`.

`manager/Local/StorageManager.cpp`:

1. `#include "StoragePolicy.h"`, thêm `#include <algorithm>`.
2. `getRecordProfiles()` tính sẵn `record_path`, và kẹp `max_value` **tại chỗ emplace từng
   luồng** (không phải chỗ tính, vì `max_value` tính một lần cho cả camera còn sàn trên đĩa là
   theo luồng). Chỉ áp cho nhánh `keepArchivedMaxForAuto`.
3. `enforceStoragePolicy()` gọi `keepTimeThreshold()` thay cho biểu thức trừ `uint64_t` cũ —
   biểu thức cũ tràn số khi `min_value < max_value`.

`tests/unit/manager/test_storage_policy.cpp` (mới, 124 dòng) — 8 case:
đúng ngày cũ nhất / mỗi luồng có sàn riêng / trả 0 khi không có gì trên đĩa / bỏ qua tên thư
mục không phải ngày / nội suy đúng theo `keep_percent` / không tràn khi hai mốc đảo / kẹp
`keep_percent` ngoài khoảng / hai mốc bằng nhau.

`tests/unit/CMakeLists.txt` — thêm file trên vào target `unit_storage`.

### Việc còn lại của V6

1. `manager/CMakeLists.txt` dùng `file(GLOB)` nên file `.cpp` mới **cần chạy lại cmake**:
   ```bash
   cmake -S . -B build-test
   cmake --build build-test --target unit_storage -j2
   ./release/linux/Debug/unit_storage --gtest_filter='StoragePolicy*'
   ```
2. Commit.

### Cố ý không làm trong V6

Không đụng `CameraStatistic::addArchiveSize`. Ba khiếm khuyết của nó
(`CameraStatistic.cpp:887-891` lọc bỏ khối có `end_time <= created_at`; `:898` gán một lần
không `min()`; `:916` đường trừ nhảy tiến) là gốc rễ thật, nhưng nó còn một consumer khác
(`makeDeviceStorageJson`, `server/Manager.cpp:1512-1524`, chỉ hiển thị) nên tách thành việc
riêng. V6 chỉ coi thống kê là gợi ý và kẹp bằng dữ liệu trên đĩa.

---

## V2 — code đã sửa, chưa commit

`manager/Local/TierFileStorageBase.cpp` đang có +4 dòng trong working tree: bọc cả file trong
`#ifdef ENABLE_TIER_STORAGE`, đúng khuôn `TierStorageBase.cpp`. Không có guard này thì
`-DENABLE_TIER_STORAGE=OFF` không biên dịch.

Đã xác minh `TierNASStorage.cpp` và `TierObjectStorage.cpp` biên dịch sạch khi tier off
(`g++ -fsyntax-only` với đúng cờ của `build-tier-off`) → **không cần thêm guard**.

Còn lại: thêm job CI `-DENABLE_TIER_STORAGE=OFF` build tới target `MediaServer`, và xử lý
lệch tài liệu — `CMakeLists.txt:46` mặc định **ON** còn `docs/api-storage-tiering.md:7`,
`docs/storage-tiering-flow.md:7` ghi **OFF**. Đề xuất sửa tài liệu theo code, nhưng
**cần xác nhận của bạn** trước khi đổi.

---

## V3, V4, V7, V5 — chưa bắt đầu

Xem kế hoạch. Tóm tắt những gì đã xác minh trong lúc lập kế hoạch, để phiên sau không phải
điều tra lại:

- **V3**: `setCredentials()` (`OnvifDeviceControl.cpp:1082-1102`) gọi `getCameraTime()` trước
  **mỗi** thao tác, được gọi từ 42 chỗ. Đo được đúng 50,0% lưu lượng ONVIF trên hai lần chạy
  độc lập. Cache **độ lệch**, không cache `cam_time`. Ba bẫy phải xử lý cùng lúc: `_camTimeInfo`
  chưa có default initializer; hook giữ offset cũ vĩnh viễn khi offset tụt dưới 5 s; và
  `findVideoSegments()` đi tới `setCredentials()` mà **không** giữ `_soap_mtx` → cache phải tự
  bảo vệ bằng `std::atomic<time_t>`.
- **V4**: gốc rễ thật **không phải** chỉ thiếu deadline mà là tải nền tự gây ra —
  `CameraController::onManager()` (`manager/Camera/CameraController.cpp:155-215`) chạy mỗi 10 s
  cho mỗi camera và **không kiểm `_ready`**, nên cứ 60 s lại chạy lại **toàn bộ** `connect()`.
  300 cam × ~16 thao tác × 2 lượt / 60 s ≈ 160 lệnh/s, khớp ~155 lệnh/s đo được.
  Cũng đã xác minh: `runApiCallbackWork` nhả slot in-flight ngay khi `start()` trả về
  (`ApiWorkDispatcher.cpp:344-351`), nên `ApiWorkLimit` hiện **không** chặn được PTZ.
- **V7**: `StorageManager.cpp:405` đẩy vào `WorkThreadPool::Instance().getPoller()->async()`
  trong khi thân hàm làm `statvfs`, quét đệ quy cây thư mục ghi, xoá file, dựng lại timefile và
  hai truy vấn SQLite. Đo được 1.036 ms cho 4.161 file.
- **V5**: mới là **quan sát**, chưa xác nhận là rò rỉ. `node-b` đi 1.353 → 2.436 → 2.440 fd qua
  chu kỳ failover/rebalance và không giảm; số thread giữ nguyên 27 nên không phải rò thread.
  Điều tra trước, chỉ sửa nếu chỉ được đúng chỗ rò; nếu fd bão hoà thì đóng V5 không sửa code.

---

## Lưu ý môi trường (đã trả giá)

- **Chỉ build `-j2`.** Máy ~7,9 GB RAM, `-j6` bị OOM lúc link `MediaServer`.
- **Thư mục output dùng chung.** `build/`, `build-test/`, `build-tier-off/`, `build-sync-off/`
  đều ghi ra `release/linux/Debug/`. Build cấu hình này xong thì phải build lại cấu hình kia
  trước khi chạy binary của nó. Triệu chứng đã gặp: vừa build `build` xong thì link
  `unit_storage` hỏng với `undefined reference to mediakit::l16_plugin` — không phải lỗi mã
  nguồn, build lại `build-test` là hết.
- **`manager/CMakeLists.txt` dùng `file(GLOB)`.** Thêm file `.cpp` mới vào `manager/` phải chạy
  lại `cmake -S . -B <build-dir>`, nếu không ninja không thấy file đó.
- **Test của `unit_storage` không được tham chiếu `StorageManager::`.** Làm vậy kéo
  `StorageManager.cpp.o` vào link, mà file đó phụ thuộc symbol chỉ có trong MediaServer. Logic
  cần test phải nằm ở TU không phụ thuộc `server/`.
- **`unit_camera_controller` không đăng ký từng case trong ctest** — nó là test gộp
  `unit.camera_controller_all`, nên `ctest -R 'OnvifPTZ'` báo "No tests were found". Chạy thẳng
  binary với `--gtest_filter`.
- **Máy host đã ngủ hai lần** giữa lần chạy trước (5554 s + 3564 s), làm hỏng mọi số liệu phụ
  thuộc thời gian của Run B. Trước khi đo lại: tắt sleep/hibernate, và đối chiếu bộ đếm `t=`
  nội bộ trong `rtsp_source.log` để phát hiện nếu vẫn bị.
- **Dữ liệu hai lần chạy cũ** còn ở `/tmp/s3mk_full` và `/tmp/s3mk_storage`. Báo cáo đã trích
  xuất xong, xoá được.

## Tiêu chí đạt sau khi sửa xong

| Hạng mục | Trước | Sau |
|---|---|---|
| Tỉ lệ `GetSystemDateAndTime` | 50,0% | < 10% (V3) |
| Lệnh ONVIF/giây ở trạng thái ổn định | ~155 | giảm rõ rệt (V4.1) |
| PTZ timeout (1 node, 300 cam) | 48/62 = 77% | ≤ 25%, p95 ≤ 15 s |
| Camera Auto mất đoạn đầu | có | không còn |
| Thread chạy chu kỳ dọn kho | `work poller N` | `storage worker N` |
