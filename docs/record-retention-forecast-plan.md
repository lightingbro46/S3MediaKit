# Plan: dự báo dung lượng giữ bản ghi + throughput ghi hình (HOT / thư mục record)

Ngày lập: 2026-10-03. Trạng thái: **đã làm** P1–P7 trên nhánh `feature/record-retention-forecast` (2026-10-03),
Q1–Q5 chốt theo đề xuất. Kết quả và chỗ khác plan ở mục 8.

## 1. Mục tiêu

1. Đo tốc độ ghi hình thật (demand) từ `kBroadcastRecordMP4` (`RecordInfo::file_size` của từng segment đã đóng),
   cho cả hai bản build:
   - `ENABLE_TIER_STORAGE=OFF`: tổng tốc độ ghi vào thư mục record (`protocol.mp4_save_path` + `record.appName`);
   - `ENABLE_TIER_STORAGE=ON`: tốc độ ghi vào từng pool HOT và tổng của tầng HOT.
2. Dự báo dung lượng (hướng c): với tốc độ hiện tại, vùng lưu trữ giữ được bao nhiêu ngày, còn bao lâu thì chạm
   ngưỡng bắt đầu xoá/chuyển tầng, và **có đủ chỗ cho thời gian lưu đã cấu hình hay không**. Không đủ thì cảnh báo.
3. Bổ sung số liệu vào API:
   - `systemStatistic` (cả `/media/mserver/...` và `/media/api/...`): throughput ghi vào thư mục record (tier OFF)
     hoặc tổng throughput ghi vào tầng HOT (tier ON);
   - `storage/dashboard/summary`: throughput + dự báo theo từng tier trong `tier_summary[]` (thay placeholder
     `estimated_remaining_days = 0`);
   - `storage/dashboard/detail`: throughput theo từng pool (`pools[]`) và theo từng tier (`summary.tiers`).

Ngoài phạm vi: cảnh báo "ghi không kịp" cho HOT/record (không đo được, xem mục 2); dự báo cho WARM/COLD
(chỉ hiển thị throughput đã có, không dự báo).

## 2. Vì sao chọn hướng này

- Ghi hình `fwrite` đồng bộ trên poller, không có hàng đợi, nên không có backlog để đo "năng lực < demand" như
  `PoolThroughputLow`. Đĩa nghẽn đã có `DiskIoMonitor` (đã chạy thật 2026-10-03, cả bản tier OFF).
- `MediaSource::getBytesSpeed()` chỉ được cộng ở vài loại source (`RtspMediaSourceImp`, `RtmpMediaSourceImp`, FMP4,
  TS, HLS), là tốc độ tức thời ~1 s và tính cả overhead giao thức. `file_size` của segment là số byte thật xuống đĩa.
- Bản tier ON đã đo đúng cách này: listener `kBroadcastRecordMP4` (`TierStorageManager.cpp:8469`) →
  `registerHotSegmentRange` → `PoolThroughputTracker::addIngest(camera, hot_pool, bytes)`. Bản tier OFF chưa có gì
  (toàn bộ `TierStorageManager.cpp` nằm trong `#ifdef ENABLE_TIER_STORAGE`).

## 3. Hiện trạng liên quan

| Thành phần | Vị trí | Ghi chú |
|---|---|---|
| Đo ingest HOT | `PoolThroughputTracker::addIngest` (`manager/Local/PoolThroughputTracker.cpp:125`) | Meter theo camera (`_cameras`) và theo pool (`PoolState::ingest`); bucket theo thời gian, cửa sổ `storage.pool_throughput_window_seconds` |
| Kết quả ingest | `PoolThroughputStats::record_ingest_mbps` | Chỉ tính lại trong `evaluate()` mỗi tiering cycle (300 s) |
| `PoolThroughputTracker.cpp` | không có guard tier | Dùng lại được ở bản tier OFF |
| Retention tier OFF | `StorageManager::enforceStoragePolicy` + `getRecordProfiles` (`manager/Local/StorageManager.cpp:232`) | Theo camera: `keepArchivedMinFor`/`keepArchivedMaxFor` (giây) và cờ `*Auto`; xoá khi mount record vượt `storage.limit_percent_usage`, giảm dần từ max về min |
| Retention tier ON (HOT) | `PolicyTierConfig::retain_until_days`, `overflow_action` (`manager/Local/StorageTier.h:293`) | Pool HOT chuyển tầng khi `usage_pct >= pool.high_watermark_percent` (`TierStorageManager.cpp:3793`) |
| Chu kỳ | tier OFF: `StorageManager` timer 300 s → `postMaintenanceCycle(true)` trên `BlockingExecutors::storage()`; tier ON: tiering cycle 300 s gọi `evaluatePoolThroughput` | |
| `systemStatistic` | `server/WebApiMonitor.cpp:18` → `GlobalMonitor::makeSystemStatisticJson()` | Route `/media/mserver/systemStatistic` là sync (chạy trên poller) → phần thêm **không được chặn** |
| Dashboard summary | `storageDashboardSummary` (`server/WebApiStorage.cpp:903`) | `tier_summary[].write_mbps` = tổng `avg_write_mbps`; `estimated_remaining_days` = 0 cứng |
| Dashboard detail | `TierStorageManager::getDashboardDetail` (`:7336`) | `summary.tiers{HOT,WARM,COLD}` chỉ có bytes/pool_count; `pools[]` = `p.toJson()`, chưa có throughput |
| `storage_alerts` | `StorageTierExtra.h` | Chỉ có ở bản tier ON |

## 4. Thiết kế

### 4.1 Đo demand dùng chung

Thêm vào `PoolThroughputTracker` (không đổi hành vi hiện có):

```cpp
struct IngestRate {
    bool valid = false;        // đã quan sát đủ min(window, 900 s)
    float mbps = 0.0f;         // bytes trong cửa sổ / thời gian quan sát
    uint64_t bytes = 0;
};
// Tính tại chỗ (chỉ khoá mutex, không I/O), không phụ thuộc evaluate().
IngestRate cameraIngest(const std::string &camera_id, int window_sec, int64_t now_ms) const;
IngestRate poolIngest(const std::string &pool_id, int window_sec, int64_t now_ms) const;
std::map<std::string, IngestRate> cameraIngestByPool(const std::string &pool_id, int window_sec, int64_t now_ms) const;
```

- Meter theo camera đổi khoá thành `camera_id + pool_id` nội bộ (camera hiếm khi ghi vào 2 pool HOT, nhưng khi
  có thì yêu cầu dung lượng phải chia đúng pool); `setRoutes`/demand hiện tại vẫn cộng theo camera như cũ.
- Bản tier OFF: một instance riêng trong `StorageManager`, gọi `addIngest(camera_id, "", bytes, now)` từ listener mới
  (pool rỗng, chỉ meter camera) + một meter tổng cho thư mục record.
- Cửa sổ dùng lại `storage.pool_throughput_window_seconds` (mặc định 3600). Ghi lại chu kỳ thường của camera
  (`mp4_max_second`) nhỏ hơn cửa sổ nhiều nên không bị răng cưa.

### 4.2 Listener tier OFF

Trong `StorageManager.cpp`, `#if !defined(ENABLE_TIER_STORAGE) && defined(ENABLE_MP4)`: `onceToken` thêm listener
`kBroadcastRecordMP4` → `_ingest.addIngest(info.app, "", info.file_size, steadyNowMs())`. Chỉ cộng khi
`info.file_path` nằm dưới thư mục record (bỏ `archive_name` như `removeExpiredSegment`). Listener chỉ khoá mutex,
không I/O, không ném exception (để không chặn listener mã hoá/motion phía sau, như ghi chú ở `TierStorageManager`).

### 4.3 Dự báo

Định nghĩa chung cho một vùng lưu trữ (thư mục record hoặc một pool HOT):

| Đại lượng | Tier OFF (mount thư mục record) | Tier ON (pool HOT) |
|---|---|---|
| `usable_bytes` | `total × limit_percent_usage%` | `total × pool.high_watermark_percent%` |
| `used_bytes` | used của mount (snapshot HddMonitor, không statvfs) | `p.used_bytes` |
| `ingest_mbps` | tổng meter record | `poolIngest(pool)` |
| `retention_days_estimate` | `usable / (ingest × 86400)` — giữ được bao nhiêu ngày ở tốc độ hiện tại | như trái |
| `estimated_remaining_days` | `max(0, usable − used) / (ingest × 86400)` — tới lúc bắt đầu xoá/chuyển tầng | như trái |
| `required_bytes` | `Σ camera rate × keepArchivedMinFor` (bỏ camera `keepArchivedMinForAuto`) | `Σ camera rate × HOT.retain_until_days × 86400` theo effective policy (bỏ `retain_until_days = 0`) |
| Cảnh báo khi | `required × safety_factor > usable` | như trái |

- `ingest = 0` hoặc chưa `valid` → các trường ngày là `null`, không đánh giá cảnh báo (giữ trạng thái).
- Tier OFF: mount record dùng chung với DB/OS nên `used` gồm cả dữ liệu khác — giống cách `estimateSpaceToReclaim`
  đang tính, giữ nhất quán.
- Ý nghĩa cảnh báo:
  - tier OFF: không giữ nổi thời gian **tối thiểu** đã cấu hình → `enforceStoragePolicy` sẽ xoá xuống dưới min
    (mất dữ liệu sớm hơn cam kết);
  - tier ON, HOT `overflow_action = MOVE_TO_NEXT_TIER`: HOT chạm watermark trước `retain_until_days` → dữ liệu
    chuyển tầng sớm (không mất, chậm hơn khi xem lại);
  - tier ON, HOT `overflow_action` là xoá: mất dữ liệu sớm.

### 4.4 Cảnh báo

- Bộ đếm trạng thái dùng `AlertLevelTracker` (`ResourceMonitor.h`), cần `storage.retention_forecast_sustain_samples`
  lần đánh giá liên tiếp (mặc định 3 × 300 s) mới báo, nhắc lại theo `..._alert_repeat_seconds`, có sự kiện hồi phục.
- Broadcast mới `kBroadcastRetentionForecastAlert` (tham số `const T &` — xem bẫy NOTICE_EMIT theo giá trị), listener
  trong `server/WebHook.cpp` gửi `on_system_alert`:

```jsonc
{"eventCode": "RetentionCapacityLow" | "RetentionCapacityRecovered",
 "eventTime": 1791000000,
 "scope": "record_dir" | "hot_pool",
 "poolId": "",                       // rỗng khi scope = record_dir
 "poolName": "", "path": "/opt/media/www/record", "mountPoint": "/opt/media",
 "currentValue": "4.20",             // retention_days_estimate
 "threshold": "7.00",                // số ngày cần (required / ingest), làm tròn 2 chữ số
 "ingestMbps": "12.50", "usableBytes": 0, "requiredBytes": 0,
 "cameras": 24,                      // số camera tham gia required
 "overflowAction": "MOVE_TO_NEXT_TIER"}   // chỉ tier ON
```

- Tier ON thêm dòng `storage_alerts` loại `TIER_RETENTION_CAPACITY_LOW` (tối đa 1 lần/giờ/pool như
  `TIER_POOL_THROUGHPUT_LOW`); tier OFF chỉ webhook (không có bảng).
- Config mới (`storage.*`, mặc định đăng ký trong code): `retention_forecast_alert_enabled=1`,
  `retention_forecast_safety_factor=1.1`, `retention_forecast_sustain_samples=3`,
  `retention_forecast_alert_repeat_seconds=21600`.

### 4.5 Nơi chạy

- Tier OFF: cuối `enforceStoragePolicy()` (đã có `record_profiles` và đang ở pool `storage()`), gọi
  `evaluateRetentionForecast(record_profiles)`; lưu kết quả vào một snapshot có mutex để API đọc.
- Tier ON: trong tiering cycle, ngay sau `evaluatePoolThroughput(camera_policies)` (đã có effective policy theo
  camera); kết quả lưu theo pool.
- API chỉ đọc snapshot + tính ingest tại chỗ (mutex) → không I/O trên poller.

### 4.6 API

**`systemStatistic`** — thêm ở `server/WebApiMonitor.cpp::systemStatistic()` (không kéo `Local/` vào `GlobalMonitor`):

```jsonc
"record_write": {
  "mode": "record_dir",              // tier OFF
  "path": "/opt/media/www/record", "mount_point": "/opt/media",
  "ingest_mbps": "12.50",            // null khi chưa đủ thời gian quan sát
  "cameras": 24,
  "usable_bytes": 0, "used_bytes": 0,
  "retention_days_estimate": 21.4, "estimated_remaining_days": 3.2,
  "required_bytes": 0, "state": "normal" | "low"
}
// tier ON
"record_write": {
  "mode": "hot_tier",
  "ingest_mbps": "12.50",            // tổng các pool HOT
  "pools": [{"pool_id": "...", "pool_name": "...", "ingest_mbps": "8.10", "state": "normal"}]
}
```

**`dashboard/summary`** — mỗi phần tử `tier_summary[]`:
- `write_mbps` (đã có), thêm `read_mbps` (tổng `avg_read_mbps`), `record_ingest_mbps` (chỉ HOT khác 0);
- `estimated_remaining_days`: HOT = giá trị nhỏ nhất giữa các pool HOT enabled có ingest; WARM/COLD giữ `0`
  (ngoài phạm vi, ghi rõ trong docs);
- thêm `retention_days_estimate` (HOT), `retention_state` (`normal|low`).

**`dashboard/detail`**:
- `pools[]`: mỗi pool thêm `throughput` (đúng object `PoolThroughputStats::toJson()` như `pool/list`, dùng chung
  `fillPoolThroughput`) và với pool HOT thêm `retention_forecast` (các trường bảng 4.3);
- `summary.tiers.{HOT,WARM,COLD}`: thêm `write_mbps`, `read_mbps`, `record_ingest_mbps`; HOT thêm
  `estimated_remaining_days`, `retention_days_estimate`.

Đơn vị/kiểu giữ theo từng response hiện có: `pool/*` và `systemStatistic` dùng chuỗi MiB/s (`sanitize_for_json`),
`dashboard/summary` dùng số (`roundMbps`).

## 5. Các bước

| # | Việc | File chính |
|---|---|---|
| P1 | `PoolThroughputTracker`: meter camera theo (camera, pool), thêm `cameraIngest/poolIngest/cameraIngestByPool`; unit test | `manager/Local/PoolThroughputTracker.{h,cpp}`, `tests/unit/manager/test_pool_throughput.cpp` |
| P2 | Lớp tính dự báo thuần (không I/O): input = usable/used/ingest/danh sách (rate, required_sec), output = các trường 4.3 + `AlertLevelTracker`; unit test biên (ingest 0, chưa valid, Auto, retain 0) | file mới `manager/Local/RetentionForecast.{h,cpp}` (không guard tier) |
| P3 | Tier OFF: listener `kBroadcastRecordMP4`, tracker trong `StorageManager`, đánh giá cuối `enforceStoragePolicy`, snapshot + getter | `manager/Local/StorageManager.{h,cpp}` |
| P4 | Tier ON: đánh giá sau `evaluatePoolThroughput`, snapshot theo pool, `storage_alerts` `TIER_RETENTION_CAPACITY_LOW` | `manager/Local/TierStorageManager.{h,cpp}` |
| P5 | Broadcast + config + webhook `RetentionCapacityLow/Recovered` | `src/Common/config.{h,cpp}`, `server/WebHook.cpp` |
| P6 | API: `record_write` trong systemStatistic, mở rộng `dashboard/summary` và `dashboard/detail` | `server/WebApiMonitor.cpp`, `server/WebApiStorage.cpp`, `TierStorageManager::getDashboardSummary/Detail` |
| P7 | Docs: `api-system-alert-events.md`, `api-storage-tiering.md` (mục 8, bỏ câu "hiện là 0"), `storage-tiering-flow.md`, docs API systemStatistic | `docs/` |
| P8 | Build cả 2 cấu hình (`build/` tier ON, `build-tier-off2` tier OFF `DebugTierOff`), `ctest -R unit.` cả hai; chạy thật mục 6 | |

## 6. Kiểm thử

- Unit: P1, P2 như bảng; `test_storage_policy` cho phần nối `getRecordProfiles` → required.
- Chạy thật tier OFF (harness như lần kiểm DiskIo 2026-10-03, đường dẫn tuyệt đối): 2 luồng ffmpeg + `startRecord`,
  `mp4_max_second` nhỏ, cửa sổ 60 s; đặt `keepArchivedMinFor` lớn để `required > usable` → `RetentionCapacityLow`;
  hạ xuống → `Recovered`. Kiểm `systemStatistic.record_write` (cần JWT/cluster auth).
- Chạy thật tier ON: pool HOT LOCAL_DISK nhỏ (loop mount ở `/mnt`, **không** dưới `/tmp` vì HddMonitor bỏ qua),
  policy HOT `retain_until_days` lớn → cảnh báo + `storage_alerts`; kiểm `dashboard/summary` và `dashboard/detail`.

## 7. Cần chốt trước khi làm

| # | Câu hỏi | Đề xuất |
|---|---|---|
| Q1 | Tier OFF so với `keepArchivedMinFor` (cam kết tối thiểu) hay `keepArchivedMaxFor`? | Min → cảnh báo; Max chỉ hiển thị (`required_max_bytes`) |
| Q2 | Tier ON, HOT với `MOVE_TO_NEXT_TIER` (không mất dữ liệu) có cảnh báo không? | Có, mức WARNING; overflow xoá → CRITICAL |
| Q3 | `eventCode` | `RetentionCapacityLow` / `RetentionCapacityRecovered` |
| Q4 | WARM/COLD có cần dự báo luôn không? | Chưa, để `estimated_remaining_days = 0` và ghi docs |
| Q5 | Cảnh báo "sắp chạm ngưỡng" (`estimated_remaining_days < N`) | Không: chạm ngưỡng là vận hành bình thường (tự xoá/chuyển tầng), chỉ hiển thị |

## 8. Kết quả thực hiện (2026-10-03)

- Build: tier OFF (`build-tier-off2`, `DebugTierOff`) và tier ON (`build/`, `build-test`) đều -j2.
- Unit: thêm `PoolThroughputTracker.IngestRates*` (2) và `RetentionForecast*` (6) trong `unit_storage`.
- Chạy thật tier OFF (camera giả, segment 10 s, cửa sổ 60 s, sustain 1): `keepArchivedMinFor` 5000 giờ →
  `RetentionCapacityLow` CRITICAL (0,48 MiB/s, 22,21 ngày so với 208,33 ngày cần); hạ 1 giờ → `Recovered`;
  `systemStatistic.record_write` (`mode = record_dir`) khớp.
- Chạy thật tier ON (pool HOT mặc định 1 TB, watermark 85 %): HOT 3650 ngày + `MOVE_TO_NEXT_TIER` (cần tầng WARM kế
  tiếp) → WARNING + dòng `storage_alerts`; HOT-only `DELETE_OLDEST` → CRITICAL; 1 ngày → `Recovered`.
  `record_write` (`hot_tier`), `dashboard/summary`, `dashboard/detail` khớp.

Khác plan:

- `storage_alerts`: plan ghi "tối đa 1 lần/giờ/pool"; chạy thật cho thấy lần nâng WARNING → CRITICAL bị chặn, nên
  giới hạn một giờ chỉ áp cho cùng mức, đổi mức thì ghi ngay.
- Nhắc lại webhook mặc định 6 giờ (theo plan) khác 300 s của các nhóm khác; `api-system-alert-events.md` ghi rõ phía
  API không được tự đóng nhóm này sau 15 phút.
- `dashboard/summary` giữ `estimated_remaining_days` là số (`0` khi chưa có), thêm `retention_days_estimate` (`null`
  khi chưa có) thay vì đổi kiểu trường cũ.
