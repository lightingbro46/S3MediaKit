# Tiến độ sửa V1–V7

Nguồn vấn đề: [REPORT-2026-09-20.md](REPORT-2026-09-20.md) — stress test 300 camera /
600 luồng, ngày 2026-09-20.

Kế hoạch đầy đủ (mô tả gốc rễ từng vấn đề, cách sửa, cạm bẫy đã khảo sát):
`/root/.claude/plans/moonlit-drifting-knuth.md`. File này chỉ ghi **trạng thái**, đọc kèm
kế hoạch đó.

Thứ tự đã chốt: **V1 → V6 → V3 → V4 → V7 → V2 → V5**, mỗi vấn đề một commit độc lập.
**Cả bảy đã xong, và cả bảy đã được xác minh bằng harness** (2026-09-21): V5 bằng soak 2 chu kỳ,
V1/V3 bằng hai lần chạy `mid`, còn V4/V6/V7 bằng một lần chạy `full storage` 300 camera trên bản
`ENABLE_TIER_STORAGE=OFF` ở đúng `ad08065`. Chi tiết số đo nằm ở từng mục.

---

## Bảng trạng thái

| # | Vấn đề | Trạng thái | Commit |
|---|---|---|---|
| — | Bộ stress test + báo cáo | **xong** | `d4c78308` |
| V1 | Segfault khi camera ONVIF thiếu zoom space | **xong** | `d57429e3` |
| V6 | `keepArchivedMaxForAuto` xoá đoạn ghi đầu tiên | **xong** | `b315c402` |
| V3 | Mỗi thao tác ONVIF tốn hai lượt đi-về | **xong** | `8d897ea5` |
| V4 | PTZ chết khi vài camera ONVIF chậm | **xong** | `68827c1e` |
| V7 | `enforceStoragePolicy()` blocking trên poller | **xong** | `b5f45f6b` |
| V2 | `ENABLE_TIER_STORAGE=OFF` không biên dịch | **xong** | `731fcad4` |
| V5 | fd không trả lại sau failover | **xong** — không rò; sửa một khoản fd giữ vô ích | `6906aece` |

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

**Xác minh bằng harness (2026-09-21, binary `fa39fc4`+):** đạt.

`setup_stress.py` nhận thêm biến môi trường `STRESS_PTZ_OMIT_ZOOM` vì `write_specs()` ghi đè
`onvif_fault_spec.json` ở đầu **mỗi** phase — sửa tay file đó thì cờ sống không quá một phase.

```
STRESS_PTZ_OMIT_ZOOM=10 PHASE_SEC=120 NODES=2 run_stress.sh <work> mid onvif
        -> 12/120 camera không khai báo zoom space; 120/120 thiết bị kết nối,
           122 GetConfigurationOptions, 0 dòng "## signal:", KẾT QUẢ ĐẠT
STRESS_PTZ_OMIT_ZOOM=1  PHASE_SEC=60  NODES=1 run_stress.sh <work> smoke steady
        -> 30/30 camera KHÔNG có zoom space; 356 lệnh PTZ đều code 0, ptz p95 = 8 ms,
           KẾT QUẢ ĐẠT
```

Gọi tay trên camera chỉ có pan/tilt (`cam-0000`) lúc node đang chạy: `LEFT`, `RIGHT`, `UP`,
`DOWN`, **`ZOOM_IN`** và `get_presets` đều trả `code 0`. Trước khi sửa, chính đường
`GetConfigurationOptions` này giết tiến trình ở `OnvifDeviceControl.cpp:401`.

---

## V6 — đã xong (`b315c402`)

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

### Xác minh đã chạy

```
cmake -S . -B build-test && cmake --build build-test --target unit_storage -j2   → exit 0
./release/linux/Debug/unit_storage --gtest_filter='StoragePolicy*'               → 9/9 PASSED
./release/linux/Debug/unit_storage                                                → 54/54 PASSED
cmake -S . -B build && cmake --build build -j2                                    → exit 0
```

**Xác minh bằng harness (2026-09-21, binary `ad08065`, bản `ENABLE_TIER_STORAGE=OFF`):** đạt.

`MEDIASERVER_DIR=/tmp/bin-tier-off PHASE_SEC=420 run_stress.sh <work> full storage`
(300 camera / 600 luồng, 1 node). Quan sát được **5 chu kỳ** chính sách:

| # | Lúc | Cấu hình đang áp | Ngưỡng của 48 luồng `retention_auto` | Đã xoá |
|---|---|---|---|---|
| 1 | 15:09:40 | mặc định theo cohort (**Auto**) | `2026-09-21 00:00:00` — sàn trên đĩa | **0 B** |
| 2 | 15:14:41 | mặc định theo cohort (**Auto**) | `2026-09-21 00:00:00` — sàn trên đĩa | **0 B** |
| 3 | 15:19:41 | `keepArchivedMaxFor = 1 h` | `14:19:40` = now − 1 h | 0 B |
| 4 | 15:24:41 | `keepArchivedMaxFor = 1 h` | `14:24:40` = now − 1 h | 0 B |
| 5 | 15:29:44 | `keepArchivedMaxFor = 0` | `15:29:40` = now | **4,67 GB** |

Chu kỳ 1 chính là chu kỳ đã cắt dữ liệu trong `REPORT-2026-09-20.md` (18,35 MB, 86 luồng mất
~405 KB mỗi luồng). Giờ ngưỡng của mọi luồng Auto bị `clampMaxToDisk()` kẹp về **nửa đêm của
ngày cũ nhất có trên đĩa**, nên không có gì quá hạn.

Đo thêm bằng đĩa chứ không chỉ bằng log: một sampler ghi lại đoạn ghi **sớm nhất còn tồn tại**
của từng luồng, 20 s một lần. Trong cửa sổ Auto còn hiệu lực (15:05 → 15:18:40, phủ chu kỳ 1 và
2): **0/48 luồng `retention_auto` bị đẩy mốc sớm nhất**, và 0/510 luồng của mọi cohort khác cũng
vậy. Phép đo này có răng: ở chu kỳ 5 nó bắt được đúng 510/510 luồng bị đẩy mốc, tức nếu có mất
đoạn đầu ở chu kỳ 1 thì nó đã thấy.

Lưu ý khi đọc lại: `retention_drop` và `retention_purge` **tắt** chế độ Auto (đặt giờ tường minh),
nên chỉ chu kỳ 1–2 kiểm được nhánh `keepArchivedMaxForAuto`; hai pha kia kiểm phần xoá thường.

### Cố ý không làm trong V6

Không đụng `CameraStatistic::addArchiveSize`. Ba khiếm khuyết của nó
(`CameraStatistic.cpp:887-891` lọc bỏ khối có `end_time <= created_at`; `:898` gán một lần
không `min()`; `:916` đường trừ nhảy tiến) là gốc rễ thật, nhưng nó còn một consumer khác
(`makeDeviceStorageJson`, `server/Manager.cpp:1512-1524`, chỉ hiển thị) nên tách thành việc
riêng. V6 chỉ coi thống kê là gợi ý và kẹp bằng dữ liệu trên đĩa.

---

## V3 — đã xong (`8d897ea5`)

### Đã thay đổi

`manager/Control/OnvifDeviceControl.cpp` / `.h` — cache **độ lệch đồng hồ**, không cache
`cam_time`:

- `_clock_offset` / `_clock_offset_at` (`std::atomic<time_t>`, mặc định 0) + ba hàm riêng
  `cacheClockOffset()` / `invalidateClockOffset()` / `clockOffset(time_t&)`.
- `getCameraTime()` tự nạp cache ở **cả hai** nhánh thành công (UTCDateTime và LocalDateTime),
  nên mọi nơi gọi nó đều làm mới cache, kể cả `setCameraTimeManual()`.
- `setCredentials()` lấy độ lệch qua `clockOffset()` — chỉ đi round-trip khi cache rỗng hoặc
  quá `onvif.clockOffsetTtlSec` (mới, mặc định 300 s, đã ghi vào `conf/config.ini`).
  `cam_time` ký digest được **tính lại** mỗi lần bằng `time(nullptr) + offset`.
- Vô hiệu cache ở hai chỗ: `reportError()` khi fault là lỗi xác thực/timestamp, và
  `setCameraTimeManual()` sau khi `SetSystemDateAndTime` thành công (đồng hồ camera vừa đổi
  thì độ lệch đo trước đó hết đúng).

### Ba cái bẫy trong kế hoạch — đã xử lý hết

1. **`_camTimeInfo` chưa khởi tạo** → thêm `= 0` cho `cam_time` và `cam_sys_time`.
2. **Hook giữ offset cũ vĩnh viễn** → thêm `bool enabled` vào `SoapHookContext` và
   `disableSoapHook()`. `soap->fsend` không gỡ lại được an toàn nên hook tự tắt tại chỗ.
   `setCredentials()` giờ **luôn** quyết định: cài/làm mới hook khi ký bằng đồng hồ camera,
   **tắt** hook khi ký bằng đồng hồ máy. Chỗ này còn sửa một lỗi tiềm ẩn thật: hook viết đè
   `wsu:Created` trong khi `soap_wsse_add_UsernameTokenDigest` (bản không `_at`) đã tính digest
   trên mốc khác → digest không khớp.
3. **Khoá** → cache dùng `std::atomic<time_t>`, không giả định caller giữ `_soap_mtx`.

### Chi tiết đáng nhớ

- `GET_CONFIG` nở ra `Broadcast::`, `ReloadConfigTag`, `BroadcastReloadConfigArgs` — đều thuộc
  `mediakit`. File này trước đó **không** có `using namespace mediakit;` nên phải thêm.
- Namespace config đặt tên `OnvifCtrl` chứ không phải `Onvif`: `src/Onvif/Onvif.cpp` đã có
  `namespace Onvif` ở global scope kèm `using namespace mediakit;`, trùng tên sẽ thành
  ambiguous lookup. Khoá vẫn nằm trong section `[onvif]` của `config.ini`.
- `isAuthOrTimestampFault()` cố ý **hẹp**: lỗi mạng hay "ActionNotSupported" không nói gì về
  đồng hồ, vứt cache ở đó là trả lại đúng cái round-trip thừa mà V3 đang bỏ đi.

### Kiểm thử

`clockOffsetFresh()` và `isAuthOrTimestampFault()` để **static public** để test được mà không
cần camera thật (cùng khuôn với `pickPTZProfile()` / `fillPTZSpaces()` của V1). Năm case mới ở
`tests/unit/manager/test_camera_controller.cpp` (`OnvifClockOffsetCacheTest.*`,
`OnvifAuthFaultTest.*`): dùng lại trong TTL / không dùng cache rỗng hoặc TTL <= 0 / vứt cache
khi đồng hồ máy nhảy lùi / nhận đúng nhóm fault xác thực-timestamp / không nhận nhầm fault mạng.

### Xác minh đã chạy

```
cmake --build build -j2                                     → exit 0
cmake --build build-test -j2                                → exit 0
./release/linux/Debug/unit_camera_controller                → 13/13 PASSED
ctest --test-dir build-test -R '^unit\.'                    → 153/153 PASSED
```

**Xác minh bằng harness (2026-09-21, binary `fa39fc4`+):** đạt.

`PHASE_SEC=120 NODES=2 run_stress.sh <work> mid steady` (120 camera / 240 luồng):

| Cửa sổ đo | `GetSystemDateAndTime` / tổng lệnh | |
|---|---|---|
| Riêng phase `steady` (158 s) | **10 / 716 = 1,4 %** | mốc so sánh: 50,0 % |
| Cả lần chạy, gồm 120 lượt connect | 351 / 3.240 = 10,8 % | |

Phải lấy **hiệu số giữa hai report liên tiếp**: `onvif_state.json` là bộ đếm cộng dồn cả lần
chạy, đọc thẳng số cộng dồn thì phần connect của `ramp` (mỗi camera một lượt
`GetSystemDateAndTime` + một `SetSystemDateAndTime`) sẽ làm tỉ lệ cao giả. 10,8 % của cả lần
chạy là đúng như thiết kế: cache chỉ bỏ được round-trip **lặp lại**, không bỏ được lượt đầu.

---

## V4 — đã xong (`68827c1e`)

Ba thay đổi của kế hoạch, làm hết cả ba.

### 1. Không bắt tay lại khi đang kết nối tốt

`manager/Control/OnvifDeviceControl.cpp` / `.h` — hai hàm mới, cả hai `virtual` để fake được:

- `probeConnection()` — một `GetDeviceInformation` đã xác thực. Trả false khi camera không trả lời,
  từ chối credential, **hoặc báo một thiết bị khác** với thiết bị mà `connect()` đã đọc capabilities
  (đổi camera ở cùng địa chỉ IP thì profile/PTZ space cũ không được dùng lại).
- `refreshMediaProfiles()` — chỉ `getMediaProfiles()`, để nhận ra config bị đổi từ web camera mà
  không phải làm lại cả handshake.

`manager/Camera/CameraController.cpp` — `onManager()` khi `_ready && isConnected()` chỉ thăm dò;
`connect()` đầy đủ chỉ chạy khi thăm dò thất bại. Hai phần đắt được giãn ra bằng config riêng:

- `onvif.profileResyncIntervalSec` (mới, 900 s) — đọc lại media profile. **Nhưng** config server
  đang cố đẩy xuống camera thì vẫn retry **mỗi chu kỳ** như trước: `hasPendingProfileConfig()`
  (mới, chạy trên `_poller`) trả true khi có entry ở trạng thái `NEW` hoặc `PROCESSING`.
- `onvif.clockSyncIntervalSec` (mới, 3600 s) — `setCameraTimeManual()`.

**Điểm kế hoạch bỏ sót, đã xử lý:** kế hoạch chỉ nói "thay `connect()` bằng thăm dò", nhưng chu kỳ
cũ còn làm hai việc khác mỗi 60 s. Bỏ hẳn `setCameraTimeManual()` thì đồng hồ camera **không bao giờ
được chỉnh lại** trong suốt thời gian nó còn kết nối (camera nối liên tục nhiều ngày). Vì vậy mới
thêm `clockSyncIntervalSec` chứ không bỏ luôn.

**Đã kiểm, không phải lo:** chu kỳ cũ cũng gọi `MediaGapFillManager::onManager(..., true)` (kết
thúc bằng `fillDevice()`). Gap mới **không** phụ thuộc chu kỳ 60 s này — nó được phát hiện qua
`MediaGapFillManager::onDeviceStateChanged()` (`GenericRtspCameraImp.cpp:347`), còn retry do
`_progress_timer` 5 s và `_retry_timer` nội bộ lo. Nên đường thăm dò không đăng ký lại provider là
đúng.

Tải nền ở trạng thái ổn định: từ ~16 thao tác/camera/phút xuống **1** (`GetDeviceInformation`).

### 2. Ưu tiên lệnh tương tác ở *cả hai* hàng đợi

`manager/Common/SerialTaskQueue.{h,cpp}` — thêm `postFront()`. Deque giờ mang `QueuedTask{task,
priority}` thay vì `Task` trơn, và `schedule(priority)` giao lượt cho pool bằng `async_first()` thay
vì `async()`. Cần cả hai vì `SerialTaskQueue` chỉ tuần tự hoá **trong một camera**, còn nút thắt
thật là hàng đợi FIFO chung của `BlockingExecutors::device()`.

`CameraController::postPriorityDeviceTask()` dùng cho PTZMove, ImageMoveControl, RelayOutputControl,
addUserPTZPreset, PTZGotoPreset (cả hai nhánh), và `postDeviceTaskAfter()` (lệnh stop tiếp nối của
continuous move). `onManager()` giữ `post()` thường.

**Một bug tôi tự tạo ra rồi sửa — đáng nhớ:** bản đầu `postFront()` đẩy thẳng lên đầu deque
(`push_front`). Việc đó biến một chuỗi lệnh tương tác thành **LIFO**: `PTZMove("left")` rồi
`PTZGotoPreset()` gửi liên tiếp sẽ chạy preset trước, camera kết thúc ở chỗ lệnh cũ trỏ tới.
Test cũ `CameraControllerTest.CommandsOfOneCameraRunInOrderWithoutOverlap` bắt được ngay. Bản đúng
chèn **sau các task ưu tiên đang chờ, trước mọi task nền** (`find_if` tìm task nền đầu tiên rồi
`insert`), nên lệnh ưu tiên giữ FIFO với nhau.

### 3. Có trần và có 503

`server/ApiWorkDispatcher.cpp` — nhóm `device.control` (`maxInflight` 16, `maxQueueMs` 10000).

Quan trọng hơn: **slot in-flight giờ được giữ đến khi request được trả lời**. Trước đây
`runApiCallbackWork` nhả slot ngay khi `start()` trả về, mà `start()` chỉ đẩy việc sang hàng đợi
thiết bị rồi về ngay — nên `maxInflight` đếm số lần dispatch, không chặn được gì (đúng như kế hoạch
đã xác minh). Cách làm: tách thân `runApiWork` thành `runApiWorkHolding()`, truyền cho `work` một
`shared_ptr<void>` giữ hai slot; `runApiCallbackWork` cho lambda `reply` bắt nó. Reply bị huỷ mà
không gọi thì slot vẫn được trả lại lúc destroy.

`server/WebApiControl.cpp` — sáu route (`ptz_control`, `goto_preset`, `set_preset`, `remove_preset`,
`imageMoveControl`, `relayOutputControl`) gộp vào một hàm `runDeviceCommand()` chung, thay cho sáu
khối copy-paste gần như giống nhau (−230 dòng). Nó cũng sửa luôn điểm vệ sinh luồng của kế hoạch:
`cb(...)` của PTZ trước đây gọi thẳng `invoker(...)` **từ thread `device worker`**; giờ mọi câu trả
lời đi qua request poller.

Giữ nguyên có ý thức: `set_preset` / `remove_preset` vẫn **không** kiểm ownership (như trước);
token ownership vẫn chỉ là phép kiểm "không ai khác đang điều khiển" rồi nhả ngay, nên lệnh tới cùng
một camera vẫn xếp hàng chứ không bị từ chối. `get_presets` vẫn đồng bộ trên request poller — chỉ
đọc cache preset, không có round trip ONVIF nào.

### Kiểm thử

11 case mới:

- `tests/unit/manager/test_serial_task_queue.cpp` (3): ưu tiên chạy trước task nền đã xếp hàng /
  chen được cả hàng đợi chung của pool / **các task ưu tiên giữ đúng thứ tự của chúng** (case chặn
  regression LIFO ở trên).
- `tests/unit/manager/test_camera_controller.cpp` (4): camera đang ready thì bị thăm dò chứ không
  bắt tay lại (`{Probe, SetCameraTime, RefreshProfiles}`, không có `Connect`) / thăm dò thất bại thì
  quay về `connect()` đầy đủ và vẫn publish lỗi / chưa từng ready thì không thăm dò /
  `hasPendingProfileConfig()` chỉ true với `NEW` và `PROCESSING`.
- `tests/unit/server/test_api_work_dispatcher.cpp` (4): slot được giữ đến lúc reply (request thứ hai
  bị 503) / slot được trả lại khi reply bị huỷ / mặc định của nhóm `device.control`.

`FakeOnvifControl` thêm override `connect/probeConnection/refreshMediaProfiles/setCameraTimeManual`
(phải cho `setCameraTimeManual()` thành `virtual`), và `CameraControllerTestPeer` thêm
`onManager/setReady/hasPendingProfileConfig`.

**Đã xác minh test có răng:** tạm thay `async_first` bằng `async` → `PostFrontJumpsTheSharedPoolQueue`
fail đúng như mong đợi, rồi phục hồi.

### Xác minh đã chạy

```
cmake --build build -j2                                  → exit 0
cmake --build build-test -j2                             → exit 0
./release/linux/Debug/unit_blocking_executors            → 7/7 SerialTaskQueue PASSED
./release/linux/Debug/unit_camera_controller             → 17/17 PASSED
./release/linux/Debug/unit_api_work_dispatcher           → 26/26 PASSED
ctest --test-dir build-test -R '^unit\.'                 → 156/156 PASSED
```

**Xác minh bằng harness (2026-09-21, binary `ad08065`, bản `ENABLE_TIER_STORAGE=OFF`):** đạt.

Cùng lần chạy với V6: `full storage`, 300 camera / 600 luồng trên **một** node,
`blocking.deviceThreads` giữ nguyên, 12/300 camera bị tiêm độ trễ ONVIF 8 s.

| Phase | ptz n | p50 | p95 | p99 | max | timeout | ONVIF/giây | lệnh/camera/phút |
|---|---|---|---|---|---|---|---|---|
| `steady` | 1.852 | 1,3 ms | 6.620 ms | 15.721 ms | 38.759 ms | 25/1.877 = **1 %** | 9,7 | 1,94 |
| `retention` | 2.245 | 2,5 ms | 5.012 ms | 18.768 ms | 48.329 ms | 20/2.265 = **1 %** | 12,7 | 2,54 |
| `purge` | 5.456 | 3,8 ms | 1.359 ms | 14.082 ms | 46.345 ms | **0**/5.456 = 0 % | 15,2 | 3,03 |

Mốc so sánh của Run B (`REPORT-2026-09-20.md`): `ptz_control` **không hoàn thành trong 120 s**,
48 timeout / 62 lệnh = 77 %. `check_stress.py` báo `KẾT QUẢ: ĐẠT (0 bất biến bị vi phạm)`, với
healthcheck p95 ≤ 48 ms ở mọi phase.

Lưu lượng ONVIF ở trạng thái ổn định: **9,7 lệnh/giây** cho 300 camera, so với ~155 của lần đo
trước — và con số này đã **bao gồm** tải PTZ của bộ sinh tải (5.456 lệnh REST trong phase
`purge`), nên phần nền còn thấp hơn nữa.

Phần đuôi còn lại là do thiết kế của kịch bản chứ không phải hàng đợi bị lấp: 4 % số camera trả
lời sau 8 s, và đúng ~4–5 % số lệnh PTZ nằm trên ngưỡng p95 — lệnh gửi tới **chính** camera chậm
vẫn phải chờ nó, vì `SerialTaskQueue` tuần tự hoá theo từng camera. Khác hẳn triệu chứng cũ, khi
lệnh tới camera **bình thường** (`cam-0000`, `cam-0001`) cũng chết.

### Cố ý không làm trong V4

Không thêm deadline cho chính hàng đợi thiết bị. Sau ba thay đổi trên, một lệnh PTZ chỉ còn xếp sau
tối đa 16 lệnh tương tác khác chứ không sau ~300 tác vụ bảo trì; thêm deadline nữa là thêm một chỗ
có thể bỏ lệnh mà chưa có số đo nào chứng minh cần.

---

## V7 — đã xong (`b5f45f6b`)

### Pool thứ năm

`manager/Common/BlockingExecutors.{h,cpp}` — `storage()` / `storageThreadCount()` /
`blocking.storageThreads` (mặc định **1**), đúng khuôn 4 pool sẵn có. Thread tên `storage worker N`.

Một thread là cố ý: vừa đủ, vừa là một lớp bảo vệ nữa chống hai chu kỳ chạy chồng nhau.

### Chu kỳ bảo trì chuyển hết sang pool đó

`manager/Local/StorageManager.{h,cpp}`. Timer 300 s trên `_poller` giờ **chỉ xếp việc**:

```
_poller (timer 300s) -> postMaintenanceCycle(true)
storage worker       -> enforceStoragePolicy()      (chỉ khi ENABLE_TIER_STORAGE=OFF)
                     -> removeExpiredUserSession()
                     -> cleanupTemporaryFiles()
```

**Kế hoạch chỉ nói tới `enforceStoragePolicy()`, nhưng hai việc kia còn tệ hơn.** Đọc mới thấy:
`removeExpiredUserSession()` (một DELETE SQLite) và `cleanupTemporaryFiles()` (quét + xoá đệ quy
nhiều thư mục, `cleanCrashFiles()`) chạy **thẳng trên `_poller`**, không qua bất kỳ dispatch nào —
trong khi `enforceStoragePolicy()` ít nhất còn được đẩy sang một poller khác. Cùng một khiếm khuyết,
cùng một hàm, nên sửa cả ba.

Thêm hai chỗ nữa cùng loại:

- `cleanupTemporaryFiles()` được gọi trong **constructor** của singleton, nên nó chạy trên bất kỳ
  thread nào chạm `Instance()` đầu tiên — là một poller với mọi caller không phải đường khởi động.
  Đã bỏ khỏi ctor; `start()` xếp một lượt `postMaintenanceCycle(false)` ngay lúc khởi động để giữ
  đúng hành vi cũ (dọn lúc start, không phải chờ 300 s).
- Listener `kBroadcastRebuildTimeFile` đẩy `rebuildTimeFile()` (quét cây thư mục + ghi lại timefile)
  vào `WorkThreadPool::Instance().getPoller()->async()`. Đã chuyển sang pool `storage`.
  **Không có chỗ nào emit event này** (grep cả repo: chỉ có khai báo ở `src/Common/config.*`), nên
  đổi không rủi ro.

Sau thay đổi, `StorageManager.cpp` không còn include `Thread/WorkThreadPool.h`.

### Hai lỗi phụ phát hiện trong lúc sửa

1. **Đua dữ liệu trên `_ticker`.** `enforceStoragePolicy()` và `cleanupTemporaryFiles()` dùng chung
   member `_ticker` (`resetTime()` rồi `elapsedTime()`), mà chúng chạy trên **hai thread khác nhau**
   (một WorkThreadPool poller và `_poller`) trong cùng một tick timer. Đã đổi thành `Ticker` local
   trong từng hàm; member `_ticker` bị bỏ.
2. **Chu kỳ chồng nhau.** Không có gì chặn timer 300 s xếp thêm chu kỳ khi chu kỳ trước còn chạy —
   mà trên kho lớn một chu kỳ có thể dài hơn 300 s. Thêm cờ `_cycle_running`
   (`std::atomic<bool>`): chu kỳ trước chưa xong thì chu kỳ này **bị bỏ** kèm `WarnL`, chứ không xếp
   hàng (xếp hàng chỉ làm backlog phình ra). Cờ nhả bằng guard `shared_ptr` nên task kết thúc kiểu
   nào cũng nhả.

`enforceStoragePolicy()` thêm `CHECK(!EventPoller::getCurrentPoller(), ...)` để chặn hồi quy.

### Kiểm thử

`tests/unit/manager/test_blocking_executors.cpp` — bổ sung vào 3 case sẵn có: pool `storage` là
singleton và khác cả 4 pool kia / thread tên `storage worker` và **không** phải poller /
`storageThreadCount() == 1`.

Không viết unit test cho `postMaintenanceCycle()`: `StorageManager` là singleton ctor private, và
gọi được nó trong test thì `cleanupTemporaryFiles()` sẽ xoá thật các thư mục theo config của test.
Phần đó để harness.

### Xác minh đã chạy

```
cmake --build build -j2                                      → exit 0
cmake --build build-tier-off --target s3managerkit -j2       → exit 0 (nhánh ENABLE_TIER_STORAGE=OFF,
                                                                nơi enforceStoragePolicy() thật sự được gọi)
cmake --build build-test -j2                                 → exit 0
./release/linux/Debug/unit_blocking_executors                → 10/10 PASSED
ctest --test-dir build-test -R '^unit\.'                     → 156/156 PASSED
```

**Xác minh bằng harness (2026-09-21):** đạt, lấy từ chính lần chạy `full storage` của V4/V6 —
kịch bản dựng `www/` riêng trong thư mục làm việc nên không đụng `release/linux/Debug/www`.
Toàn bộ **304** dòng log của `enforceStoragePolicy` / `recreateTimeFile` /
`removeExpiredUserSession` / `cleanupTemporaryFiles` đều mang thread `storage worker`; dòng duy
nhất mang thread khác là `BlockingExecutors.cpp:46 | storage worker pool started with 1 threads`.
Chu kỳ xoá 4,67 GB chiếm **3 s 862 ms** — trước V7 chừng đó thời gian nằm trên một `EventPoller`.

---

## V2 — đã xong (`731fcad4`)

`manager/Local/TierFileStorageBase.cpp` — bọc cả file trong `#ifdef ENABLE_TIER_STORAGE`, đúng
khuôn `TierStorageBase.cpp`. Không có guard này thì `-DENABLE_TIER_STORAGE=OFF` không biên dịch.

`TierNASStorage.cpp` và `TierObjectStorage.cpp` biên dịch sạch khi tier off → **không cần guard**.

**Lệch tài liệu:** đã sửa docs theo code (bạn chốt). `docs/api-storage-tiering.md:7` và
`docs/storage-tiering-flow.md:7` giờ ghi mặc định **ON**, khớp `CMakeLists.txt:46`.

**CI:** repo **không có CI nào cả** — không `.github/workflows`, không gitlab-ci, không Jenkinsfile.
Kế hoạch ghi "thêm job CI" nhưng không có chỗ để thêm. Bạn chốt: chỉ ghi tài liệu, không dựng CI.
Đã thêm `docs/storage-tiering-flow.md` §16 "Kiểm Cấu Hình Tier-Off".

### Hai cạm bẫy phải trả giá mới ra, đã ghi vào §16

Lần link tier-off đầu tiên hỏng hai lần, **cả hai đều không phải lỗi mã nguồn**:

1. **Thư mục output dùng chung, lừa được cả stale-check.** Mọi build dir ghi thư viện ra
   `release/linux/Debug/`. Build cấu hình A xong rồi build B, thì B thấy `libs3managerkit.a` **mới
   hơn** các `.o` của chính nó → coi archive đã cập nhật, **bỏ qua bước archive**, rồi link đúng
   archive chứa object của A. Triệu chứng: `undefined reference to vtable for
   Aws::S3::Model::HeadObjectRequest`. Đây là bản chính xác hơn của ghi chú cũ (không chỉ là
   "binary cũ" mà là link sai cấu hình).
   Cách chẩn đoán không phá huỷ: object của chính build-tier-off có **0** symbol AWS chưa định
   nghĩa, object cùng tên của `build/` có **41**.
   Cách xử lý: `find build-tier-off -name '*.o' -exec touch {} +` rồi build lại — archive lại mà
   không phải biên dịch lại.
2. **`file(GLOB)` trong `manager/CMakeLists.txt`.** `build-tier-off/` được cấu hình từ trước khi V6
   thêm `manager/Local/StoragePolicy.cpp`, nên file đó không được biên dịch →
   `undefined reference to managerkit::clampMaxToDisk / keepTimeThreshold`. Phải
   `cmake -S . -B build-tier-off ...` lại. Build dir ít dùng là chỗ dễ bị nhất.

Cả hai đều là thứ một job CI sẽ tự bắt. Chừng nào chưa có CI, hai bước đó là phần bắt buộc của
việc kiểm.

### Xác minh đã chạy

```
cmake -S . -B build-tier-off -DCMAKE_BUILD_TYPE=Debug -DENABLE_TIER_STORAGE=OFF -DENABLE_AWS_SDK=OFF
find build-tier-off -name '*.o' -exec touch {} +
cmake --build build-tier-off --target MediaServer -j2   → exit 0, MediaServer 244 MB
cmake --build build -j2 (sau khi touch .o)              → exit 0, MediaServer 269 MB (bản AWS)
ctest --test-dir build-test -R '^unit\.'                → exit 0, 155 test
```

---

## V5 — đã xong (`6906aece`)

### Kết luận: KHÔNG rò fd

Soak 4 chu kỳ `failover`↔`rebalance` (`mid`, 2 node, `PHASE_SEC=120`). node-b lặp giữa đúng hai
mức, không tích luỹ chút nào:

| phase | fd cuối | socket | file |
|---|---|---|---|
| steady | 556 | 126 | 398 |
| failover_1 / 2 / 3 / 4 | 1069 / 1065 / 1059 / 1064 | ~244 | ~787 |
| rebalance_1 / 2 / 3 / 4 | 728 / 723 / 728 / 725 | ~124 | ~570 |

Không bất biến nào trượt. Theo đúng tiêu chí kế hoạch ("fd bão hoà thì kết luận là trạng thái ổn
định mới"), V5 đóng ở phần rò rỉ.

### Quan sát gốc của V5 được đo sai

`check_stress.py` tính `fds_peak` trên **cả lần chạy** chứ không cắt theo phase, nên dãy
1.353 → 2.436 → 2.440 trong báo cáo là **đỉnh tích luỹ** — đại lượng về bản chất không bao giờ
giảm, và không nói được gì về việc fd có được trả lại. Đã sửa: `resource_summary(work, since)` cắt
theo mốc phase, thêm `fds_start`/`fds_end`, và phân loại fd theo đích symlink.

### Truy được phần dư đến từng file

Phần dư +170 fd sau `rebalance` so với `steady`: đo trực tiếp trong `rebalance_3` (sau khi lắng 95 s)
cho thấy node-b giữ **274 fd cho 60 camera đang sở hữu** và **172 fd cho 60 camera đã chuyển đi** —
3 fd mỗi camera: `info.txt`, `<ngày>.s3db`, `<ngày>.idx`. Phần phải nhả thì đã nhả đúng: socket về
nền (123 so với 126), camera đã chuyển đi không còn fd `.mp4` nào.

Nguyên nhân là **nghiệp vụ**: camera chuyển đi vẫn ở trong danh sách (`enableActive=false`) tới khi
retention xoá hết dữ liệu. Log: `Keep device in list` **1.980** lần, `Remove device out of list`
**0** lần, lý do `has not been released from cluster` — `failoverActiveDelaySec` còn chưa được xét.

### Phần đã sửa: `record.timefileIdleCloseSec`

Hai handle timefile không cần: camera inactive không ghi gì, mà
`TimeRecorderManager::removeRecorder()` — hàm duy nhất nhả `TimeRecorder` — **không có chỗ nào gọi
trong toàn bộ repo**, nên writer của file hôm nay bị pin suốt đời tiến trình.

Cách sửa khớp với thiết kế sẵn có: `FileWriter` **đã có** `last_access`, và đã có
`evictIdleImportWritersLocked()` cho writer file cũ (LRU, tối đa 2) — chỉ writer của file hôm nay là
bị pin. Thêm:

- `TimeRecorder::closeIdleWriters(idle_ms)` — đóng writer không ghi gì quá `idle_ms`, **kể cả file
  hôm nay**; bỏ qua writer đang tham gia rebuild (cùng điều kiện loại trừ như eviction).
- `TimeRecorderManager::closeIdleWriters()` — đọc `record.timefileIdleCloseSec` (mới, 600 s), copy
  danh sách recorder ra ngoài lock rồi gọi từng cái (đóng file chậm, không được giữ lock mà
  `addBlock()` cần).
- Gọi từ chu kỳ bảo trì kho (pool `storage` của V7).

**Vì sao an toàn:** `TimeMuxer::openFile` dùng mode `"ab+"` và `setLastOffset(File::fileSize(...))`
với chú thích "set current offset to write correct time index after restart server" — mở lại là
append, vốn đã được thiết kế cho việc đó. Không mất dữ liệu.

**Một điểm tôi sửa trong lúc làm:** bản đầu dùng `asyncCloseMuxer` như eviction. Nhưng đóng bất đồng
bộ mà còn dữ liệu trong buffer trong khi writer mới mở lại cùng file thì `setLastOffset()` đọc kích
thước cũ. Đã chuyển sang đóng **đồng bộ** — hợp chỗ vì hàm này chạy trên pool `storage`, còn
eviction phải bất đồng bộ vì nó chạy trong `inputBlock()` trên đường ghi.

Cũng đặt `last_access` ngay lúc tạo writer: trước đó writer mới tạo có `last_access == 0`, sẽ trông
như idle vĩnh viễn với hàm mới (và luôn là victim đầu tiên của eviction).

### Cải tiến harness (dùng lại cho mọi lần đo sau)

- `run_stress.sh`: sampler phân loại fd theo đích symlink; kịch bản `soak` lặp
  `failover`↔`rebalance` `CYCLES` lần (mặc định 4).
- `check_stress.py`: `resource_summary(work, since)` + `phase_start()`; in `fd đầu→cuối` và fd theo
  loại.
- `make_report.py`: bảng tài nguyên có `fd đầu→cuối`; bảng mới "fd theo loại"; `ordered()` xếp phase
  theo thứ tự chạy thật (đọc `phases.jsonl`) thay vì theo tên — nếu không thì `failover_*` bị gom
  một chỗ và mất diễn biến chu kỳ.
- `README.md`: mục kịch bản `soak` và cách đọc số fd cho đúng.

### Kiểm thử

`tests/unit/manager/test_time_file.cpp` — `TimeRecorderIdleCloseTest.ClosesWriterWithoutLosingData`:
ngưỡng 0 không đóng gì / chưa idle thì không đóng / idle thì đóng đúng 1 và lần hai không còn gì /
**block ghi trước lúc đóng vẫn đọc được sau khi mở lại** (đây là ca quan trọng: nếu `openFile`
truncate thì mất index cả ngày).

### Xác minh đã chạy

```
NODES=2 PHASE_SEC=120 CYCLES=4 run_stress.sh /tmp/s3mk_soak mid soak  → exit 0, không failure
cmake --build build -j2                                               → exit 0
cmake --build build-test -j2                                          → exit 0
./release/linux/Debug/unit_storage --gtest_filter='TimeRecorderIdle*'  → 1/1 PASSED
ctest --test-dir build-test -R '^unit\.'                              → 157/157 PASSED
```

### Phần fd thứ ba: `info.txt` — hoá ra là rò thật (`ac1cfb2b`)

Lần đầu tôi kết luận `info.txt` là "thông tin camera nghiệp vụ muốn giữ, để nguyên". **Sai.** Đọc
`FileRecorder` mới thấy nó **không giữ fd nào cả**: `save_string()` mở `ofstream` với `trunc`, ghi,
đóng; `load_string()` cũng vậy. Fd đó là handle **bị rơi** trong constructor:

```cpp
if (!toolkit::File::fileExist(file_path)) {
    toolkit::File::create_file(file_path, "wb+");   // fopen() rồi trả FILE*, bị bỏ
```

`File::create_file` được gọi vì nó tạo sẵn các thư mục cha (`mkdir` vòng lặp), nhưng nó cũng `fopen`.
Mỗi file record tạo mới rò đúng 1 fd suốt đời tiến trình — 1 cho `info.txt` mỗi camera, 1 mỗi
speaker, 1 mỗi bản ghi audio file (3 chỗ dùng `FileRecorder`). Đã sửa bằng `fclose`, kèm test đếm fd
của tiến trình trỏ tới đường dẫn; đã kiểm test bắt được lỗi (bỏ `fclose` → fail 0 vs 1).

Vậy cả 3 fd mỗi camera bị giữ lại đều đã xử lý: 2 handle timefile (idle close) + 1 handle rò
(`fclose`).

### Xác nhận bằng soak sau khi sửa

`setup_stress.py` nay đặt `record.timefileIdleCloseSec=30` cho node của harness (mặc định 600 s là
quá dài so với một phase), và `PHASE_SEC` phải ≥ 330 s để mỗi phase chứa ít nhất một chu kỳ bảo trì
(timer 300 s trong `StorageManager::start()`).

```
NODES=2 PHASE_SEC=330 CYCLES=2 run_stress.sh /tmp/s3mk_soak2 mid soak  → exit 0, không failure
```

node-b: `steady` 491 → `rebalance_1` 493 → `rebalance_2` 493. **Phần dư +170 fd còn +2 và phẳng
hoàn toàn giữa hai chu kỳ**; fd loại `file` 336 → 338 → 338. Mức nền `steady` cũng giảm 556 → 491,
đúng bằng phần handle `create_file()` không còn rò cho 60 camera mỗi node. Đỉnh khi failover
1.069 → 944/963. Log `Closed 51 idle time file writer(s) of 56 device(s)` trên thread
`storage worker` xác nhận cả cơ chế idle-close và pool của V7.

Số liệu đầy đủ ở `REPORT-2026-09-20.md`, mục "Xác nhận sau khi sửa".

---

## Lưu ý môi trường (đã trả giá)

- **Chỉ build `-j2`.** Máy ~7,9 GB RAM, `-j6` bị OOM lúc link `MediaServer`.
- **Thư mục output dùng chung — tệ hơn ghi chú cũ.** Nó lừa được cả stale-check: build cấu hình A
  xong rồi build B, thì B thấy `libs3managerkit.a` **mới hơn** các `.o` của chính nó → coi archive
  đã cập nhật, **bỏ qua bước archive**, rồi link đúng archive chứa object của A. Triệu chứng khi
  kiểm tier-off ngay sau khi build `build/`: `undefined reference to vtable for
  Aws::S3::Model::HeadObjectRequest`. Cách xử lý: `find <build-dir> -name '*.o' -exec touch {} +`
  rồi build lại (archive lại mà không phải biên dịch lại). Chi tiết ở
  `docs/storage-tiering-flow.md` §16.
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

| Hạng mục | Trước | Tiêu chí | Đo được 2026-09-21 |
|---|---|---|---|
| Tỉ lệ `GetSystemDateAndTime` | 50,0% | < 10% (V3) | **1,4%** ở phase `steady` |
| Lệnh ONVIF/giây ở trạng thái ổn định | ~155 | giảm rõ rệt (V4.1) | **9,7/giây** (đã gồm tải PTZ) |
| PTZ timeout (1 node, 300 cam) | 48/62 = 77% | ≤ 25%, p95 ≤ 15 s | **1%**, p95 6,6 s |
| Camera Auto mất đoạn đầu | có | không còn | **0/48 luồng** qua 2 chu kỳ Auto |
| Thread chạy chu kỳ dọn kho | `work poller N` | `storage worker N` (V7) | **304/304 dòng** `storage worker` |
| Node sống khi camera không có zoom space | segfault | sống | **30/30 camera**, PTZ đều `code 0` |
