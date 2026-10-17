# Plan xử lý lưu ý sau khi tách poller monitor (f1412fe2)

Commit f1412fe2 cho GlobalMonitor và HealthMonitor chạy chung một luồng `monitor poller 0`
(`monitorPoller()` trong `manager/Server/GlobalMonitor.cpp`) và thêm `process.monitor.collect_ms`
vào `/media/monitor/summary`. Plan này xử lý ba lưu ý còn lại. Mỗi bước là một diff nhỏ, không
đổi hợp đồng schemaVersion 1 (chỉ thêm field/metric, mức `unknown` đã có trong hợp đồng).

## Kết quả kiểm tra code

| # | Lưu ý ban đầu | Thực tế trong code | Còn phải làm |
|---|---|---|---|
| L1 | `statvfs` treo (NFS) làm đứng snapshot health | `HddMonitor` đã gọi `statvfs` trên `BlockingExecutors::api()` (`HddMonitor.cpp:228`), `StorageWriteMonitor` cũng chạy trên `api()`. Trên luồng monitor chỉ còn đọc `/proc`, `/sys` và duyệt bộ nhớ (`DeviceSource::for_each_device`, `getCameraStatisticImp()` chỉ `weak_ptr::lock`); SQLite đều qua `api()` | Không có tín hiệu nào khi chính luồng monitor chậm/kẹt: `requestPollerDelays()` chỉ đo `EventPollerPool` và `WorkThreadPool`; `/health` vẫn trả snapshot cũ, app chỉ thấy qua `generatedAt` |
| L2 | Webhook cảnh báo chạy trên luồng monitor | Đúng: luồng đăng ký làm poller hiện tại (`register_thread=true`), `ResourceMonitor` phát `kBroadcastSystemAlert[Recovered]` trên luồng này, listener ở `WebHook.cpp:1808/1824` gọi `do_http_hook` → `HttpRequester()` không truyền poller → `EventPollerPool::getPoller()` trả luồng monitor; retry `doDelayTask` cũng ở đó | Ảnh hưởng nhỏ (request async, ít; chỉ trễ thêm tối đa `collect_ms`). Cần xác nhận bằng E2E, không cần đổi code |
| L3 | `collect_ms` không tính ghép JSON + `recordHistory` | Đúng: đo xong trước khi dựng `HealthSnapshot` | Thêm thời gian trọn chu kỳ |

## Các bước

### P1. Báo snapshot cũ (L1) — ĐÃ LÀM

Đã làm theo mô tả dưới đây, kèm hook test. `run_cluster.sh <dir> stale` đạt 7/7 bước (bản Debug), thoát sạch. Khác với mô tả: bước 5 không kiểm `rev` tăng, vì `rev` chỉ tăng khi mức hoặc lý do đổi; chỉ kiểm `generatedAt` mới và `stale=false`.

`server/WebApiMonitor.cpp`, `monitorSnapshotResponse()`:

- `age = time(nullptr) - snap->generated_at`. Nếu `age > max(3 × monitor.intervalSec, 30)`:
  `data.stale = true`, `data.snapshotAgeSec = age`, `data.overall = "unknown"`, thêm một reason
  `{component: "process", metric: "monitor.snapshot_age_sec", level: "critical", value: age}`.
  Bình thường trả `stale = false`, `snapshotAgeSec`.
- Với `/health?rev=`: snapshot cũ thì **không** trả `notModified` (rev không đổi vì luồng monitor
  không chạy, app phải thấy `stale`).
- Không đụng `HealthSnapshot`: tính lúc trả lời, trên luồng API, nên vẫn đúng khi luồng monitor kẹt.

Test: unit cho hàm tính `stale` (hàm tĩnh nhỏ trong WebApiMonitor.cpp, không thêm file), và chạy thật
bằng hook test dưới đây.

#### Hook test: làm kẹt luồng monitor một lần

Vì sao cần: không có cách nào từ bên ngoài làm kẹt riêng một luồng. `kill -STOP` dừng cả tiến trình
(API cũng chết nên không đọc được `stale`), còn `monitor.intervalSec` phải restart mới đổi. Unit test chỉ
kiểm hàm tính tuổi, không kiểm được đường thật: Timer trên `monitorPoller()` → snapshot ngừng cập nhật →
luồng API vẫn trả lời và tự tính `stale`.

Cách làm (khoảng 15 dòng trong `HealthMonitor.cpp`):

- Chỉ có trong bản Debug: bọc `#ifdef ENABLE_DEBUG` (CMake định nghĩa ở bản Debug, cùng cơ chế với
  `api-config.json` trong `server/Manager.cpp`). Bản Release không có đoạn code này.
- Bật bằng biến môi trường, không dùng khoá config: khoá config sẽ hiện ra trong `config/get`/`config/set`
  và trong config production (sinh từ default trong code), còn biến môi trường chỉ có khi harness đặt.
  - `S3MK_TEST_MONITOR_STALL_SEC=<giây>`: thời gian kẹt; không đặt hoặc ≤ 0 thì hook tắt.
  - `S3MK_TEST_MONITOR_STALL_AT_CYCLE=<n>` (mặc định 3): kẹt ở chu kỳ thứ n, để trước đó đã có vài
    snapshot tốt làm mốc.
- `HealthMonitor::start()` đọc hai biến một lần (`getenv`) vào thành viên `_test_stall_sec`, `_test_stall_cycle`.
- Đầu `collectNow()`, ngay sau `++_cycle`: nếu `_cycle == _test_stall_cycle` thì
  `WarnL << "TEST HOOK: monitor poller stalls " << sec << " s"` rồi `this_thread::sleep_for(sec)`.
  Chỉ một lần, sau đó chu kỳ chạy bình thường.
- Ngủ ngay trên `monitor poller` là cố ý, giống luồng bị kẹt thật: GlobalMonitor và webhook cảnh báo
  dùng chung luồng nên cũng đứng theo.

Kịch bản `run_cluster.sh <dir> stale` (một node, không cần camera), kiểm bằng `check_monitor.py`:

1. Khởi động node-a với `S3MK_TEST_MONITOR_STALL_SEC=60`, `S3MK_TEST_MONITOR_STALL_AT_CYCLE=3`.
2. Trước lúc kẹt (chu kỳ 1–2): `/health` có `stale=false`, `snapshotAgeSec` < 15. Ghi lại `rev`.
3. Log có `TEST HOOK: monitor poller stalls 60 s`; `generatedAt` ngừng tăng.
4. Khi `snapshotAgeSec` > 30 (khoảng 30–60 s sau lúc kẹt):
   - `/health`: `stale=true`, `overall=unknown`, có reason `process/monitor.snapshot_age_sec` mức critical.
   - `/health?rev=<rev cũ>` **không** trả `notModified`.
   - `/summary` cũng `stale=true`.
   - `/media/mserver/healthcheck` vẫn trả lời trong < 1 s, tức API không chạy trên luồng monitor.
5. Sau khi hết 60 s và thêm một chu kỳ: `stale=false`, `generatedAt` mới, `rev` tăng. Nếu đã làm P2 thì
   `process.monitor.lag_ms` ≈ 50000 ở chu kỳ đó (kẹt trừ một chu kỳ).
6. Đợi bước 5 xong rồi mới SIGINT (dừng trong lúc kẹt thì phải chờ hết giờ ngủ); thoát sạch, không có
   `## signal:` hay `crash.<pid>`.

Nếu không muốn có hook này trong code thì chỉ làm unit test. Khi đó đường Timer → snapshot cũ → API báo
`stale` chưa được chạy thật lần nào.

### P2. Đo độ trễ của chính luồng monitor (L1) — ĐÃ LÀM (phần số đo)

`lag_ms`, `queue_delay_ms` và hai giá trị max đã có trong `process.monitor`; `check_ccu.py` thêm cột
`lag/queue ms max`; `run_cluster.sh <dir> stale` thêm `s1_poller_idle` (lag 0, queue 1 ms) và `s3_lag_seen`
(kẹt 60 s ⇒ `lag_max_ms` 50070). Khác mô tả:
- `lag_ms` sau khi kẹt ≈ thời gian kẹt **trừ một chu kỳ**, vì Timer đặt lịch lần sau từ lúc nó chạy.
- Task thăm dò đặt ở giữa hai chu kỳ (`doDelayTask(interval/2)`) chứ không gửi cùng lúc với
  `requestPollerDelays()`: gửi từ trong `collectNow()` thì chỉ đo phần còn lại của chính chu kỳ đó.
  Thăm dò không bắt được lần kẹt nằm trong một chu kỳ (lag bắt được), chỉ bắt luồng bị việc khác chiếm.
- Metric đánh giá `monitor.collect_ms` + ngưỡng: **chưa làm**, chờ số live500 Release (P5).


`HealthMonitor::collectNow()`:

- Giữ `steady` của lần chạy trước; `process.monitor.lag_ms = (now − prev) − intervalSec × 1000`
  (âm thì 0). Lần đầu `null`.
- Gửi một task rỗng vào `monitorPoller()` cùng lúc `requestPollerDelays()` và đo lúc nó chạy
  (giống `getExecutorDelay` của pool) → `process.monitor.queue_delay_ms`; cho thấy webhook/GlobalMonitor
  chiếm luồng giữa hai chu kỳ.
- Metric đánh giá `process / monitor.collect_ms` với ngưỡng `monitor.collectMsWarn`/`Crit`
  (đề xuất tạm 2000 / 8000 với chu kỳ 10 s), **chỉ bật sau khi có số live500 Release** ở 300 camera;
  trước đó chỉ trả số, không đánh giá.

Test: `unit.health_monitor_all` (ngưỡng mới vào `editableKeys`, kiểm warn < crit); chạy thật 1 node:
`lag_ms` ≈ 0, `queue_delay_ms` vài ms.

### P3. Thời gian trọn chu kỳ (L3) — ĐÃ LÀM

`process.monitor.total_ms_prev` (chu kỳ trước, `null` ở chu kỳ đầu) và `total_max_ms`; `check_ccu.py`
cột `collect ms tb/max / total max`; `run_cluster.sh <dir> stale` thêm `s1_total_cycle`
(`total_max_ms ≥ collect_max_ms`, so cùng chu kỳ không được vì `total_ms_prev` là của chu kỳ trước):
10/10, Debug không camera collect 1–4 ms, total 2,7–4,4 ms. Khác mô tả: đo từ cùng mốc với
`collect_ms` (sau hook test làm kẹt), nên lần kẹt chỉ hiện ở `lag_ms`, không hiện ở `total`.

Mô tả ban đầu:

- Đo thêm từ đầu `collectNow()` tới sau `_snapshot = snap` → `_last_total_ms`; chu kỳ sau ghi
  `process.monitor.total_ms_prev` (của chu kỳ trước, vì chu kỳ hiện tại chưa xong khi ghi JSON).
- `check_ccu.py`: cột `collect ms tb/max` thêm `total max`.
- Không ghi vào `health_metrics` (cần migration mới); live500 đã lấy mẫu `/summary` mỗi 10 s là đủ.

Test: chạy thật 1 node, `total_ms_prev ≥ collect_ms`.

### P4. Xác nhận webhook cảnh báo trên luồng monitor (L2)

- Không đổi code. Chạy `run_cluster.sh <dir> monitor` (đã có bước dừng/bật hook API) và thêm kiểm
  `kBroadcastSystemAlert`: hạ ngưỡng CPU/RAM qua `config/set` để có cảnh báo, xác nhận mock API nhận
  `on_system_alert` và `..._recovered`, kể cả khi hook API tắt rồi bật (retry trên luồng monitor).
- Ghi vào `docs/health-monitor-runbook.md`: webhook cảnh báo hệ thống đi qua luồng `monitor poller`.
- Chỉ đổi sang `register_thread=false` nếu E2E lỗi. Lúc đó `HttpRequester` rơi về event poller như cũ,
  nhưng phải rà mọi chỗ trên luồng monitor gọi `EventPoller::getCurrentPoller()` / `isCurrentThread()`.

### P5. Đo trên máy đích

Dựng lại bundle `release/ccu_test.sh` (make_ccu_bundle.sh), chạy live500. Đọc `collect_ms`, `total_ms_prev`,
`lag_ms`, `queue_delay_ms` ở bậc nghiệm thu rồi chốt ngưỡng P2.

## Thứ tự và phạm vi

P1 → P3 → P2 → P4 (E2E gộp một lượt) → P5. File đụng tới: `server/WebApiMonitor.cpp`,
`manager/Server/HealthMonitor.{h,cpp}`, `tests/unit/manager/test_health_evaluator.cpp`,
`tests/integration/stress_vms/check_ccu.py`, `tests/integration/sync_cluster/check_monitor.py`,
`docs/api-health-monitor.md`, `docs/health-monitor-runbook.md`.

Cần chốt: P1 có thêm hook test (mục "Hook test", chỉ có ở bản Debug) hay chỉ unit test.
