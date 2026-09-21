# Tiến độ sửa V1–V7

Nguồn vấn đề: [REPORT-2026-09-20.md](REPORT-2026-09-20.md) — stress test 300 camera /
600 luồng, ngày 2026-09-20.

Kế hoạch đầy đủ (mô tả gốc rễ từng vấn đề, cách sửa, cạm bẫy đã khảo sát):
`/root/.claude/plans/moonlit-drifting-knuth.md`. File này chỉ ghi **trạng thái**, đọc kèm
kế hoạch đó.

Thứ tự đã chốt: **V1 → V6 → V3 → V4 → V7 → V2 → V5**, mỗi vấn đề một commit độc lập.
**Cả bảy đã xong.** Phần còn lại là xác minh bằng harness cho V1, V3, V4, V6, V7 và V5 (xem từng mục).

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

**Chưa làm:** phần kiểm thử bằng harness — bật `ptz_omit_zoom_spaces` trong
`onvif_fault_spec.json` rồi chạy `run_stress.sh <work> mid onvif`, node phải sống.

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

**Chưa làm:** xác minh bằng harness — chạy pha `retention_drop` / `retention_purge` và kiểm
không còn file nào của camera Auto bị xoá sớm.

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

**Chưa làm:** xác minh bằng harness — chạy `mid steady` rồi đối chiếu `onvif_state.json`, tỉ lệ
`GetSystemDateAndTime / tổng lệnh` phải tụt từ ~50% xuống < 10%.

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

**Chưa làm:** xác minh bằng harness — chạy `full storage` một node, bộ bất biến PTZ trong
`check_stress.py` (p95 ≤ 15 s, timeout ≤ 25%) phải đạt, và lệnh ONVIF/giây ở trạng thái ổn định
phải tụt rõ rệt từ ~155.

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

**Chưa làm:** xác minh bằng harness — log của chu kỳ dọn (`Remove temporary files. Finished.`,
`Finished enforcing storage policy`) phải mang thread `storage worker N` thay vì `work poller N`.
Không chạy thử ở máy này vì `cleanupTemporaryFiles()` xoá thật `www/snap` và `www/extract`
(đang có 142 MB dữ liệu dev trong `release/linux/Debug/www`).

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

**Chưa làm:** chạy lại soak để xác nhận phần dư +170 thật sự về ~0. Cần soak dài hơn ngưỡng 600 s
của một chu kỳ bảo trì, hoặc hạ `record.timefileIdleCloseSec` trong config của harness.

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

| Hạng mục | Trước | Sau |
|---|---|---|
| Tỉ lệ `GetSystemDateAndTime` | 50,0% | < 10% (V3) |
| Lệnh ONVIF/giây ở trạng thái ổn định | ~155 | giảm rõ rệt (V4.1) |
| PTZ timeout (1 node, 300 cam) | 48/62 = 77% | ≤ 25%, p95 ≤ 15 s |
| Camera Auto mất đoạn đầu | có | không còn |
| Thread chạy chu kỳ dọn kho | `work poller N` | `storage worker N` (V7) |
