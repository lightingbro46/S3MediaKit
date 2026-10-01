# Theo dõi IO ổ lưu trữ trong GlobalMonitor + sửa vi phạm quy tắc thread từ test CCU

Lập 2026-10-01 trên `develop` `e7a72db5`, từ kết quả test CCU trên db3
(`ccu_result_a1227b5e4336_20260930_105532`, xem mục 11.1 của
[ccu-capacity-test-design.md](ccu-capacity-test-design.md)).

Tài liệu có hai phần độc lập:

- **Phần A — yêu cầu tính năng (đã làm 2026-10-01, nhánh `feature/disk-io-monitor`, xem A.6):** `GlobalMonitor`
  theo dõi mức nghẽn IO của ổ lưu trữ, phát hiện vùng lưu trữ không ghi được (read-only) và bắn cảnh báo lên API quản lý.
- **Phần B — plan sửa:** các chỗ vi phạm quy tắc thread của repo lộ ra trong log của lần test đó.

---

## Phần A — Theo dõi iowait ổ lưu trữ và cảnh báo lên API

### A.1 Vì sao cần

Trên db3 (RAID đĩa quay qua SMC3108, 300 camera), đĩa bão hoà ngay khi chưa có viewer nào:

- vmstat `wa` khoảng 40%, khoảng 18 tiến trình chờ IO, ghi khoảng 287 MB/s;
- có lúc cả MediaServer im 16 s, kéo theo 324 luồng kéo camera timeout, ghi hình hở 10–17 s,
  `database is locked` và replay `data_timeout`.

Lúc đó `GlobalMonitor` vẫn báo mọi thứ bình thường, vì hiện nó chỉ biết:

- CPU, RAM, mạng;
- **dung lượng** đĩa (`HddMonitor`, `statvfs`).

Nó không biết đĩa **đang nghẽn**. Phía vận hành chỉ thấy triệu chứng (camera rớt, mất bản ghi) mà
không có tín hiệu gốc. Máy CentOS 7 (kernel 3.10) còn không có PSI.

### A.2 Hiện trạng liên quan

| Thành phần | Vị trí | Ghi chú |
|---|---|---|
| Ngưỡng + phát cảnh báo | `ResourceMonitor::emitSystemAlert` (`manager/Server/ResourceMonitor.cpp:94`) | `NOTICE_EMIT kBroadcastSystemAlert(type, usage, threshold, is_critical)`, không có ngữ cảnh ổ/mount |
| Webhook | `server/WebHook.cpp:1771` | POST `hook.api_url + hook.on_system_alert` (mặc định `/api/event-rule/system-event`), body `eventCode/eventTime/currentValue/threshold`; `eventCode` theo `ResourceType`: `Cpu*`, `Ram*`, `Disk*`, `NetWork*` |
| Ngưỡng từ API quản lý | `server/Manager.cpp:1041` `GET_THRESHOLD` | `thresholdConfig.{CPU,RAM,STORAGE}_levelLow/_levelMedium`, thiếu thì lấy default `globalmonitor.*` |
| Dung lượng đĩa | `HddMonitor` | refresh 10 s trên `BlockingExecutors::api()`. **Bắn cảnh báo lại mỗi lần refresh, cho mọi partition, không chống lặp**, và body không có mount point |
| Thống kê qua REST | `WebApiMonitor.cpp` → `makeSystemStatisticJson`, `getSystemStatisticHistory` | `disks[]` = `DiskPartition::toJson()` |
| Lưu lịch sử | `SystemMetric.disks_json` (bảng `system_metrics`) | Là JSON, thêm trường không cần migration |

### A.3 Đề xuất thiết kế

**Nguồn số liệu (Linux, đọc procfs, không chặn):**

- `/proc/diskstats`, đọc theo từng block device. Mỗi kỳ tính:
  - `util%` = Δ`io_ticks` / Δt;
  - `await_ms` = Δ(`read_ms` + `write_ms`) / Δ(`reads` + `writes`);
  - `aqu` = Δ`weighted_ms` / Δt;
  - MB/s đọc và ghi.
- `/proc/stat`: tỉ lệ `iowait` toàn máy. Đây là phương án dự phòng thô, đã dùng trong `check_ccu.py`.
- `/proc/pressure/io`: `some`/`full` khi kernel có PSI (≥ 4.20). Có PSI thì ưu tiên tín hiệu này cho mức
  toàn máy.
- Nối mount với device: `DiskPartition.device` (vd `/dev/sdb1`) → `realpath` (`/dev/mapper/x` →
  `dm-N`) → tên trong diskstats. Bỏ qua mount không có block device: tmpfs, overlay, NFS/CIFS. NAS
  qua NFS để giai đoạn sau, có thể dùng `/proc/self/mountstats`.
- Trong docker, `/proc/diskstats` là của host (không theo namespace), còn `/proc/mounts` của container
  vẫn chỉ ra `/dev/sdX` cho bind mount. **Phải kiểm lại trên db3.**

**Thành phần mới:**

- Lớp `DiskIoMonitor : ResourceMonitor` (`ResourceType::DISK_IO = 5`), timer 5 s trên `_poller` của
  `GlobalMonitor`. Đọc procfs không phải IO đĩa nên được phép chạy trên poller.
- Không gộp vào `HddMonitor`: `statvfs` trên NFS treo có thể giữ lượt refresh vô thời hạn, khi đó
  tín hiệu nghẽn IO không được kẹt theo.
- Nhận nguồn số liệu qua một provider tiêm vào (cùng mẫu `HddMonitor::PartitionProvider`) để test
  không cần đĩa thật.
- Chỉ đánh giá các mount **lưu trữ**:
  - mount chứa `record` / `mp4_save_path`;
  - các pool HOT/NAS đã đăng ký trong tier (`TierFileStorageBase` pool `base_path`);
  - mount chứa DB.

  Xem D2.

**Ngưỡng:** default đăng ký bằng `onceToken` trong `GlobalMonitorConfig`, đọc bằng `GET_CONFIG`:

| Key | Mặc định đề xuất | Ý nghĩa |
|---|---|---|
| `globalmonitor.disk_io_sample_interval_sec` | 5 | Chu kỳ lấy mẫu |
| `globalmonitor.disk_io_util_warning_threshold` / `_critical_` | 80 / 95 | % thời gian thiết bị bận |
| `globalmonitor.disk_io_await_warning_ms` / `_critical_` | 50 / 200 | Độ trễ trung bình mỗi IO (RAID đĩa quay) |
| `globalmonitor.iowait_warning_threshold` / `_critical_` | 20 / 40 | % iowait toàn máy, dùng khi không có PSI |
| `globalmonitor.disk_io_sustain_samples` | 3 | Phải vượt liên tiếp N mẫu (khoảng 15 s) mới báo |
| `globalmonitor.disk_io_alert_repeat_sec` | 300 | Đang ở mức vượt thì nhắc lại tối đa mỗi chừng này giây |

`util%` một mình dễ báo oan trên NVMe/RAID có nhiều hàng đợi song song. Vì vậy mức **critical** cần
`util` **và** `await` cùng vượt; mức warning chỉ cần một trong hai.

**Cảnh báo lên API:**

- Thêm broadcast mới `kBroadcastDiskIoAlert`. Không đổi chữ ký `kBroadcastSystemAlert`, vì các listener
  hiện có đang dùng nó.
- Tham số khai báo `const T &` (bài học `NOTICE_EMIT` truyền theo giá trị):
  `mount_point, device, metric ("util"|"await"|"iowait"|"psi"), value, threshold, is_critical, is_main_storage`.
- Listener trong `WebHook.cpp` POST cùng `on_system_alert`:
  - `eventCode` `DiskIoWarning` / `DiskIoCritical`;
  - body thêm `mountPoint`, `device`, `metric`, `isMainStorage`.
- Chống lặp: chỉ bắn khi **đổi mức** (normal→warning→critical) và nhắc lại theo `disk_io_alert_repeat_sec`.
  Có gửi sự kiện hồi phục `DiskIoRecovered` hay không là D3.
- Ngưỡng từ API quản lý: thêm `thresholdConfig.DISK_IO_levelLow/_levelMedium` vào `GET_THRESHOLD`
  (D4).

**Hiển thị và lịch sử:**

- `makeSystemStatisticJson`:
  - mỗi phần tử `disks[]` thêm `io_util_pct`, `await_ms`, `read_mbps`, `write_mbps`, `io_state`;
  - mức toàn máy thêm `iowait_pct`, `psi_io_full_pct` (null nếu không có PSI).
- `SystemMetric.disks_json` lưu các trường mới, không cần migration. Phải kiểm
  `getSystemStatisticHistory` (gộp theo `bucket_sec`) để nó giữ hoặc gộp đúng các trường này.

**Cùng đợt (nên làm):** sửa luôn cảnh báo dung lượng của `HddMonitor` cho cùng mẫu chống lặp và thêm
`mountPoint` vào body. Hiện nó bắn mỗi 10 s cho từng partition vượt ngưỡng.

### A.4 Cần chốt trước khi làm

| # | Câu hỏi | Đề xuất |
|---|---|---|
| D1 | Phía API quản lý có nhận `eventCode` mới và trường `mountPoint`/`device` không? | Hỏi team API trước. Nếu họ chỉ nhận `Disk*` cũ thì tạm dùng `DiskWarning`/`DiskCritical` kèm `metric` |
| D2 | Theo dõi mọi block device hay chỉ mount lưu trữ? | Chỉ mount lưu trữ (record, pool tier, DB) để tránh báo oan từ ổ hệ điều hành |
| D3 | Có gửi sự kiện hồi phục không? | Có (`DiskIoRecovered`), để phía vận hành đóng cảnh báo tự động |
| D4 | Ngưỡng lấy từ `thresholdConfig` của API hay chỉ config.ini? | Cả hai, giống CPU/RAM/STORAGE; tên trường cần team API xác nhận |
| D5 | Khi đĩa critical có từ chối replay hay xuất video mới không? | Chưa làm trong đợt này; chỉ cảnh báo. Ghi nhận làm ý tưởng cho admission (`isReaderResourceLimit`) |

### A.5 Các bước

1. `DiskIoMonitor` + provider + tính toán delta (xử lý bộ đếm quay vòng, device biến mất hoặc mới
   cắm). Unit test bằng provider giả: util/await, nối mount với device (partition, dm), chống lặp, đổi
   mức, hồi phục.
2. Gắn vào `GlobalMonitor::start()`, thêm vào `makeSystemStatisticJson` và `disks_json`. Test JSON.
3. Broadcast + listener webhook + `GET_THRESHOLD`. Test listener nhận đúng tham số (bẫy by-value).
4. Chống lặp cho cảnh báo dung lượng của `HddMonitor`.
5. Kiểm thử thật:
   - chạy lại CCU trên db3, `getSystemStatistic` phải thấy `io_util_pct` cao ở bậc 0;
   - mock API phải nhận `DiskIo*`;
   - harness `check_ccu.py` có thể đọc thẳng các trường này thay cho vmstat.
6. Tài liệu: `docs/` phần monitor/webhook, mô tả payload mới cho team API.

### A.6 Kết quả thực hiện (2026-10-01, nhánh `feature/disk-io-monitor`)

**Đã chốt:**

| # | Chốt |
|---|---|
| D1 | `eventCode` mới `DiskIoWarning` / `DiskIoCritical` / `DiskIoRecovered`, body thêm `mountPoint`, `device`, `metric`, `isMainStorage` |
| D2 | Chỉ mount lưu trữ: thư mục record (`isMainStorage=true`), thư mục DB, base path của pool tier đã đăng ký |
| D3 | Có gửi `DiskIoRecovered` |
| D4 | Ngưỡng **chỉ** lấy từ config `globalmonitor.*`, không đọc `thresholdConfig` của API quản lý |
| D5 | Không đổi: chỉ cảnh báo, không từ chối replay/xuất video |
| Bước 4 | Làm: cảnh báo dung lượng chống lặp + thêm `mountPoint`/`device` |
| Thêm | Yêu cầu bổ sung giữa chừng: cảnh báo vùng lưu trữ không ghi được (read-only filesystem). Phát hiện bằng cờ `ro` của mount **và** ghi thử; `eventCode` `DiskReadOnly` / `DiskWriteFailed` / `DiskWritable` |

**Thành phần:**

- `manager/Server/DiskIoMonitor.{h,cpp}`: timer `disk_io_sample_interval_sec` trên `_poller` của `GlobalMonitor`, đọc
  `/proc/diskstats`, `/proc/stat`, `/proc/pressure/io`. Nối mount với device: `realpath` (`/dev/mapper/x` → `dm-N`;
  trong container thiếu node thì dò `/sys/block/dm-*/dm/name`), kernel không có dòng partition thì lùi về đĩa cha.
  Bộ đếm giảm (quay vòng, device tạo lại) thì bỏ mẫu đó. Mức critical cần `util` **và** `await` cùng vượt.
  Đổi mức cần `disk_io_sustain_samples` mẫu liên tiếp cùng phía; chốt ở mức nhẹ nhất (đi lên) hoặc nặng nhất (đi xuống)
  trong chuỗi đó. Mức toàn máy: PSI `full avg10` nếu kernel có, không thì `iowait` của `/proc/stat`.
- `manager/Server/StorageWriteMonitor.{h,cpp}`: timer theo `mount_refresh_interval_seconds` (10 s) trên poller chỉ để
  gom danh sách; mỗi lần ghi thử (tạo `.s3mk_write_probe`, `write`, `fsync`, `unlink`) chạy trên
  `BlockingExecutors::api()`. Một đường dẫn còn lượt ghi thử đang chạy thì bỏ lượt mới, nên NFS treo chỉ giữ một thread.
  Mount có cờ `ro` thì báo ngay, không ghi. Thư mục record chưa tồn tại thì ghi thử vào thư mục cha gần nhất, không tạo thư mục.
  Đường dẫn đã ghi được từ đầu thì không báo; hỏng ngay từ đầu thì báo.
- `HddMonitor`: cảnh báo dung lượng chuyển sang `kBroadcastDiskUsageAlert`, chỉ bắn khi đổi mức và nhắc lại theo
  `hdd_alert_repeat_sec`; không còn đi qua `kBroadcastSystemAlert`.
- `TierFileStorageBase::registerPool/unregisterPool` gọi `GlobalMonitor::setIoWatchPath("tier:<pool>", path)`.
- Broadcast mới (`src/Common/config.h`), tham số đều là `const T &`: `kBroadcastDiskIoAlert`, `kBroadcastDiskUsageAlert`,
  `kBroadcastDiskWriteAlert`. Listener trong `server/WebHook.cpp`.

**Config mới (`globalmonitor.*`, mặc định đăng ký trong code):**

| Key | Mặc định |
|---|---|
| `disk_io_sample_interval_sec` | 5 |
| `disk_io_util_warning_threshold` / `disk_io_util_critical_threshold` | 80 / 95 |
| `disk_io_await_warning_ms` / `disk_io_await_critical_ms` | 50 / 200 |
| `iowait_warning_threshold` / `iowait_critical_threshold` | 20 / 40 |
| `disk_io_psi_full_warning_threshold` / `disk_io_psi_full_critical_threshold` | 10 / 30 (thêm so với A.3, cho kernel có PSI) |
| `disk_io_sustain_samples` | 3 |
| `disk_io_alert_repeat_sec` | 300 |
| `hdd_alert_repeat_sec` | 300 (nhắc lại cảnh báo dung lượng và cảnh báo không ghi được) |
| `system_alert_sustain_samples` | 3 (CPU/RAM) |
| `system_alert_repeat_sec` | 300 (CPU/RAM/người xem) |

**Tài liệu tích hợp cho service API:** [api-system-alert-events.md](api-system-alert-events.md).

**Payload gửi lên API quản lý** (POST `hook.api_url + hook.on_system_alert`, mặc định `/api/event-rule/system-event`):

```jsonc
// Nghẽn IO một mount
{"eventCode": "DiskIoWarning" | "DiskIoCritical" | "DiskIoRecovered",
 "eventTime": 1759300000, "currentValue": "92.40", "threshold": "80.00",
 "mountPoint": "/data", "device": "/dev/sdb1",
 "metric": "util" | "await",          // await khi độ trễ vượt ngưỡng, ngược lại util; Recovered dùng metric của cảnh báo nó đóng
 "isMainStorage": true}
// Nghẽn IO toàn máy: mountPoint = device = "", metric "psi" | "iowait", isMainStorage false

// Dung lượng (eventCode như cũ, thêm vị trí)
{"eventCode": "DiskWarning" | "DiskCritical", "eventTime": ..., "currentValue": "91.00", "threshold": "90.00",
 "mountPoint": "/data", "device": "/dev/sdb1"}

// Không ghi được
{"eventCode": "DiskReadOnly" | "DiskWriteFailed" | "DiskWritable", "eventTime": ...,
 "path": "/opt/media/www/record", "mountPoint": "/opt/media", "device": "/dev/sdb1",
 "isMainStorage": true, "error": "Read-only file system"}   // error rỗng với DiskWritable
```

**REST `getSystemStatistic`:**

- mỗi phần tử `disks[]` của mount được theo dõi IO thêm `io_util_pct`, `await_ms`, `io_queue_depth`, `read_mbps`,
  `write_mbps` (null khi chưa đủ hai mẫu), `io_device`, `io_state` (`normal|warning|critical`), `is_main_storage`;
- `disk_io`: `iowait_pct`, `psi_io_some_pct`, `psi_io_full_pct` (null khi không có nguồn), `state`;
- `storage_write[]`: `path`, `mount`, `device`, `is_main_storage`, `state` (`writable|read_only|write_failed`), `error`, `checked_at`.

**Lịch sử:** `disks_json` lưu thêm các trường IO, không cần migration. `getSystemStatisticHistory` thêm `disk_io_util_pct`,
`disk_await_ms` (mức tệ nhất trong các mount, gộp trung bình theo `bucket_sec`). `iowait`/PSI toàn máy **không** lưu vào lịch sử.

**Chống lặp cho CPU, RAM, số người xem (làm thêm cùng đợt, theo yêu cầu):**

Trước đây CPU/RAM bắn lại mỗi 10 s khi còn vượt ngưỡng; tổng người xem và người xem theo camera bắn mỗi lần số người
xem đổi. Giờ dùng chung `AlertLevelTracker` (`manager/Server/ResourceMonitor.h`, `DiskIoMonitor` cũng dùng):

- chỉ bắn khi đổi mức, nhắc lại theo `globalmonitor.system_alert_repeat_sec` (mặc định 300);
- CPU/RAM phải ở mức mới `globalmonitor.system_alert_sustain_samples` mẫu liên tiếp (mặc định 3 × 10 s) mới báo;
  số người xem là sự kiện nên báo ngay (sustain 1);
- khi về bình thường gửi sự kiện hồi phục (broadcast mới `kBroadcastSystemAlertRecovered`,
  `kBroadcastStreamReaderAlertRecovered`; chữ ký broadcast cũ giữ nguyên).

| eventCode mới | Body |
|---|---|
| `CpuRecovered`, `RamRecovered`, `LiveStreamLimitRecovered` | `eventTime`, `currentValue`, `threshold` (ngưỡng của cảnh báo được đóng) |
| `CameraStreamLimitRecovered` | như trên + `cameraId` |

**Kiểm thử:** `tests/unit/manager/test_disk_io_monitor.cpp` (trong `unit_storage`): parse procfs, util/await/MB/s, sustain,
critical cần cả hai, nhắc lại, dm/partition, bộ đếm reset, PSI/iowait, chống lặp dung lượng, read-only/ghi lỗi/hồi phục,
ghi thử thật trên thư mục tạm, probe treo không chiếm poller và không bị gửi lại. Mutation check: đổi `&&`→`||` ở mức
critical, bỏ chống lặp dung lượng, bỏ cờ `in_flight` — mỗi cái làm đúng test tương ứng trượt.

**Còn lại:**

- Bước 5 (chạy thật): CCU trên db3 xem `io_util_pct`; kiểm docker map `/dev/sdX` đúng tên trong `/proc/diskstats`;
  mock API nhận `DiskIo*`/`DiskReadOnly`; thử remount `ro` một mount record.
- Báo cho team API các `eventCode` và trường mới ở trên trước khi lên production (gồm cả các sự kiện `*Recovered`).
- NAS qua NFS không có số liệu IO (chỉ có cảnh báo không ghi được).
- Lưu ý: ghi thử tạo file ẩn `.s3mk_write_probe` trong thư mục record mỗi 10 s rồi xoá ngay.

---

## Phần B — Plan sửa vi phạm quy tắc thread

### B.1 Đối chiếu log CCU với `develop`

**Binary chạy trên db3 cũ hơn `develop`.** Nó thiếu 489f98e3, e810a7e3, 546b6a86 và migration 21.
Đối chiếu từng chỗ trong log với code hiện tại:

| Log trên db3 | Thread | Trên `develop` |
|---|---|---|
| `Resource.cpp:133/167` `Failed to record (dis)connection … database is locked` | `work poller N` | **Đã sửa** e810a7e3: `saveStream/DeviceStatusHistory` qua `postEscTask` → `SerialTaskQueue` trên `BlockingExecutors::api()` |
| `EventPoller.cpp:319 Exception … database is locked` sau `Record mp4 file` | `work poller N` | **Đã sửa**: 489f98e3 (listener `kBroadcastRecordMP4` chạy trên `rec close N`) + 546b6a86 (bắt lỗi `registerHotSegmentRange`) |
| `GlobalMonitor.cpp:184 Failed to record system metric: database is locked` | `event poller 11/13` | **Còn nguyên** → B2 |
| `ThreadPool.h:139 … basic_string::_M_construct null not valid` | `storage worker` | Do binary và migration lệch nhau (thiếu bảng `user_auth_sessions`). Bundle mới đã có migration 21; riêng việc lỗi SQL biến thành `std::string(nullptr)` cần điều tra → B5 |
| `SqlitePool.h:338 rollback failed: no transaction is active` | nhiều | Chỉ là nhiễu log → B5 |

### B.2 `GlobalMonitor` ghi SQLite trên event poller (mức cao)

**Hiện trạng**

- `_poller = EventPollerPool::Instance().getPoller()` (`GlobalMonitor.cpp:55`) là một poller mạng đang
  mang phiên RTSP/HTTP.
- `_metrics_timer` (mỗi `metrics_sample_interval_sec` = 10 s) gọi
  `_metrics_store->insertMetric(m)` (`GlobalMonitor.cpp:156`).
- `_cleanup_timer` (3600 s) gọi `deleteOlderThan` (`GlobalMonitor.cpp:201`).
- Cả hai chạy thẳng trên poller đó. Khi DB bị khoá, mỗi lần chặn poller tới `busy_timeout` (3 s); mọi
  phiên trên poller đó khựng theo. Log db3 đã thấy chuyện này 3–4 lần trong mỗi kịch bản.

**Sửa**

1. Thêm `SerialTaskQueue::Ptr _metrics_queue` trên `BlockingExecutors::api()`, tên `"system metrics"`.
   Dùng cùng pool với ESC/`HddMonitor` (D2 của đợt 2). Không dùng `storage()`: pool đó chỉ có 1 thread và
   chu kỳ bảo trì dài sẽ làm trễ mẫu.
2. Timer giữ trên poller, chỉ thu số liệu từ bộ nhớ (các monitor đã có snapshot sẵn) và dựng
   `SystemMetric`. Sau đó `post` phần `insertMetric` vào hàng đợi.
   - Nếu `_metrics_queue->pending() > 0` thì bỏ mẫu này, để DB khựng lâu không làm dồn việc (cùng cách
     timer gap-fill P1 đợt 2).
   - Log InfoL 300 s giữ ở timer.
3. `_cleanup_timer` chỉ `post` `deleteOlderThan` vào cùng hàng đợi. Insert và purge không chạy chồng nhau.
4. Callback giữ `weak_self` như hiện tại. `GlobalMonitor` là singleton nhưng đang dùng
   `enable_shared_from_this`, nên giữ nguyên mẫu đó.

**Test:** `GlobalMonitorTest.MetricsInsertRunsOnBlockingPoolNotPoller`, cùng mẫu
`HddMonitorTest.TimerRefreshBlocksAWorkerNotAPoller`:

- dùng `SystemMetricsImp` trên DB tạm;
- giữ khoá ghi bằng một connection khác;
- khẳng định poller vẫn chạy task trong < 100 ms;
- khẳng định bản ghi được chèn sau khi nhả khoá.

Cập nhật `docs/threading-blocking-work.md` §1.

### B.3 `ResourceStatusManager` giữ mutex toàn cục trong lúc chờ SQLite (mức trung bình)

**Hiện trạng**

- `onDisconnected` / `onReconnected` / `removeResourceStatusGaps` (`manager/Extension/Resource.cpp:100–190`)
  lấy `_mtx` của singleton rồi mới `beginWriteTxn()`.
- Từ e810a7e3, mỗi camera ghi qua hàng đợi ESC riêng trên `api()`, nhưng mọi camera lại dồn về **một**
  mutex này.
- Khi DB khựng, thread `api()` thứ nhất chờ `busy_timeout` 3 s trong lúc giữ `_mtx`; các thread `api()`
  khác xếp hàng chờ mutex. Với 300 camera reconnect cùng lúc, pool `api()` có thể kẹt hết, và REST
  (cũng chạy trên `api()`) chậm theo.
- `cleanupExpiredGaps` (mỗi giờ, theo batch) cũng chạy trong lúc giữ `_mtx`.

**Sửa (cần xác nhận khi làm)**

- Thứ tự theo từng resource đã được hàng đợi ESC của camera bảo đảm. Tính nguyên tử của "tìm gap mở rồi
  thêm/sửa" đã có `BEGIN IMMEDIATE` bảo đảm. Vì vậy `_mtx` chỉ còn cần bảo vệ `_last_cleanup_time`.
  Hướng sửa: thu `_mtx` lại quanh phần đó (hoặc đổi sang `std::atomic`), bỏ khoá ra khỏi đoạn SQLite.
- Trước khi bỏ, rà mọi caller (`rg "ResourceStatusManager::Instance"`) để chắc không còn chỗ nào ghi
  cùng resource từ ngoài hàng đợi ESC.

**Test:** hai thread ghi hai resource khác nhau. Một thread bị chặn bởi khoá ghi giữ ngoài; thread kia
vẫn phải xong trong < `busy_timeout`.

### B.4 Xác nhận lại các mục đã sửa bằng test CCU trên binary `develop` (bắt buộc)

- Build Release từ `develop` (≥ `e7a72db5`), đóng gói lại `release/ccu_test.sh` (bundle hiện tại đã có
  migration 21, HLS trên `/dev/shm`, check iowait), chạy `--suite quick` trên db3 với
  `docker --shm-size` đủ lớn.
- Tiêu chí: trong `stdout.log.gz` không còn dòng `database is locked` nào trên thread tên `event poller`
  hoặc `work poller`:

  ```bash
  zcat */node-a/stdout.log.gz | grep "database is locked" | grep -oE '\[[0-9]+-[a-z ]+' | sort | uniq -c
  ```

  `api worker`, `rec close`, `tier` được phép vì đó là các pool blocking.

### B.5 Mức thấp

| Vị trí | Việc | Gợi ý |
|---|---|---|
| `3rdpart/S3ToolKit` `SqlitePool.h:338` | `~SqliteTransaction` báo `rollback failed: no transaction is active` khi SQLite đã tự rollback (BUSY/IOERR giữa transaction) | Kiểm `sqlite3_get_autocommit()` trước khi `ROLLBACK`. Sửa trong submodule nên phải commit riêng ở S3ToolKit |
| `UserSessionImp::removeByStamp` → executor | Bảng không tồn tại làm `ThreadPool` bắt `basic_string::_M_construct null not valid`, mất thông báo SQL gốc | Tái hiện bằng DB chỉ chạy migration tới 20, tìm chỗ dựng `std::string` từ `const char*` null (thông báo lỗi hoặc cột) |

### B.6 Thứ tự và quy trình

- Nhánh `fix/thread-db-locked` từ `develop`, mỗi mục một commit (tiếng Việt).
- Thứ tự: **B2 → B3 → B5 → B4** (B4 là lần chạy nghiệm thu cuối, gộp với lần chạy lại CCU).
- Mỗi bước:
  - `cmake --build build-test -j2`;
  - `ctest --test-dir build-test --output-on-failure -R unit.`;
  - cập nhật `docs/threading-blocking-work.md`.
- Phần A làm sau B2: `DiskIoMonitor` dùng chung `_poller`/timer của `GlobalMonitor`, nên dựng trên nền
  đã sửa.

### B.7 Trạng thái (2026-10-01, nhánh `fix/thread-db-locked`)

| Mục | Trạng thái | Test |
|---|---|---|
| B2 | Xong: `_metrics_queue` (`SerialTaskQueue` `system metrics` trên `api()`); timer bỏ mẫu khi lượt ghi trước còn chờ; purge cũng qua hàng đợi | `unit_global_monitor_threading` `MetricsInsertWaitsOnBlockingPoolNotPoller` (code cũ: "poller blocked") |
| B3 | Xong: bỏ `_mtx` khỏi `onDisconnected/onReconnected/removeResourceStatusGaps`; `_cleanup_mtx` (`try_lock`) chỉ giữ phần dọn gap | `unit_media_gap_fill_threading` `LockedDatabaseCostsEachStatusWriteOneBusyTimeout`: 4 lần ghi khi DB khoá xong trong ~3 s (code cũ: chờ nối nhau, trượt) |
| B5a | Xong, **không phải ở submodule**: gốc là `beginWriteTxn` (`DbStorage.h`) chạy `COMMIT; BEGIN IMMEDIATE`; `BEGIN IMMEDIATE` hết `busy_timeout` thì transaction đã bị `COMMIT`. Sửa: mở lại `BEGIN` (deferred) trước khi ném lỗi để destructor `ROLLBACK` được | `unit_tier_txn_lock` `BeginWriteTxnTimeoutReportsLockAndRecovers` (chỉ khẳng định lỗi là "locked" và dùng lại được; dòng log cảnh báo không kiểm bằng test) |
| B5b | Xong ở submodule S3ToolKit: `queryString` ném `SqliteException(fmt, …)` thay vì `sqlite3_sql(NULL)` | `unit_tier_txn_lock` `PrepareFailureKeepsSqliteError` (code cũ: "basic_string: construction from null") |
| B4 | Chưa: cần binary Release từ nhánh này chạy `ccu_test.sh --suite quick` trên db3 | — |
