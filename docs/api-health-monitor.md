# API giám sát sức khỏe MediaServer (`/media/monitor/*`)

Hợp đồng cho app desktop monitor (vận hành/NOC). App gọi định kỳ tới **từng** MediaServer; mỗi
node tự đánh giá trạng thái và trả snapshot đã tính sẵn, nên gọi dồn không làm nặng server.

Trạng thái: hợp đồng `schemaVersion: 1` đã chốt (2026-10-16), mọi endpoint và nhóm chỉ số dưới đây
đã triển khai. Ví dụ tích hợp cho app ở mục 6. Ý nghĩa từng cảnh báo và việc người vận hành cần làm
ở [health-monitor-runbook.md](health-monitor-runbook.md).

Tương thích: server chỉ **thêm** field, chỉ số (`metric`), mã lý do camera hoặc `eventCode` mới mà
không đổi `schemaVersion`. App phải bỏ qua field lạ, hiển thị `metric`/`eventCode` lạ ở dạng thô
(không coi là lỗi), và coi mức lạ như `unknown`. Đổi tên, bỏ field hoặc đổi nghĩa một field thì tăng
`schemaVersion`.

## 1. Xác thực

- Mọi API `/media/monitor/*`, đọc lẫn cấu hình, dùng **`api.secret`** (cùng khoá với `/index/api/*`),
  không cần JWT. Không có khoá riêng cho monitor.
- Gửi khoá qua header HTTP **`secret: <api.secret>`**. Server cũng nhận tham số `secret` trong
  query/body, nhưng không nên đưa khoá lên URL (lộ trong log proxy).
- IP của app phải nằm trong `http.allow_ip_range` (rỗng = mọi IP). Cấu hình này áp chung cho mọi
  API dùng `api.secret`, nên dải IP phải gồm cả các hệ thống tích hợp khác đang gọi các API đó.
- Lỗi: thiếu khoá → `code=-300`; sai khoá → `code=-100 "Incorrect secret"`; IP không được phép →
  `code=-100 "Your ip is not allowed to access the service."` (HTTP 401).
- `api.secret` là khoá quản trị: app lưu mã hoá (keychain/DPAPI), không ghi thô xuống đĩa, không in
  ra log; nên gọi qua HTTPS.
- `data.apiSecretIsDefault = true` khi node vẫn dùng giá trị `api.secret` mặc định trong code: app
  nên nhắc người vận hành đổi khoá (sửa file cấu hình, API không đổi được khoá này).

## 2. Mức và luật gộp

| Mức | Nghĩa |
|---|---|
| `ok` | bình thường |
| `warning` | vượt ngưỡng cảnh báo |
| `critical` | vượt ngưỡng nghiêm trọng |
| `unknown` | chưa đủ mẫu (vừa khởi động) hoặc chưa có số liệu; không tính vào tổng |
| `disabled` | (chỉ ở `components`) thành phần bị tắt khi build |

- Mỗi chỉ số chỉ đổi mức sau `monitor.sustainSamples` mẫu liên tiếp (mặc định 3 × 10 s), cả lúc xấu
  đi và lúc hồi phục. Chỉ số đã có hysteresis riêng (nghẽn IO đĩa) hoặc dạng sự kiện (ổ không ghi
  được, GPU fallback) đổi mức ngay.
- Mức thành phần = mức xấu nhất của các chỉ số trong nó.
- `overall` = mức xấu nhất của các thành phần, trừ `ingest` và `delivery` (lỗi nằm ở camera/người xem
  bên ngoài) chỉ đẩy `overall` lên tối đa `warning`.
- `score` = 100 − 10 × (số thành phần `warning`) − 30 × (số thành phần `critical`), không âm. Chỉ để
  vẽ xu hướng, không dùng để báo động.
- HTTP luôn 200 khi xác thực qua; trạng thái đọc ở `data.overall`.

| Thành phần | Gồm |
|---|---|
| `host` | CPU, load, RAM, swap, NIC, file descriptor |
| `process` | độ trễ/tải poller, hàng đợi pool blocking, GPU fallback |
| `storage` | dung lượng, nghẽn IO, khả năng ghi của các ổ chứa bản ghi / DB / pool tier; độ trễ lấy khoá ghi DB; sức khỏe, throughput, backlog của pool tier; job tier lỗi |
| `ingest` | tỉ lệ camera offline, camera online mà không có hình |
| `recording` | camera lẽ ra đang ghi mà không ghi, lỗi ghi file, thư mục bản ghi ghi được |
| `delivery` | người xem so với giới hạn license, tỉ lệ từ chối yêu cầu xem, restore S3 cho xem lại bị lỗi |
| `cluster` | node khác trong cụm liên lạc được không, sync ESC, node bị bỏ khỏi cụm, webhook tới VMS/ESC, Kafka |

## 3. Endpoint

Tất cả là GET (trừ `config/set`), envelope `{"code":0,"data":{...}}`, mọi `data` có `schemaVersion: 1`.
Thêm field mới không đổi `schemaVersion`; app phải bỏ qua field lạ.

| Endpoint | Chu kỳ gợi ý | Nội dung |
|---|---|---|
| `/media/monitor/health` | 5–10 s | trạng thái tổng, thành phần, lý do |
| `/media/monitor/summary` | 15–30 s | như `health` + toàn bộ số liệu theo nhóm |
| `/media/monitor/alerts?since=<seq>` | 10 s | cảnh báo đang mở + sự kiện mới sau `seq` |
| `/media/monitor/cameras?health=&page=&size=` | 30–60 s | camera kèm chỉ số chất lượng từng luồng |
| `/media/monitor/history?from=&to=&bucket_sec=&fields=` | khi mở biểu đồ | chuỗi thời gian sức khỏe + tài nguyên (mục 3.8) |
| `/media/monitor/config/get` | khi mở tab cấu hình | ngưỡng và tham số hiện hành |
| `/media/monitor/config/set` | khi bấm lưu | POST JSON, đổi ngưỡng/tham số |

Snapshot được tính mỗi `monitor.intervalSec` (mặc định 10 s); gọi nhanh hơn chỉ nhận lại cùng
snapshot.

### 3.1 `/media/monitor/health`

Tham số tuỳ chọn `rev`: nếu bằng `rev` hiện tại (mức và lý do không đổi) server trả
`{"code":0,"notModified":true}` không kèm `data`.

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1,
    "enabled": true,
    "mediaServerId": "ms-01",
    "generatedAt": 1760500000,
    "rev": 12,
    "overall": "warning",
    "score": 90,
    "uptimeSec": 86400,
    "version": {"buildTime": "2026-10-16T08:50:45", "branchName": "develop", "commitHash": "f1284c3"},
    "apiSecretIsDefault": false,
    "alerts": {"active": 1, "critical": 0},
    "components": {
      "host": "warning", "process": "ok", "storage": "ok",
      "ingest": "unknown", "recording": "unknown", "delivery": "unknown", "cluster": "unknown"
    },
    "reasons": [
      {"component": "host", "metric": "cpu.usage_pct", "level": "warning",
       "value": 84.2, "threshold": 80, "since": 1760499700}
    ]
  }
}
```

- `reasons[]`: mỗi chỉ số đang `warning`/`critical`, xấu nhất trước. `threshold` là ngưỡng đã vượt,
  `since` là lúc mức hiện tại bắt đầu (unix giây), `detail` (nếu có) chỉ thực thể: tên NIC, mount
  point, đường dẫn thư mục, tên pool.
- `rev` tăng khi `overall`, mức một thành phần, hoặc tập lý do đổi; giá trị số đổi thì không tăng.
  Mỗi lần khởi động `rev` bắt đầu từ thời điểm khởi động (unix mili-giây) nên luôn tăng qua các lần
  khởi động lại: `rev` của lần chạy trước không bao giờ trả nhầm `notModified`. App chỉ so bằng, không
  suy ra số lần đổi từ hiệu hai `rev`.
- Monitor tắt (`monitor.enable=0`): `data = {"schemaVersion":1,"enabled":false,"overall":"unknown",...}`.

### 3.2 `/media/monitor/summary`

`data` gồm các field của `health` (trừ `uptimeSec`, `version` nằm trong `process`/top-level) và:

| Nhóm | Field |
|---|---|
| `host.cpu` | `cores`, `usage_pct` (máy), `proc_usage_pct`, `load1`, `load1_per_core` |
| `host.ram` | `total`, `used`, `usage_pct`, `proc_rss` (byte), `proc_usage_pct` |
| `host.swap` | `total`, `used`, `used_pct` |
| `host.nets[]` | `name`, `rx_mbps`, `tx_mbps`, `speed_mbps`, `util_pct` (null khi NIC ảo không báo tốc độ), `errors_per_sec`, `drops_per_sec` |
| `host.fd` | `used`, `limit`, `used_pct` |
| `host.threads` | số thread của tiến trình |
| `process` | `uptimeSec`, `startedAt` |
| `process.pollers`, `process.work_pollers` | `count`, `delay_max_ms`, `delay_avg_ms`, `load_max` (%), `sampled_age_ms` |
| `process.blocking[]` | theo pool: `name`, `threads`, `queued`, `running`, `completed`, `window_tasks`, `wait_avg_ms`, `wait_max_ms`, `busy_pct` (cửa sổ = 1 chu kỳ) |
| `transcode` | `streams`, `decoders`, `encoders`, `motion`, `mjpeg_ingest` |
| `gpu` | `backends`, `load`, `decoders`, `encoders`, `fallbacks` (từ lúc khởi động), `last_fallback_at` |
| `storage.disks[]` | chỉ các mount chứa bản ghi / DB / pool: `mount`, `device`, `total_bytes`, `used_bytes`, `free_bytes`, `usage_pct`, `read_only`, `retention_limit_pct` (mức retention giữ mount, `null` nếu không có), `warn_pct`/`crit_pct` (ngưỡng đang áp), `io_util_pct`, `await_ms`, `read_mbps`, `write_mbps`, `io_state` |
| `storage.disk_io` | `valid`, `iowait_pct`, `psi_io_full_pct`, `state` (`normal`/`warning`/`critical`) |
| `storage.storage_write[]` | kết quả thử ghi từng thư mục (như `storage_write` của `systemStatistic`) |
| `recording` | `cameras` (đang bật), `expected`, `active`, `starting`, `stalled`, `stalled_pct`, `stale_sec`, `write_errors` (từ lúc khởi động), `write_errors_cycle`, `stalled_cameras[]` (tối đa 100: `cameraId`, `name`, `ip`, `recordMode`, `lastWriteAt`, `expectedSince`) |
| `delivery.viewers` | `live`, `playback`, `total`, `limit` (giới hạn license, null = không giới hạn), `limit_used_pct` |
| `delivery.play` | `window_sec`, `requests`, `failures`, `failure_pct` (trong cửa sổ), `failures_by_reason` (từ lúc khởi động: `Unauthorized`, `MaxRequest`, `ResourceUnavailable`, `OverlayForbidden`, `TranscodeDisabled`, ...) |
| `alerts` | `active`, `critical`: số cảnh báo đang mở |
| `ingest` | `cameras` (đang bật), `online`, `offline`, `offline_pct`, `no_video`, `no_video_pct`, `health` (`ok`/`warning`/`critical`/`unknown`: số camera theo mức) |
| `storage.database` | `checked_at`, `stale`, `write_ms` (thời gian lấy khoá ghi `BEGIN IMMEDIATE` DB MediaServer, null khi lỗi), `error` |
| `storage.tier` | `enabled` (false khi build tắt tier storage); khi bật: `pools[]` (`id`, `name`, `tier`, `type`, `enabled`, `usage_pct`, `health_check`, `health_status` `OK`/`WARNING`/`HIGH`/`CRITICAL`/`OFFLINE`/`UNKNOWN`, `health_stale`, `message`, `throughput_low`, `write_mbps`, `backlog_bytes`, `backlog_eta_sec`), `jobs` (`tiering_failed_24h`, `tiering_pending`, `tiering_running`, `restore_failed_24h`, `restore_failed_1h`, `restore_pending`) |
| `delivery.restore` | `failed_1h`, `failed_24h` (khi bật tier storage) |
| `cluster` | `peers_known`, `peers_unreachable`, `peers[]` (`id`, `name`, `reachable`) |
| `cluster.sync` | `enabled` (false khi build tắt sync hoặc `database.enable_sync_db=0`), `bootstrapped`, `single_node`, `decommissioned`, `peers[]` (`id`, `last_ok_at`, `last_ok_age_sec`) |
| `cluster.webhook` | `window_sec`, `requests`, `failures`, `failure_pct`: các lần gọi tới `hook.api_url` (VMS/ESC), kết quả cuối sau khi đã retry; lời gọi giữa các node không tính (đã có `peers_unreachable`) |
| `host.ram` (thêm) | `proc_rss_growth_mb_per_h` (độ dốc RSS, null khi chưa đủ 2 h số liệu), `proc_rss_growth_span_sec` |
| `process` (thêm) | `restarts_24h` (số lần khởi động có lần chạy trước trong 24 h), `unclean_restarts_24h` (lần chạy trước không thoát sạch: crash, kill) |
| `recording.coverage` | `computed_at`, `cameras_evaluated` (camera có ≥ 1 h lẽ ra ghi), `below_warn`, `below_crit`, `low_pct`, `avg_pct` |
| `delivery.play.open` | thời gian từ yêu cầu xem tới khi có luồng (`MediaSource::findAsync`, gồm chờ kéo luồng theo yêu cầu) trong `monitor.playWindowSec`: `live_samples`, `live_p50_ms`, `live_p95_ms`, `live_max_ms` (luồng live mở được), `replay_samples`, `replay_max_ms` (xem lại, có thể chờ restore S3, chỉ đếm), `not_found` (không có luồng sau `general.maxStreamWaitMS`, thường do client gửi sai id, chỉ đếm) |
| `cluster.kafka` | `enabled`; khi bật: `mode` (`LIVE`/`SPOOL`/`DRAIN`), `brokers_up`, `brokers_known`, `queued`, `delivered`, `spool_pending`, `spool_bytes`, `dropped_rate` (vượt hạn mức/camera, không tính là lỗi), `dropped` (mất do giới hạn spool/hàng đợi), `last_drop_at` |

Số liệu `storage.database`, `storage.tier`, `delivery.restore` đọc SQLite nên chạy trên pool `api` mỗi
`monitor.slowIntervalSec` (60 s), chu kỳ đánh giá dùng kết quả gần nhất; kết quả cũ hơn 3 lần chu kỳ đó
(`stale`) vẫn trả về nhưng không đánh giá.

Số không đo được trả `null`.

Poller không trả lời lệnh đo độ trễ sau 1 s (bị kẹt trong một tác vụ) thì `delay_max_ms` là thời gian
đã chờ, nên một poller treo hẳn vẫn hiện `critical`.

### 3.3 Chỉ số và ngưỡng mặc định

| Chỉ số (`metric`) | Thành phần | Warn / Crit | Khoá cấu hình |
|---|---|---|---|
| `cpu.usage_pct` | host | 80 / 90 | `globalmonitor.cpu_warning_threshold` / `cpu_critical_threshold` (VMS cũng đặt qua cấu hình server) |
| `cpu.load1_per_core` | host | 1.5 / 3 | `monitor.load1PerCoreWarn` / `Crit` |
| `ram.usage_pct` | host | 80 / 90 | `globalmonitor.memory_warning_threshold` / `memory_critical_threshold` |
| `swap.used_pct` | host | 20 / 50 | `monitor.swapUsedPctWarn` / `Crit` |
| `net.util_pct` | host | 70 / 90 | `monitor.netUtilPctWarn` / `Crit` |
| `net.errors_per_sec` | host | 0.01 / 100 | `monitor.netErrorsPerSecWarn` / `Crit` |
| `net.drops_per_sec` | host | 10 / 100 | `monitor.netDropsPerSecWarn` / `Crit` |
| `proc.fd_used_pct` | host | 70 / 90 | `monitor.fdUsedPctWarn` / `Crit` |
| `pollers.delay_max_ms` | process | 50 / 200 | `monitor.pollerDelayMsWarn` / `Crit` |
| `pollers.load_max` | process | 80 / 95 | `monitor.pollerLoadPctWarn` / `Crit` |
| `work_pollers.delay_max_ms` | process | 100 / 500 | `monitor.workPollerDelayMsWarn` / `Crit` |
| `blocking.wait_max_ms` (theo pool) | process | 1000 / 5000 | `monitor.blockingWaitMsWarn` / `Crit` |
| `blocking.busy_pct` (theo pool) | process | 80 / 95 | `monitor.blockingBusyPctWarn` / `Crit` |
| `gpu.fallbacks` | process | warning trong `monitor.gpuFallbackWindowSec` (300 s) sau mỗi lần GPU rơi về CPU | |
| `disk.usage_pct` (theo mount) | storage | 80 / 90; mount retention tự xoá: max(80, L + 3) / max(90, L + 6) | `globalmonitor.hdd_warning_threshold` / `hdd_critical_threshold` (VMS đặt qua `STORAGE_levelLow/Medium`). L = `storage.limitPercentUsage` của mount thư mục record khi tắt tier, `high_watermark_percent` lớn nhất của pool LOCAL_DISK/NAS trên mount khi bật tier. Ở mount đó `DiskWarning`/`DiskCritical` dưới ngưỡng mới không vào `alerts` (webhook gửi backend không đổi) |
| `disk_io.mount`, `disk_io.host` | storage | theo `globalmonitor.disk_io_*`, `iowait_*` | mức đã sustain của DiskIoMonitor |
| `storage_write.writable` (theo thư mục) | storage | không ghi được = critical | |
| `recording.stalled_pct` | recording | > 0 / 5 (% camera lẽ ra ghi) | `monitor.recordStalledPctWarn` / `Crit` |
| `recording.write_errors` | recording | lỗi mới trong chu kỳ = warning, 3 chu kỳ liên tiếp = critical | |
| `recording.storage_writable` | recording | thư mục bản ghi chính không ghi được = critical | |
| `viewers.limit_used_pct` | delivery | 80 / 95 (chỉ khi license có giới hạn) | `monitor.viewerLimitPctWarn` / `Crit` |
| `play.failure_pct` | delivery | 5 / 20 (khi cửa sổ có ≥ `monitor.playMinRequests` yêu cầu) | `monitor.playFailurePctWarn` / `Crit` |
| `cameras.offline_pct` | ingest | 5 / 20 (% camera đang bật) | `monitor.cameraOfflinePctWarn` / `Crit` |
| `cameras.no_video_pct` | ingest | > 0 / 5 | `monitor.cameraNoVideoPctWarn` / `Crit` |
| `replay.restore_failures_1h` | delivery | 1 / 10 job restore lỗi trong 1 h | `monitor.restoreFailed1hWarn` / `Crit` |
| `db.write_ms` | storage | 500 / 2000 ms; lấy khoá ghi lỗi (DB khoá quá `busy_timeout`, file không ghi được) = critical | `monitor.dbWriteMsWarn` / `Crit` |
| `pool.health` (theo pool) | storage | theo health check của pool: `WARNING`/`HIGH` = warning, `CRITICAL`/`OFFLINE` = critical, quá hạn = unknown (đã có ngưỡng lỗi/hồi phục riêng) | `storage.pool_health_*` |
| `pool.throughput_low` (theo pool) | storage | pool nhận dữ liệu tier chậm hơn nhu cầu = warning (đã sustain trong PoolThroughputTracker) | `storage.pool_throughput_*` |
| `pool.backlog_eta_sec` (theo pool) | storage | 6 h / 24 h | `monitor.poolBacklogEtaSecWarn` / `Crit` |
| `jobs.tiering_failed_24h` | storage | 1 / 10 | `monitor.tierJobsFailed24hWarn` / `Crit` |
| `cluster.peers_unreachable` | cluster | có node không trả lời health check (60 s) = warning | |
| `cluster.decommissioned` | cluster | node bị bỏ khỏi cụm = critical | |
| `sync.bootstrapped` | cluster | chưa bootstrap ESC xong = warning | |
| `sync.last_ok_age_sec` (theo peer) | cluster | 120 / 600 s từ lần pull gần nhất lấy hết dữ liệu của peer | `monitor.syncLastOkAgeSecWarn` / `Crit` |
| `webhook.failure_pct` | cluster | > 0 / 20 % trong `monitor.integrationWindowSec`; dưới 10 lần gọi thì lỗi chỉ là warning; 4xx là câu trả lời của backend, không tính lỗi | `monitor.webhookFailurePctWarn` / `Crit` |
| `kafka.spool` | cluster | không broker nào nhận, sự kiện đang ghi xuống spool = warning | |
| `kafka.dropped` | cluster | warning trong `monitor.integrationWindowSec` sau mỗi lần mất sự kiện | |
| `ram.proc_rss_growth_mb_per_h` | host | 200 / 500 MB/h, hồi quy trên `monitor.rssGrowthWindowSec` (6 h), bỏ 30 phút đầu sau khởi động, cần ≥ 2 h | `monitor.rssGrowthMbPerHWarn` / `Crit` |
| `process.unclean_restarts_24h` | process | 1 / 3 (khởi động lại theo lịch thoát sạch nên không tính) | `monitor.restarts24hWarn` / `Crit` |
| `recording.coverage_low_pct` | recording | > 0 / 5 % camera có độ phủ 24 h ≤ `coveragePct24hCrit` | `monitor.recordCoverageLowPctWarn` / `Crit` |
| `play.open_latency_p95_ms` | delivery | 2000 / 5000 ms, p95 thời gian mở luồng live thành công (khi có ≥ `monitor.playMinRequests` mẫu) | `monitor.openLatencyP95MsWarn` / `Crit` |

Ngưỡng theo camera (mục 3.6), cùng dạng `monitor.<tên>Warn` / `Crit`:

| Chỉ số luồng | Warn / Crit | Tên |
|---|---|---|
| `fps_ratio` = fps đo được / `fps_baseline` | ≤ 0.8 / ≤ 0.5 (thấp là xấu) | `fpsRatio` |
| `bitrate_ratio` | ≤ 0.5 / ≤ 0.2 (thấp là xấu) | `bitrateRatio` |
| `loss_pct` | 1 / 5 | `lossPct` |
| `last_frame_age_ms` | 3000 / 10000 | `lastFrameAgeMs` |
| `disconnects_1h` | 3 / 10 | `disconnects1h` |
| `uptime_pct_24h` | ≤ 99 / ≤ 95 (thấp là xấu) | `uptimePct24h` |
| `recording.coverage_pct_24h` (theo camera) | ≤ 99 / ≤ 95 (thấp là xấu), cả hai chỉ là warning của camera | `coveragePct24h` |
| `recording.gaps_24h` (theo camera) | 1 / 6, cả hai chỉ là warning của camera | `recordGaps24h` |

Ngưỡng là `>=`. Drop gói ở NIC ảo/multicast thường khác 0 nên ngưỡng `net.drops_per_sec` rộng hơn lỗi.

### 3.4 Ghi hình: "lẽ ra đang ghi"

- Lấy đúng quyết định của bộ lập lịch ghi của camera (giờ địa phương, lịch 7 × 24 ô `recordingSchedule`):
  `enableRecord = false` hoặc ô giờ `NoRecord` → không kỳ vọng; `RecordAlways`,
  `RecordLowResAndMotion` → luôn kỳ vọng; `RecordOnlyMotion` → chỉ khi sự kiện chuyển động đang mở.
  Camera chỉ ghi theo chuyển động mà cả ngày không có chuyển động là bình thường.
- "Đang ghi" (`active`) = camera có segment đóng xong trên đĩa trong `stale_sec` gần nhất. Mặc định
  `stale_sec = protocol.mp4_max_second × 1.5 + 60` (segment chỉ được tính khi đóng file).
- `starting`: vừa chuyển sang kỳ vọng ghi (đầu giờ có lịch, bắt đầu chuyển động, camera mới bật) chưa
  quá `stale_sec`; không tính là lỗi.
- `stalled` = kỳ vọng ghi, quá thời gian ân hạn, và không có segment nào trong `stale_sec`. Mỗi camera
  chuyển sang `stalled` mở cảnh báo `RecordingStalled` (chỉ có trong `/media/monitor/alerts`, không gửi
  webhook), hồi phục thì `RecordingRecovered`.
- Chỉ tính camera đang bật trên node này. Số liệu nằm trong RAM, mất khi khởi động lại.

Độ phủ 24 h (tính mỗi `monitor.coverageIntervalSec` = 900 s trên pool `api`, từ timefile nên không mất khi
khởi động lại):

- Thời gian lẽ ra ghi = các giờ trong 24 h qua có mode `RecordAlways`/`RecordLowResAndMotion` theo lịch
  hiện tại (giờ chỉ ghi theo chuyển động không tính), chỉ trong khoảng camera thuộc node này theo
  `VmsResourceAssignment` (failover sang node khác không tính là thiếu), bỏ `stale_sec` cuối (segment
  đang ghi chưa vào timeline). Camera chưa có bản ghi nào trước cửa sổ 24 h (camera mới, vừa bật ghi)
  chỉ tính từ bản ghi đầu tiên; không có bản ghi nào trong 48 h thì không tính độ phủ (đã có `stalled`).
- `coverage_pct_24h` = thời gian có bản ghi (mọi luồng, kể cả bản ghi gap-fill) trong thời gian lẽ ra ghi.
  Lỗ ≤ 5 s giữa hai file liền nhau coi như liền.
- `gaps_24h` = số khoảng thiếu dài hơn `2 × protocol.mp4_max_second`; `longest_gap_sec`.
- Camera có dưới 1 h lẽ ra ghi không được đánh giá. Lịch đã đổi trong 24 h thì tính theo lịch hiện tại.

### 3.5 Người xem

- `play.*` đếm kết quả phân quyền mỗi yêu cầu xem (RTSP/RTMP/HTTP-FLV/HLS/WebRTC). HLS gửi một lần
  phân quyền cho mỗi lần tải playlist nên số yêu cầu HLS cao hơn số người xem. Yêu cầu tới stream không
  tồn tại không được tính (stream xem lại được tạo theo yêu cầu nên "chưa có" không phải lỗi).

### 3.6 `/media/monitor/cameras`

Tham số: `health` = danh sách mức cách nhau dấu phẩy (mặc định `warning,critical`; `all` = mọi
camera), `page` (từ 1), `size` (mặc định 50, tối đa 500). Camera xấu nhất trước, cùng mức theo tên.

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1, "mediaServerId": "ms-01", "generatedAt": 1760500000, "rev": 12,
    "total": 3, "page": 1, "size": 50,
    "cameras": [{
      "cameraId": "cam-01", "name": "Cổng chính", "ip": "10.0.0.5", "manufacturer": "Hikvision",
      "health": "warning", "healthSince": 1760499000, "observed": "warning",
      "reasons": ["main.fps_ratio", "main.bitrate_ratio"],
      "online": true,
      "recording": {"state": "active", "expected": true, "mode": "RecordAlways", "lastWriteAt": 1760499990,
                    "coverage_pct_24h": 99.8, "expected_sec_24h": 86100, "gaps_24h": 0, "longest_gap_sec": 0,
                    "coverage_at": 1760499100},
      "controller": {"connected": true, "status": "success"},
      "streams": {
        "main": {"streamId": "cam-01-main", "live": true, "status": "success", "lastChangeAt": 1760400000,
                 "codec": "H264", "width": 1920, "height": 1080,
                 "fps": 10.4, "fps_baseline": 25.0, "fps_ratio": 0.42,
                 "bitrate_kbps": 2630, "bitrate_baseline": 6700, "bitrate_ratio": 0.39,
                 "baseline_at": 1760401000, "sampled_at": 1760499960,
                 "loss_pct": 0.0, "last_frame_age_ms": 40,
                 "disconnects_1h": 0, "disconnects_24h": 1, "uptime_pct_24h": 99.9, "window_started_at": 1760413600},
        "sub": null
      }
    }]
  }
}
```

- `recording.coverage_pct_24h`, `gaps_24h`: null khi chưa tính hoặc không có thời gian lẽ ra ghi (mục 3.4).
- `health`: mức của camera sau hysteresis (`monitor.sustainSamples` chu kỳ), `observed`: mức của chu kỳ
  vừa đo (đổi ngay), `healthSince`: lúc `health` bắt đầu.
- Luật: **critical** khi camera offline (không luồng nào live), lẽ ra đang ghi mà
  không ghi (`recording.stalled`), hoặc luồng live mà không có frame video ≥ `lastFrameAgeMs` crit
  (`<luồng>.no_video`); **warning** khi một trong hai luồng mất, `last_frame_age_ms` ≥ warn, fps/bitrate
  thấp hơn baseline, mất gói, mất kết nối nhiều, uptime thấp, hoặc controller (ONVIF/ISAPI/PSIA) không
  kết nối (camera không phải `GENERIC-RTSP`). Mã trong `reasons[]`: `offline`, `recording.stalled`,
  `controller.disconnected`, `main.offline`, `main.no_video`, `main.last_frame_age_ms`, `main.fps_ratio`,
  `main.bitrate_ratio`, `main.loss_pct`, `main.disconnects_1h`, `main.uptime_pct_24h` (tương tự `sub.`).
- Luồng bị tắt trong cấu hình camera hoặc không có URL: `null`.
- `fps` là fps **đo được** (số frame video nhận / thời gian giữa hai mẫu), không phải fps khai báo trong
  SPS. `bitrate_kbps` lấy từ track, track không báo thì từ tốc độ byte của luồng. Cả hai lấy mẫu cùng
  thống kê luồng, mỗi 60 s, nên đổi fps/bitrate hiện ra sau tối đa ~1 phút; sau khi mất kết nối là
  `null` cho tới mẫu đầu của kết nối mới.
- **Baseline** (`fps_baseline`, `bitrate_baseline`, `baseline_at`): trung bình các mẫu trong
  `monitor.baselineWindowSec` (300 s) sau khi luồng live liên tục `monitor.baselineWarmupSec` (120 s);
  cửa sổ có độ lệch chuẩn fps > 15 % trung bình bị bỏ, đo lại. Giữ qua mất kết nối; học lại khi codec
  hoặc độ phân giải đổi, khi cấu hình luồng camera được đổi qua API (media profile, video config rule),
  hoặc khi fps lệch > 20 % liên tục 1 giờ (người cài đặt chủ động đổi fps). Chưa có baseline → ratio
  `null`, không đánh giá. Baseline lưu trong DB (bảng `stream_baselines`), nạp lại sau khi khởi động
  (luồng gửi codec/độ phân giải khác thì bỏ và học lại).
- `loss_pct`: tỉ lệ mất gói RTP từ lúc kết nối (chỉ nguồn RTSP), `null` với nguồn khác.
- `last_frame_age_ms`: thời gian từ frame video cuối vào MediaServer.
- `disconnects_*`, `uptime_pct_24h` tính từ lúc MediaServer bắt đầu theo dõi luồng
  (`window_started_at`), mất khi khởi động lại. `uptime_pct_24h` chỉ được đánh giá sau 1 giờ theo dõi.
- Không có mật khẩu/URL của camera trong response.

### 3.7 `/media/monitor/alerts`

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1, "mediaServerId": "ms-01", "generatedAt": 1760500000,
    "seq": 42,
    "active": [
      {"seq": 40, "eventCode": "RecordingStalled", "family": "RecordingStalled", "entity": "cam-01",
       "level": "critical", "since": 1760499700, "at": 1760499700, "currentValue": 180, "threshold": 150,
       "cameraId": "cam-01", "name": "Cổng chính", "ip": "10.0.0.5", "recordMode": "RecordAlways", "lastWriteAt": 1760499520}
    ],
    "recent": [ {"seq": 41, "eventCode": "DiskWritable", "level": "recovered", "...": "..."} ]
  }
}
```

- `active[]`: cảnh báo đang mở, xấu nhất trước rồi cũ nhất trước. `since` = lúc mở, giữ nguyên khi cảnh
  báo lặp lại hoặc tăng mức.
- `recent[]`: tối đa 500 sự kiện gần nhất có `seq > since` (cả sự kiện hồi phục), cũ trước. App lưu
  `seq` của lần trả lời trước và gửi lại ở `?since=` để chỉ nhận phần mới; mở app lần đầu gửi `since=0`.
  `seq` bắt đầu từ thời điểm khởi động (unix mili-giây) và luôn tăng qua các lần khởi động lại, nên
  `since` cũ vẫn nhận đủ sự kiện của lần chạy mới. `recent` chỉ giữ 500 sự kiện: app gọi thưa hơn
  mà có hơn 500 sự kiện mới thì mất phần cũ nhất, `active` vẫn đủ.
- `eventCode` trùng webhook `system-event` ([api-system-alert-events.md](api-system-alert-events.md)):
  `CpuWarning/Critical/Recovered`, `RamWarning/...`, `DiskWarning/DiskCritical` (theo mount),
  `DiskIoWarning/Critical/Recovered`, `DiskReadOnly/DiskWriteFailed/DiskWritable`,
  `PoolThroughputLow/Recovered`, `RetentionCapacityLow/Recovered`, `EventBus*`,
  `LiveStreamLimitExceeded/Recovered`, `CameraStreamLimitExceeded/Recovered`; thêm `RecordingStalled` /
  `RecordingRecovered` của monitor, và `DiskRecovered` theo mount (webhook không có sự kiện hồi phục dung
  lượng theo mount; monitor tự đóng khi mount về dưới ngưỡng cảnh báo).
- Có cả khi webhook tắt (`hook.enable=0`). Danh sách nằm trong RAM, mất khi khởi động lại.
- Field riêng của từng sự kiện (`mountPoint`, `device`, `path`, `poolId`, `cameraId`, ...) nằm cùng
  cấp với các field chung.

### 3.8 `/media/monitor/history`

Tham số: `from`, `to` (unix giây, mặc định 24 h gần nhất, tối đa 31 ngày), `bucket_sec` (gộp theo
bucket; khoảng rộng tự gộp để mỗi chuỗi ≤ ~2000 điểm, bội của 60 s), `fields` (danh sách field của
điểm cách nhau dấu phẩy, thêm `system` để có chuỗi tài nguyên; rỗng = tất cả). Đọc SQLite trên pool
`api`, nhóm giới hạn `storage.read` (quá tải trả 503).

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1, "mediaServerId": "ms-01", "from": 1760413600, "to": 1760500000, "bucket_sec": 300,
    "points": [{"timestamp": 1760413800, "samples": 5, "overall": "warning", "score": 86.0, "score_min": 80,
                "cameras_total": 300, "cameras_online": 297.4, "rec_expected": 298, "rec_active": 297.8,
                "rec_stalled": 0.2, "viewers_total": 41.2, "poller_delay_max_ms": 3.0, "alerts_active": 2,
                "proc_rss": 1834500096}],
    "system": [{"timestamp": 1760413800, "samples": 30, "cpu_usage_pct": 41.2, "ram_usage_pct": 62.0, "...": "..."}]
  }
}
```

- `points` từ bảng `health_metrics`, một mẫu mỗi `monitor.historyIntervalSec` (60 s); trong bucket:
  `overall` xấu nhất, `score` trung bình và `score_min`, các số đếm trung bình, `poller_delay_max_ms`,
  `alerts_active`, `proc_rss` lớn nhất.
- `system`: như `/media/mserver/systemStatisticHistory` cùng khoảng và bucket.
- Giữ `monitor.historyRetentionDays` ngày (mặc định 30). Chuỗi `system` lấy từ `system_metrics`, chỉ giữ
  `globalmonitor.metrics_retention_days` ngày (mặc định 7): hỏi xa hơn thì `system` rỗng phần cũ, `points` vẫn có.

## 4. Cấu hình

### 4.1 `/media/monitor/config/get`

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1,
    "values": {"monitor.intervalSec": "10", "monitor.pollerDelayMsWarn": "50", "globalmonitor.cpu_warning_threshold": "80", "...": "..."},
    "effective": {"cpu": {"warn": 80, "crit": 90}, "ram": {...}, "disk": {...}, "reader": {"warn": -1, "crit": -1}},
    "restartRequired": ["monitor.enable", "monitor.intervalSec"],
    "apiSecretIsDefault": false
  }
}
```

- `values`: mọi khoá sửa được, giá trị trong file cấu hình.
- `effective`: ngưỡng CPU/RAM/disk/reader đang áp dụng. VMS cũng đặt CPU/RAM/disk khi đẩy cấu hình
  server (lần đẩy sau ghi đè giá trị đặt từ app), reader lấy theo license (`-1` = không giới hạn) và
  không sửa được qua API này.
- `restartRequired`: khoá chỉ có hiệu lực sau khi khởi động lại MediaServer. Các khoá khác có hiệu
  lực ở chu kỳ đánh giá kế tiếp; trạng thái hysteresis đang tính được giữ nguyên.

### 4.2 `/media/monitor/config/set`

`POST`, `Content-Type: application/json`, body phẳng `{"<khoá>": <giá trị>, ...}`; header `secret`
(hoặc field `secret` trong body).

```json
{"monitor.pollerDelayMsWarn": 80, "globalmonitor.cpu_warning_threshold": 75}
```

- Chỉ nhận khoá có trong `values` của `config/get`; khoá khác (kể cả `api.secret`) →
  `code=902001` HTTP 400, không đổi gì.
- Giá trị phải là số ≥ 0; khoá đếm (`monitor.enable` 0/1, `monitor.intervalSec` ≥ 1,
  `monitor.sustainSamples` ≥ 1, `monitor.gpuFallbackWindowSec` ≥ 0, `monitor.recordStaleSec` ≥ 0,
  `monitor.playWindowSec` ≥ 10, `monitor.playMinRequests` ≥ 1, `monitor.baselineWarmupSec` ≥ 0,
  `monitor.baselineWindowSec` ≥ 120 (mẫu thống kê 60 s, cần 3 mẫu trong cửa sổ), `monitor.slowIntervalSec` ≥ 10, `monitor.integrationWindowSec` ≥ 10,
  `monitor.historyIntervalSec` ≥ 0, `monitor.historyRetentionDays` ≥ 1, `monitor.coverageIntervalSec` ≥ 60, `monitor.rssGrowthWindowSec` ≥ 3600,
  `globalmonitor.disk_io_sustain_samples` ≥ 1) phải là số nguyên.
- Mỗi cặp warn/crit phải có warn < crit, riêng chỉ số "thấp là xấu" (`fpsRatio`, `bitrateRatio`,
  `uptimePct24h`) phải có warn > crit; so với giá trị mới hoặc giá trị đang lưu.
- Thành công: `{"code":0,"changed":<số khoá đổi>,"data":<như config/get>}`; ghi file cấu hình.
- Request không phải POST → `code=902001`.

### 4.3 Khoá trong file cấu hình

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `monitor.enable` | 1 | 0 tắt đánh giá (API trả `enabled:false`) |
| `monitor.intervalSec` | 10 | chu kỳ đánh giá (giây) |
| `monitor.sustainSamples` | 3 | số mẫu liên tiếp trước khi đổi mức |
| `monitor.gpuFallbackWindowSec` | 300 | thời gian giữ cảnh báo sau một lần GPU fallback |
| `monitor.recordStaleSec` | 0 | không có segment trong chừng này giây = không ghi; 0 = `mp4_max_second × 1.5 + 60` |
| `monitor.playWindowSec` | 300 | cửa sổ đếm yêu cầu/lỗi xem |
| `monitor.playMinRequests` | 20 | ít yêu cầu hơn thì không đánh giá tỉ lệ lỗi |
| `monitor.baselineWarmupSec` | 120 | luồng live liên tục chừng này giây mới bắt đầu học baseline |
| `monitor.baselineWindowSec` | 300 | số giây mẫu được lấy trung bình thành baseline |
| `monitor.slowIntervalSec` | 60 | chu kỳ kiểm tra cần SQLite (độ trễ ghi DB, pool tier, job) |
| `monitor.integrationWindowSec` | 300 | cửa sổ đếm webhook, thời gian giữ cảnh báo sau khi Kafka mất sự kiện |
| `monitor.historyIntervalSec` | 60 | chu kỳ ghi `health_metrics`; 0 = không ghi lịch sử |
| `monitor.historyRetentionDays` | 30 | số ngày giữ `health_metrics` và `process_starts` |
| `monitor.coverageIntervalSec` | 900 | chu kỳ tính độ phủ bản ghi 24 h theo camera |
| `monitor.rssGrowthWindowSec` | 21600 | cửa sổ tính độ dốc RSS |
| `monitor.<tên>Warn` / `monitor.<tên>Crit` | xem bảng 3.3 | ngưỡng |

Baseline fps/bitrate học được lưu vào bảng `stream_baselines` và nạp lại sau khi khởi động (luồng gửi
codec/độ phân giải khác thì bỏ và học lại). Mỗi lần khởi động ghi một dòng `process_starts`; thoát sạch
ghi `stopped_at`.

## 5. Ảnh hưởng tới API cũ

`/index/api/getBlockingExecutors`: khi monitor chạy, các field `window_*` là của chu kỳ đánh giá gần
nhất (`monitor.intervalSec`), không còn là "từ lần gọi trước", vì `BlockingExecutors::stats()` chỉ
cho một người gọi định kỳ.

## 6. Tích hợp app: ví dụ

### 6.1 Vòng gọi khuyến nghị

| Việc | Gọi | Ghi chú |
|---|---|---|
| Trạng thái từng node | `/health?rev=<rev đang hiển thị>` mỗi 10 s | `notModified` thì giữ màn hình hiện tại |
| Sự kiện, thông báo | `/alerts?since=<seq lần trước>` mỗi 10 s | lưu `seq` cả khi tắt app; mở lần đầu `since=0` |
| Danh sách camera lỗi | `/cameras?health=warning,critical` | chỉ khi `components.ingest` hoặc `recording` khác `ok`, hoặc khi mở trang camera |
| Chi tiết node | `/summary` mỗi 15–30 s | chỉ khi đang mở trang chi tiết node |
| Biểu đồ | `/history?from=&to=&fields=` | khi mở biểu đồ; không gọi định kỳ |

- Timeout 5 s mỗi lời gọi. Gọi lỗi 3 lần liên tiếp thì hiện node là **mất liên lạc**, khác `critical`.
- `code` khác 0 (hoặc HTTP 401 khi IP không được phép) là lỗi xác thực/cấu hình (mục 1): hiện node là **bị từ chối**, không tính là mất liên lạc, không thử lại dồn dập; báo người dùng kiểm `api.secret`
  và `http.allow_ip_range`.
- Mỗi node một vòng gọi độc lập: node chậm không làm trễ node khác.
- `reasons[].since`, `alerts[].since` là giờ của server: hiển thị theo giờ máy app, không so với giờ app để
  tính "đã kéo dài bao lâu" khi đồng hồ hai bên lệch; dùng `generatedAt - since`.

### 6.2 curl

```bash
S='secret: <api.secret>'
curl -s -H "$S" http://10.0.0.11:8080/media/monitor/health
curl -s -H "$S" "http://10.0.0.11:8080/media/monitor/alerts?since=0"
curl -s -H "$S" "http://10.0.0.11:8080/media/monitor/cameras?health=warning,critical&size=100"
curl -s -H "$S" -H 'Content-Type: application/json' \
     -d '{"monitor.pollerDelayMsWarn": 80}' http://10.0.0.11:8080/media/monitor/config/set
```

### 6.3 Client tham chiếu (Python, chỉ thư viện chuẩn)

Chạy thử với một node: `python3 monitor_client.py http://<ip>:<port> <api.secret>`.

```python
"""Ví dụ app giám sát: gọi /media/monitor/* của nhiều node (chỉ thư viện chuẩn)."""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

NODES = {"ms-01": "http://10.0.0.11:8080"}   # tên hiển thị -> địa chỉ HTTP của MediaServer
SECRET = "<api.secret>"                       # đọc từ kho khoá của app, không ghi thô
HEALTH_SEC, ALERTS_SEC = 10, 10
UNREACHABLE_AFTER = 3                         # số lần gọi lỗi liên tiếp trước khi báo mất liên lạc


class ApiError(Exception):
    """Server trả lời nhưng từ chối (sai secret, IP không được phép): sửa cấu hình, không phải mất liên lạc."""


def call(base, path, **query):
    url = base + path + ("?" + urllib.parse.urlencode(query) if query else "")
    req = urllib.request.Request(url, headers={"secret": SECRET})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:   # IP ngoài http.allow_ip_range trả HTTP 401 kèm body JSON
        try:
            body = json.loads(exc.read().decode("utf-8"))
        except ValueError:
            raise ApiError("HTTP %d" % exc.code)
    if body.get("code") != 0:
        raise ApiError("code=%s %s" % (body.get("code"), body.get("msg")))
    return body


class Node(object):
    def __init__(self, name, base):
        self.name, self.base = name, base
        self.rev = None        # rev của snapshot đang hiển thị
        self.health = None     # data của /health gần nhất
        self.seq = 0           # seq của lần gọi /alerts trước
        self.active = []       # cảnh báo đang mở
        self.failures = 0
        self.error = None      # lỗi API đang có (sai secret, IP không được phép)

    def poll_health(self):
        query = {"rev": self.rev} if self.rev is not None else {}
        body = call(self.base, "/media/monitor/health", **query)
        if body.get("notModified"):
            return False
        self.health = body["data"]
        self.rev = self.health.get("rev")
        return True

    def poll_alerts(self):
        data = call(self.base, "/media/monitor/alerts", since=self.seq)["data"]
        self.seq = data["seq"]
        self.active = data["active"]
        return data["recent"]   # sự kiện mới, cũ trước: ghi nhật ký / bật thông báo

    def state(self):
        # mất liên lạc khác critical: node có thể vẫn ghi hình bình thường
        if self.failures >= UNREACHABLE_AFTER:
            return "unreachable"
        if self.error:
            return "rejected"
        return (self.health or {}).get("overall", "unknown")


def show(node, changed, events):
    h = node.health or {}
    if changed:
        print("%s %s score=%s components=%s" % (node.name, node.state(), h.get("score"), h.get("components")))
        for r in h.get("reasons", []):
            print("    %s %s/%s%s = %s (ngưỡng %s, từ %s)" % (
                r["level"], r["component"], r["metric"], " [%s]" % r["detail"] if r.get("detail") else "",
                r.get("value"), r.get("threshold"), time.strftime("%H:%M:%S", time.localtime(r["since"]))))
        if h.get("apiSecretIsDefault"):
            print("    node vẫn dùng api.secret mặc định: đổi khoá trong file cấu hình")
    for e in events:
        print("%s [%s] %s %s %s" % (node.name, time.strftime("%H:%M:%S", time.localtime(e["at"])),
                                    e["level"], e["eventCode"], e.get("entity", "")))


def main():
    nodes = [Node(n, b) for n, b in NODES.items()]
    while True:
        for node in nodes:
            try:
                changed = node.poll_health()
                events = node.poll_alerts()
                node.failures, node.error = 0, None
            except ApiError as exc:
                if node.error != str(exc):
                    print("%s từ chối: %s (kiểm api.secret, http.allow_ip_range)" % (node.name, exc))
                node.failures, node.error = 0, str(exc)
                continue
            except (OSError, ValueError) as exc:
                node.failures += 1
                if node.failures == UNREACHABLE_AFTER:
                    print("%s mất liên lạc: %s" % (node.name, exc))
                continue
            show(node, changed, events)
            # Chi tiết chỉ gọi khi cần, không gọi mỗi chu kỳ:
            #   /media/monitor/cameras?health=warning,critical  khi ingest/recording khác ok
            #   /media/monitor/summary                          khi mở trang chi tiết node
            #   /media/monitor/history?from=&to=                khi mở biểu đồ
        time.sleep(min(HEALTH_SEC, ALERTS_SEC))


if __name__ == "__main__":
    if len(sys.argv) == 3:   # chạy thử: monitor_client.py <http://host:port> <secret>
        NODES, SECRET = {"node": sys.argv[1]}, sys.argv[2]
    main()
```
