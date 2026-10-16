# Giám sát sức khỏe và chất lượng MediaServer cho app desktop monitor

Trạng thái: **đang làm** — H1, H3, H2, H4, H5 xong (2026-10-16), hợp đồng ở [api-health-monitor.md](api-health-monitor.md). Câu hỏi mở đã chốt 2026-10-15, xem mục 9.

Mục tiêu: mở rộng `GlobalMonitor` để MediaServer tự đánh giá **sức khỏe** (máy và tiến trình có
chạy ổn không) và **chất lượng dịch vụ VMS** (camera có hình, có ghi, người xem có xem được), rồi
cung cấp một nhóm API chỉ đọc cho app desktop monitor gọi định kỳ, xác thực bằng `api.secret` có sẵn (không thêm khoá mới).

---

## 1. Phân tích yêu cầu

### 1.1 Người dùng và cách dùng

| Đối tượng | Nhu cầu |
|---|---|
| App desktop monitor (vận hành/NOC) | Gọi mỗi 5–30 s tới **từng** MediaServer, hiện đèn xanh/vàng/đỏ, danh sách camera lỗi, cảnh báo đang mở, biểu đồ lịch sử |
| Kỹ sư hỗ trợ | Biết vì sao node vàng/đỏ: chỉ số nào, ngưỡng nào, từ khi nào |
| Quản lý dự án | Tỉ lệ camera online, tỉ lệ ghi hình đủ, uptime theo ngày/tuần (SLA) |

### 1.2 Yêu cầu chức năng

- F1. Một API nhẹ trả **trạng thái tổng** (`ok | warning | critical | unknown`) + trạng thái từng thành phần + lý do.
- F2. API tổng hợp đầy đủ chỉ số theo nhóm (tài nguyên máy, tiến trình, camera, ghi hình, người xem, lưu trữ tier, cụm, transcode/GPU).
- F3. Danh sách camera có vấn đề kèm chỉ số chất lượng từng luồng; lọc được, phân trang được.
- F4. Danh sách cảnh báo **đang mở** và sự kiện gần đây (cùng mã `eventCode` với webhook `system-event`).
- F5. Lịch sử chỉ số sức khỏe để vẽ biểu đồ, gộp theo bucket.
- F6. Xác thực mọi API monitor bằng `api.secret` có sẵn (`CHECK_SECRET`), không cần JWT người dùng, không thêm secret key mới.
- F7. Tab cấu hình của app desktop đọc/sửa ngưỡng và tham số monitor, dùng cùng `api.secret`.

### 1.3 Yêu cầu phi chức năng

- N1. **Rẻ khi bị gọi dồn**: nhiều app gọi cùng lúc không được làm nặng server. API trả snapshot đã tính sẵn, không đo lại, không chạm SQLite trên poller (trừ API lịch sử, chạy trên `BlockingExecutors::api()`).
- N2. **Không đụng thread affinity**: số liệu của session/camera chỉ đọc qua getter đã có khoá hoặc bộ đếm atomic; không gọi vào object của poller khác.
- N3. **Hồi phục có trễ (hysteresis)**: trạng thái không nhảy xanh–đỏ theo từng mẫu; dùng lại cơ chế “vượt N mẫu liên tiếp” đã có (`kSystemAlertSustainSamples`, `kDiskIoSustainSamples`).
- N4. **Hợp đồng ổn định**: response có `schemaVersion`; thêm field không phá app cũ.
- N5. Build được khi tắt `ENABLE_TIER_STORAGE`, `ENABLE_SYNC_CHANGES`, `ENABLE_FFMPEG`, `ENABLE_KAFKA` (nhóm tương ứng trả `"enabled": false`).
- N6. C++11, không chụp `this` thô, cấu hình qua `onceToken` + `GET_CONFIG`.

### 1.4 Ngoài phạm vi (đợt này)

- App desktop (chỉ cung cấp API + tài liệu hợp đồng).
- Đẩy (push) số liệu lên server trung tâm, Prometheus exporter: để giai đoạn sau, snapshot thiết kế sao cho xuất được.
- Gộp nhiều node trong một API: app tự gọi từng node (mỗi node có `mediaServerId`).
- Đo chất lượng hình ảnh (nhiễu, mờ, che camera): cần decode, không làm ở đây.

---

## 2. Hiện trạng trong code

| Có sẵn | Ở đâu | Ghi chú |
|---|---|---|
| CPU, RAM (máy + tiến trình), NIC, dung lượng mount | `manager/Server/{Cpu,Memory,Network,Hdd}Monitor` | mẫu 10 s |
| Nghẽn IO (util/await/iowait/PSI), ổ ghi được | `DiskIoMonitor`, `StorageWriteMonitor` | chỉ mount chứa record/pool/DB |
| Số người xem tổng/theo camera, ngưỡng | `ReaderMonitor` | |
| Lịch sử CPU/RAM/reader/net/disk | bảng `system_metrics` (`migration/mserver_updates/10_system_metrics.sql`) | giữ `kMetricsRetentionDays` = 30 ngày |
| Cảnh báo CPU/RAM/disk/IO/ghi/pool/retention, có `*Recovered` | webhook `system-event`, [api-system-alert-events.md](api-system-alert-events.md) | **chỉ đẩy đi, không giữ danh sách đang mở để hỏi lại** |
| Tốc độ ghi + dự báo số ngày giữ | `StorageManager::makeRecordWriteJson`, `TierStorageManager::makeHotWriteJson` | có `camera_mbps` theo camera trong cửa sổ |
| Trạng thái từng luồng camera: live, fps, bitrate, codec, độ phân giải, lỗi | `makeDeviceStatisticJson` (`server/Manager.cpp:2191`) | fps/bitrate là giá trị tức thời, không có kỳ vọng để so |
| Tỉ lệ mất gói RTP | `MediaSource::getLossRate`, `PlayerProxy::getLossRate` | chưa đưa vào thống kê camera |
| Độ trễ / tải poller | `getThreadsLoad` (`server/WebApi.cpp:803`) | phải gọi async |
| Hàng đợi pool blocking | `BlockingExecutors::stats()` | **cửa sổ bị reset mỗi lần gọi, chỉ một người gọi** |
| Transcode/GPU | `src/Codec/TranscodeStats.h` | đếm decoder/encoder/fallback/tải GPU |
| API xem từ ngoài | `/media/api/systemStatistic` (cluster author), `/media/api/device/statisticsList` (`CHECK_SECRET`) | |

### Khoảng trống cần lấp

1. Không có **trạng thái tổng** và luật gộp; app phải tự diễn giải hàng chục số.
2. Không có **chất lượng camera theo thời gian**: số lần mất kết nối, thời gian mất hình, % uptime, fps/bitrate so với cấu hình, loss rate.
3. Không biết **camera bật ghi nhưng thực tế không ghi** (lỗi nguy hiểm nhất của VMS, xem mục 3).
4. Không có **độ trễ event loop / hàng đợi blocking** trong snapshot — chỉ số sớm nhất báo server sắp nghẽn.
5. Không có **danh sách cảnh báo đang mở** để app mới mở lên vẫn thấy.
6. Không có uptime tiến trình, phiên bản, trạng thái DB/sync/tier/Kafka trong một chỗ.

### Vấn đề xác thực hiện tại (ảnh hưởng thiết kế)

- `CHECK_SECRET` (`server/WebApi.h:211`) dùng `api.secret`: kiểm IP theo `http.allow_ip_range` (`HttpFileManager::isIPAllowed`) rồi so tham số `secret`. Khoá có giá trị mặc định cố định trong code (`server/WebApi.cpp:115`) và quyền rộng (`/index/api/*` thêm/xoá stream, …), nên app monitor giữ khoá này tức là giữ khoá quản trị.
- `CHECK_API_KEY` (`server/WebApi.h:354`): khi `manager.apiSecret` rỗng (mặc định) thì **mọi** `X-Secret-Key` khác rỗng đều qua; khi `auth.enableAuthorize=0` thì bỏ qua hẳn. Không dùng macro này cho monitor.
- So sánh chuỗi bằng `!=` (không constant-time).

→ Monitor dùng **`api.secret` qua `CHECK_SECRET()` có sẵn**, không thêm khoá/macro mới; rủi ro khoá quản trị nằm trong app được giảm bằng các biện pháp ở mục 6.1.

---

## 3. Phương pháp đánh giá (tham khảo)

- **USE** (Brendan Gregg) cho tài nguyên: Utilization – Saturation – Errors với từng CPU, RAM, NIC, ổ, pool thread.
- **RED / Four Golden Signals** (Google SRE) cho dịch vụ: Rate – Errors – Duration (+ Saturation) với luồng vào (camera), luồng ra (người xem), API.
- **VMS thương mại**: Milestone XProtect System Monitor đặt ngưỡng cho server (CPU, RAM khả dụng, tải GPU decode) và theo camera (Live FPS, Recording FPS, dung lượng), tính trung bình theo khoảng (vd 1 phút) để lọc gai; Genetec “System Status” liệt kê camera online và **đang ghi hay không**. Kinh nghiệm thực tế (IPVM, Boring Toolbox): camera “online” theo ping nhưng không có hình, hoặc báo “đang ghi” mà 20 ngày không có bản ghi → phải đo **dữ liệu thật tới**, không chỉ trạng thái kết nối.
- **Media**: RFC 3550 (loss/jitter từ RTCP), RFC 3611/8451 (XR: burst loss, frame impairment), RFC 4445 MDI (độ lệch tốc độ luồng so với danh định + tỉ lệ mất). Áp vào đây: loss rate, fps/bitrate thực so với cấu hình, khoảng không có frame.
- **Định dạng health**: draft IETF *Health Check Response Format* dùng `pass | warn | fail` + `checks` theo thành phần; ta dùng cùng ý tưởng với tên `ok | warning | critical` cho khớp mức cảnh báo đã có, cộng `unknown` khi chưa đủ mẫu.

---

## 4. Bộ chỉ số

Cột **Nguồn**: `có` = đã đo, chỉ cần đưa vào snapshot; `mới` = phải đo thêm. Ngưỡng là **mặc định**, cấu hình được.
Cột **Đợt** khớp với mục 8.

### 4.1 Tài nguyên máy (USE)

| Chỉ số | Đơn vị | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|---|
| `cpu.usage_pct` (máy), `cpu.proc_usage_pct` | % | 80 / 90 (ngưỡng hiện có) | có | H1 |
| `cpu.load1_per_core` | – | 1.5 / 3 | mới (`/proc/loadavg`) | H1 |
| `ram.usage_pct`, `ram.proc_rss` | %, byte | 85 / 95 | có | H1 |
| `ram.proc_rss_growth_mb_per_h` (dò rò rỉ) | MB/h | 200 / 500, cửa sổ 6 h | mới (từ lịch sử) | H5 |
| `swap.used_pct` | % | 20 / 50 | mới (`/proc/meminfo`) | H1 |
| `net[].rx/tx_mbps`, `net[].util_pct` = max(rx,tx)/speed | %, Mb/s | 70 / 90 | có (speed có trong `NetInterfaceInfo`) | H1 |
| `net[].errors`, `net[].drops` / s | /s | > 0 kéo dài / > 100 | mới (`/sys/class/net/*/statistics`) | H1 |
| `disk[].usage_pct` | % | 80 / 90 | có | H1 |
| `disk_io` util / await / PSI | %, ms | 80/95, 50/200, 10/30 | có | H1 |
| `storage_write[]` ghi được | bool | – / fail | có | H1 |
| `proc.fd_used_pct` = fd mở / `RLIMIT_NOFILE` | % | 70 / 90 | mới | H1 |
| `proc.threads` | số | thông tin | mới | H1 |

### 4.2 Tiến trình MediaServer (Saturation sớm)

| Chỉ số | Đơn vị | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|---|
| `process.uptime_sec`, `version`, `build_time`, `mediaServerId` | – | – | mới (một phần có) | H1 |
| `process.restarts_24h` (đếm từ log khởi động lưu DB) | lần | 1 / 3 | mới | H5 |
| `pollers.delay_max_ms`, `delay_avg_ms` (EventPollerPool) | ms | 50 / 200 | có (`getExecutorDelay`, async) | H1 |
| `pollers.load_max` | % | 80 / 95 | có | H1 |
| `work_pollers.delay_max_ms` (WorkThreadPool) | ms | 100 / 500 | có | H1 |
| `blocking[].queued`, `wait_max_ms`, `busy_pct` theo pool (`api/device/discovery/ffmpeg/gapFill`) | –, ms, % | wait 1000/5000, busy 80/95 | có, **phải gom về 1 người gọi** | H1 |
| `db.write_latency_ms` (đo bằng 1 câu ghi nhỏ mỗi chu kỳ trên `api()`), `db.busy_errors` | ms, lần | 200/1000, >0 | mới | H4 |

### 4.3 Camera / luồng vào (VMS – Rate & Errors)

Tính trên camera **đang được quản lý và được bật** (không tính camera disable).

| Chỉ số | Đơn vị | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|---|
| `cameras.total / online / offline` | số | – | có (gộp từ `makeDeviceStatisticJson`) | H2 |
| `cameras.offline_pct` | % | 5 / 20 | mới (tính) | H2 |
| `cameras.no_video` (online nhưng không có frame video > `noFrameSec`) | số | > 0 / > 5 % | mới | H2 |
| Theo luồng: `live`, `fps`, `bitrate_kbps`, `codec`, `w×h` | – | – | có | H2 |
| `fps_ratio` = fps thực (trung bình 1 phút) / `fps_baseline` | – | < 0.8 / < 0.5 | mới | H2 |
| `bitrate_ratio` = bitrate thực (trung bình 1 phút) / `bitrate_baseline` | – | < 0.5 / < 0.2 | mới | H2 |
| `loss_pct` (RTP, chỉ khi kéo RTSP/UDP hoặc RTP) | % | 1 / 5 | có ở `getLossRate`, đưa vào thống kê | H2 |
| `last_frame_age_ms` (khoảng từ frame cuối) | ms | 3000 / 10000 | mới (atomic timestamp trong sink) | H2 |
| `disconnects_1h`, `disconnects_24h` | lần | 3 / 10 trong 1 h | mới (bộ đếm khi `live` đổi) | H2 |
| `uptime_pct_24h` (thời gian `live` / thời gian theo dõi) | % | < 99 / < 95 | mới (vòng thời gian chuyển trạng thái trong RAM) | H2 |
| `last_error`, `last_change_ts` | – | – | có (`status`, `last_change_status`) | H2 |
| `controller.connect` (ONVIF/ISAPI/PSIA) | bool | lỗi = warn | có | H2 |

**Mức kỳ vọng fps/bitrate (đã chốt): lấy trung bình lúc ổn định, áp dụng cho mọi camera** (kể cả camera ONVIF có
`stream_settings`, để một cách tính cho tất cả):

- Luồng được coi **ổn định** sau khi `live` liên tục ≥ `monitor.baselineWarmupSec` (mặc định 120 s, bỏ qua GOP đầu và
  lúc camera vừa khởi động encoder).
- `fps_baseline` = trung bình fps của các mẫu 10 s trong `monitor.baselineWindowSec` (mặc định 300 s) sau giai đoạn
  khởi động. Chỉ nhận cửa sổ có độ lệch chuẩn ≤ 15 % trung bình; cửa sổ dao động mạnh thì đo lại cửa sổ kế tiếp.
- Baseline **giữ qua lần mất kết nối**; chỉ đo lại khi codec, độ phân giải hoặc cấu hình luồng của camera đổi
  (VMS cập nhật cấu hình, video config rule), hoặc khi camera giữ fps mới (lệch > 20 %) liên tục ≥ 1 h mà không
  báo lỗi nào khác — trường hợp người cài đặt chủ động đổi fps trên camera.
- Chưa có baseline ⇒ `fps_ratio`/`bitrate_ratio` = `null`, không tính vào mức của camera.
- Baseline giữ trong RAM; H5 lưu thêm vào DB để không phải học lại sau mỗi lần restart.
- Response trả cả `fps_baseline`, `bitrate_baseline`, `baseline_at` để kỹ sư thấy so với cái gì.
- Bitrate của camera VBR dao động tự nhiên theo cảnh (đêm/ngày, ít/nhiều chuyển động), nên ngưỡng bitrate lỏng hơn fps
  và chỉ dùng để báo `warning`.

### 4.4 Ghi hình (chỉ số quan trọng nhất của VMS)

| Chỉ số | Đơn vị | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|---|
| `recording.expected` = camera lẽ ra đang ghi theo lịch (cách tính ở dưới) | số | – | có (option + `RecordScheduler`) | H3 |
| `recording.active` = camera có dữ liệu ghi vào ổ trong `staleSec` gần nhất | số | – | có một phần (`camera_mbps` của ingest tracker) | H3 |
| `recording.stalled` = expected mà không active | số | > 0 / > 5 % expected | mới | H3 |
| `recording.coverage_pct_24h` theo camera = thời lượng có bản ghi / thời lượng lẽ ra ghi | % | < 99 / < 95 | mới (từ timeline / ingest) | H5 |
| `recording.gaps_24h` (số khoảng hở > 2× độ dài file) | số | > 0 / > 5 | mới | H5 |
| `recording.write_errors` (mở/ghi file lỗi, encryption lỗi) / chu kỳ | lần | > 0 / liên tục 3 chu kỳ | mới (bộ đếm atomic) | H3 |
| `recording.ingest_mbps`, `retention.forecast_days` vs `policy_days` | Mb/s, ngày | forecast < policy (đã có `RetentionCapacityLow`) | có | H3 |

`staleSec` mặc định = độ dài file ghi × 1.5 + 60 s, vì ingest tracker cập nhật khi đóng segment.

**“Lẽ ra đang ghi” theo lịch từng camera (đã chốt).** VMS gửi lịch theo giờ cho từng camera qua
`cameraAdvanceConfig.recordingSchedule` → `CameraOption::recordSchedules` (168 ô: 7 ngày × 24 giờ, trả lại ở
`deviceStatistic` → `options.recordSchedules`). `RecordScheduler` (`manager/Camera/RecordPolicy.h`) đã phân tích
chuỗi này và bật/tắt ghi theo giờ. Mỗi ô có `RecordMode`:

| `RecordMode` của ô giờ hiện tại | Lẽ ra đang ghi khi |
|---|---|
| `NoRecord` | không bao giờ |
| `RecordAlways` | luôn luôn |
| `RecordLowResAndMotion` | luôn luôn (ít nhất luồng phân giải thấp) |
| `RecordOnlyMotion` | chỉ khi có sự kiện chuyển động (`isEventActive()`), cộng thời gian ghi sau sự kiện |

- `enableRecord = false` ⇒ không bao giờ kỳ vọng ghi.
- Lấy giờ theo **đúng giờ địa phương** mà `RecordScheduler` dùng, không tự tính lại, để monitor không lệch với hành vi
  ghi thật. Cách làm: `RecordScheduler` công bố một `std::atomic<uint8_t>` trạng thái “expected” (none/always/motion)
  mỗi khi `check()` đổi ô giờ hoặc sự kiện motion bật/tắt; HealthMonitor chỉ đọc atomic này
  (`_active_item`, `_event_active` thuộc poller của camera, không đọc trực tiếp).
- Có **ân hạn** sau mỗi lần chuyển trạng thái expected (đầu giờ có lịch, motion vừa bắt đầu): `staleSec` để file
  đầu tiên kịp đóng, tránh báo `stalled` giả.
- Với `RecordOnlyMotion`, `stalled` chỉ tính trong lúc sự kiện đang mở; camera không có chuyển động cả ngày là bình
  thường, không phải lỗi.
- `coverage_pct_24h` (H5) tính trên **thời gian lẽ ra ghi** theo lịch, không phải 24 h.

### 4.5 Người xem / luồng ra (RED)

| Chỉ số | Đơn vị | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|---|
| `viewers.live / playback / total`, theo giao thức (RTSP/RTMP/HLS/FLV/FMP4/WebRTC) | số | ngưỡng reader hiện có | có một phần (tổng có, theo giao thức lấy từ `getStatistic`) | H3 |
| `viewers.limit_used_pct` | % | 80 / 95 | có | H3 |
| `egress_mbps` / tốc độ NIC | % | 70 / 90 | có (NIC) | H3 |
| `play.requests_5m`, `play.failures_5m`, `play.failure_pct` (auth bị từ chối, stream không tồn tại, hết hạn mức) | %, lần | 5 / 20 % | mới (đếm trong listener `kBroadcastMediaPlayed` / `on_play`) | H3 |
| `play.open_latency_p95_ms` (từ yêu cầu tới frame đầu, có thể chỉ đo cho HTTP-FLV/HLS) | ms | 2000 / 5000 | mới | H5 |
| `replay.restore_failures_1h` (restore S3 không kịp) | lần | > 0 / > 10 | mới | H4 |

### 4.6 Lưu trữ tier / S3 (khi `ENABLE_TIER_STORAGE`)

| Chỉ số | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|
| `pools[].throughput_low`, `backlog_bytes`, `backlog_eta_sec` | low = warn, eta > 24 h = crit | có (`PoolThroughputTracker`) | H4 |
| `pools[].reachable` (lần test gần nhất), `s3_error_rate_1h` | lỗi = crit | có một phần | H4 |
| `jobs.tiering_failed_24h`, `jobs.restore_failed_24h`, `jobs.oldest_pending_age_sec` | > 0 / > 10 | có (bảng job) | H4 |

### 4.7 Cụm, sync, tích hợp

| Chỉ số | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|
| `sync.status`, `sync.lag_seq`, `sync.last_ok_age_sec` (khi `ENABLE_SYNC_CHANGES`) | 60 s / 300 s | có (`getSyncStatus`) | H4 |
| `cluster.state` (active / decommissioned), peers không liên lạc được | decommissioned = crit | có (`ClusterManager`) | H4 |
| `webhook.failures_5m` (hook VMS/ESC lỗi) | > 0 / > 20 % | mới | H4 |
| `kafka.queue_len`, `kafka.delivery_errors_5m` (khi `ENABLE_KAFKA`) | > 0 / liên tục | mới | H4 |

### 4.8 Transcode / GPU

| Chỉ số | Warn / Crit | Nguồn | Đợt |
|---|---|---|---|
| `transcode.streams`, `decoders`, `encoders`, `motion`, `mjpeg_ingest` | thông tin | có (`TranscodeStats.h`) | H1 |
| `gpu.backends`, `gpu.load`, `gpu.fallbacks` (tăng trong cửa sổ) | fallback tăng = warn | có | H1 |

### 4.9 Cảnh báo

| Chỉ số | Nguồn | Đợt |
|---|---|---|
| `alerts.active[]` (eventCode, khoá nhận diện, mức, từ lúc, giá trị, ngưỡng) | mới: `AlertRegistry` nhận đúng các sự kiện đang gửi qua webhook | H3 |
| `alerts.recent[]` (vòng 500 sự kiện gần nhất, kể cả `*Recovered`) | mới | H3 |

---

## 5. Gộp trạng thái

### 5.1 Mức

`ok` < `warning` < `critical`; `unknown` khi nhóm chưa đủ mẫu (vừa khởi động) hoặc bị tắt khi build (`enabled:false`, không tính vào tổng).

### 5.2 Thành phần (components)

| Component | Gồm | Luật |
|---|---|---|
| `host` | 4.1 | xấu nhất của các chỉ số, mỗi chỉ số qua hysteresis N mẫu |
| `process` | 4.2 | xấu nhất; poller delay critical ⇒ critical |
| `ingest` | 4.3 | theo **tỉ lệ** camera lỗi, không phải từng camera (1 camera hỏng không làm node đỏ) |
| `recording` | 4.4 | `stalled > 0` ⇒ warning; `stalled ≥ 5 %` hoặc ổ ghi hình không ghi được ⇒ critical |
| `delivery` | 4.5 | xấu nhất |
| `storage` | 4.1 phần ổ + 4.6 | xấu nhất |
| `cluster` | 4.7 | xấu nhất |

### 5.3 Tổng

- `overall = max(components)`, **trừ** `ingest` và `delivery` chỉ đẩy tổng lên tối đa `warning` khi lỗi nằm ở camera/người xem bên ngoài, còn `host/process/recording/storage` được đẩy lên `critical`.
- Kèm `reasons[]`: mỗi phần tử `{component, metric, level, value, threshold, since}` sắp theo mức, để app hiện ngay vì sao.
- `score` 0–100 (tuỳ chọn, cho biểu đồ xu hướng): `100 − Σ penalty` với warning = 10, critical = 30 mỗi component, chặn dưới 0. Trạng thái vẫn là thông tin chính; score không dùng để báo động.

### 5.4 Theo camera

`camera.health = ok | warning | critical` từ 4.3 + 4.4: không ghi khi lẽ ra ghi = critical; offline = critical; `fps_ratio`/`loss`/`disconnects` vượt = warning.

---

## 6. API

### 6.1 Xác thực

- Mọi API `/media/monitor/*` (đọc lẫn cấu hình) kiểm bằng `CHECK_SECRET()` có sẵn: IP phải nằm trong
  `http.allow_ip_range` (rỗng = mọi IP) và tham số `secret` phải bằng `api.secret`. **Không** thêm
  `monitor.secretKey`, header/macro xác thực riêng, hay danh sách IP riêng cho monitor.
- Không phụ thuộc `auth.enableAuthorize`; không chấp nhận `manager.apiSecret` / JWT thay thế.
- Sai khoá hoặc IP không được phép: `AuthException` như các API `CHECK_SECRET` khác.
- App gửi `secret` qua header HTTP `secret: <api.secret>`, không đưa lên query string để khỏi lộ khoá trong log
  proxy; `allArgs["secret"]` đã tự tìm theo thứ tự body/args → query → header (`getValue`, `server/WebApi.h:103-123`),
  nên `CHECK_SECRET()` đọc được header mà không phải sửa. `config/set` có thể gửi trong body POST.
- Vì app desktop giữ khoá quản trị:
  - app lưu `api.secret` mã hoá (keychain/DPAPI của OS), không ghi thô xuống đĩa, không in ra log;
  - khuyến nghị giới hạn `http.allow_ip_range` về dải mạng của máy monitor/NOC và gọi qua HTTPS (ghi trong
    tài liệu hợp đồng). Lưu ý `http.allow_ip_range` áp chung cho mọi API `CHECK_SECRET` và
    `server/WebHook.cpp:1704`, nên dải phải gồm cả các máy tích hợp đang gọi các API đó;
  - `api.secret` hiện có giá trị mặc định cố định trong code (`server/WebApi.cpp:115`): khi vẫn là giá trị
    mặc định thì log `WarnL` lúc khởi động và trả `"apiSecretIsDefault": true` trong `/media/monitor/health` và
    `/media/monitor/config/get` để app nhắc đổi. Không tự đổi giá trị này (các tích hợp hiện có đang dùng nó).
- `config/set` **không** cho sửa `api.secret` (đổi khoá quản trị vẫn sửa file cấu hình như hiện nay).

### 6.2 Endpoint (họ `/media/monitor/*`, GET, envelope `code/msg/data` như các API khác, tất cả qua `CHECK_SECRET()`)

| Endpoint | Chu kỳ app gợi ý | Nội dung | Luồng |
|---|---|---|---|
| `/media/monitor/health` | 5–10 s | `overall`, `score`, `components{}`, `reasons[]`, `uptime`, `version`, `generatedAt`, `rev` | snapshot cache, trên poller |
| `/media/monitor/summary` | 15–30 s | toàn bộ mục 4 dạng tổng hợp (không liệt kê từng camera) | snapshot cache |
| `/media/monitor/cameras?health=warning,critical&page=&size=` | 30–60 s | camera + luồng + chỉ số 4.3/4.4, mặc định chỉ camera có vấn đề | snapshot cache, lọc/phân trang trên poller |
| `/media/monitor/alerts?since=<seq>` | 10 s | `active[]` + `recent[]` có `seq` tăng dần để lấy phần mới | registry trong RAM |
| `/media/monitor/history?from=&to=&bucket_sec=&fields=` | khi mở biểu đồ | chuỗi thời gian từ `system_metrics` + `health_metrics` | `dispatchApiWork` trên `api()` |
| `/media/monitor/config/get` | khi mở tab cấu hình | mọi ngưỡng warn/crit, hysteresis, chu kỳ, baseline, `apiSecretIsDefault` | trên poller (chỉ đọc `mINI`) |
| `/media/monitor/config/set` (POST JSON) | khi bấm lưu | chỉ các khoá thuộc nhóm monitor + ngưỡng hiện có của `GlobalMonitor` (CPU/RAM/disk/reader/IO); khoá lạ ⇒ `CODE_INVALID_ARGS`; kiểm warn < crit; ghi `mINI` + `dumpFile`, phát `kBroadcastReloadConfig` | `dispatchApiWork` trên `api()` (ghi file) |

- Ngưỡng CPU/RAM/disk/reader đã sửa được qua API cấu hình người dùng hiện có (`setThreshold`). `config/set` gọi
  chung đường đó, để hai nơi không ra hai giá trị khác nhau.
- Đổi ngưỡng có hiệu lực ở chu kỳ đánh giá kế tiếp; trạng thái hysteresis đang tính được giữ nguyên.

- `rev`: tăng mỗi lần snapshot đổi; app gửi `?rev=` cũ, nếu chưa đổi server trả `{"code":0,"notModified":true}` để giảm băng thông.
- `schemaVersion: 1` trong mọi `data`.
- Không đưa mật khẩu/URL có credential của camera vào response (chỉ id, tên, IP).

Ví dụ `/media/monitor/health`:

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1,
    "mediaServerId": "ms-01",
    "generatedAt": 1760500000,
    "rev": 1834,
    "overall": "warning",
    "score": 80,
    "uptimeSec": 86400,
    "version": "…",
    "components": {
      "host": "ok", "process": "ok", "ingest": "warning", "recording": "warning",
      "delivery": "ok", "storage": "ok", "cluster": "ok"
    },
    "reasons": [
      {"component": "recording", "metric": "recording.stalled", "level": "warning",
       "value": 2, "threshold": 0, "since": 1760499700},
      {"component": "ingest", "metric": "cameras.offline_pct", "level": "warning",
       "value": 6.2, "threshold": 5, "since": 1760498000}
    ]
  }
}
```

---

## 7. Kiến trúc

```
 timer 10 s (poller của GlobalMonitor)
   └─ HealthMonitor::collect()
        ├─ đọc GlobalMonitor getters (đã thread-safe)
        ├─ getExecutorDelay (async) ─┐
        ├─ BlockingExecutors::stats() (người gọi DUY NHẤT, xem dưới)
        ├─ duyệt camera: CameraStatistic::getParams() (copy có khoá) + CameraQualityCounters (atomic)
        ├─ ingest tracker / tier / sync / alert registry
        └─ evaluate() → HealthSnapshot (const, shared_ptr) ──► swap dưới mutex
 API /media/monitor/* ──► lấy shared_ptr snapshot, serialize, trả ngay
 mỗi 60 s ──► SerialTaskQueue trên api() ──► INSERT health_metrics
```

- **`managerkit::HealthMonitor`** (`manager/Server/HealthMonitor.{h,cpp}`): singleton, khởi động trong `GlobalMonitor::start()`. Bước thu thập chia hàm theo nhóm, mỗi hàm trả struct; `HealthEvaluator` (hàm thuần, không phụ thuộc singleton) tính mức + hysteresis ⇒ unit test được không cần server.
- **Ngưỡng**: namespace `HealthMonitorConfig` đăng ký bằng `onceToken`, khoá dạng `monitor.<metric>Warn/Crit`; ngưỡng CPU/RAM/disk/reader **dùng lại** ngưỡng hiện có của `GlobalMonitor` để không có hai nguồn sự thật.
- **`BlockingExecutors::stats()`** reset cửa sổ mỗi lần gọi: HealthMonitor trở thành người gọi định kỳ duy nhất và giữ bản sao; endpoint đang gọi trực tiếp (`server/WebApi.cpp:851`) đổi sang đọc bản sao này.
- **Bộ đếm chất lượng camera** (`CameraQualityCounters`, gắn vào `CameraStatistic` hoặc `GenericRtspCameraImp`): `std::atomic<uint64_t>` cho `last_frame_ms`, `disconnects`, `frames`, `bytes`; vòng chuyển trạng thái `live` (tối đa ~200 mục / 24 h) có mutex riêng để tính `uptime_pct_24h`. Ghi trên poller của camera, đọc từ HealthMonitor — chỉ đọc atomic/copy dưới khoá, không gọi vào camera.
  - `last_frame_ms`: cập nhật trong sink frame của luồng (hạn chế: một `store` relaxed mỗi frame).
  - `loss_pct`: hỏi `MediaSource::getLossRate` **trên poller của source** khi thu thập (async, gom kết quả), hoặc để PlayerProxy cập nhật atomic mỗi giây — chọn cách sau để HealthMonitor không phải post vào hàng trăm poller.
- **Ghi hình đang chạy**: từ `PoolIngestRates::camera_mbps` (cả tier-off và HOT pool) + bộ đếm lỗi ghi atomic trong recorder; không quét đĩa.
- **`AlertRegistry`**: chỗ đang dựng body webhook `system-event` gọi thêm `AlertRegistry::Instance().onEvent(code, key, level, json)`; `*Recovered` xoá khỏi `active`. Có mutex, vòng `recent` 500 mục. Nhờ vậy cảnh báo có cả khi `hook.enable=0`.
- **Lịch sử**: migration mới `migration/mserver_updates/23_health_metrics.sql` — bảng `health_metrics(timestamp, overall, score, cameras_total, cameras_online, rec_expected, rec_active, viewers_total, poller_delay_max_ms, alerts_active, components_json)` + index `timestamp`; dọn cùng `kMetricsRetentionDays`. Không sửa `10_system_metrics.sql`. Mẫu mỗi 60 s (`monitor.historyIntervalSec`) để 30 ngày ≈ 43 k dòng.
- **Uptime camera 24 h và coverage ghi** chỉ trong RAM ở H2/H3 (mất khi restart, ghi rõ trong response `windowStartedAt`); H5 tính `coverage_pct_24h` / `gaps_24h` từ timeline đã lưu (dùng `TierRangeIndex` / timefile), chạy trên `api()` mỗi 15 phút, cache kết quả.

---

## 8. Kế hoạch thực hiện

| Đợt | Nội dung | File chính | Kiểm tra / tiêu chí đạt |
|---|---|---|---|
| **H0** | Chốt hợp đồng: rà bảng mục 4, ngưỡng, tên field với team app desktop; viết `docs/api-health-monitor.md` | docs | team app xác nhận |
| **H1** | Khung `HealthMonitor` + `HealthEvaluator` + snapshot cache; xác thực mọi API bằng `CHECK_SECRET()` (`api.secret`); `/media/monitor/config/get|set`; `/media/monitor/health` và `/summary` với nhóm host, process (poller delay, blocking pools — chuyển người gọi `stats()`), transcode/GPU; chỉ số mới loadavg, swap, NIC errors, fd | `manager/Server/HealthMonitor.*`, `GlobalMonitor.cpp`, `server/WebApiMonitor.cpp`, `server/WebApi.cpp:851` | unit `test_health_evaluator` (mức, hysteresis, gộp, `unknown`); unit auth (thiếu/sai `secret` ⇒ từ chối, IP ngoài `http.allow_ip_range` ⇒ từ chối, `secret` qua header đọc được, JWT/`manager.apiSecret` không thay được, `config/set` không đổi được `api.secret`, `apiSecretIsDefault`); unit `config/set` (khoá lạ, warn ≥ crit); chạy thật: curl 100 req/s vào `/health` không tăng CPU đáng kể, poller delay không đổi |
| **H2** | Chất lượng camera: `CameraQualityCounters`, baseline fps/bitrate lúc ổn định, `fps_ratio`, `bitrate_ratio`, `loss_pct`, `last_frame_age`, disconnects, uptime 24 h; component `ingest`; `/media/monitor/cameras` | `manager/Camera/CameraStatistic.*`, `GenericRtspCameraImp.*`, `src/Player/PlayerProxy.*`, `server/Manager.cpp` | unit vòng uptime + đếm; unit baseline (khởi động, cửa sổ dao động bị bỏ, giữ qua mất kết nối, đo lại khi đổi cấu hình); E2E camera giả (`rtsp_source`): tắt nguồn ⇒ camera critical trong ≤ 2 chu kỳ, bật lại ⇒ ok sau hysteresis; giảm fps nguồn ⇒ warning |
| **H3** | Ghi hình + người xem + cảnh báo: `recording.expected` theo lịch từng camera (atomic từ `RecordScheduler`), `active/stalled/write_errors`, play failures, `AlertRegistry`, `/media/monitor/alerts`; component `recording`, `delivery` | `manager/Camera/RecordPolicy.*`, `StorageManager`, `TierStorageManager`, recorder, `WebHook.cpp`, `HealthMonitor` | unit expected theo 4 `RecordMode` + `enableRecord=false` + ân hạn đầu giờ; E2E: lịch chỉ bật giờ hiện tại ⇒ expected = 1, giờ khác ⇒ 0 không báo stalled; motion-only không có chuyển động ⇒ không stalled; camera bật ghi + chmod thư mục record ⇒ `stalled`/`DiskWriteFailed` cùng xuất hiện trong `alerts.active`; khôi phục ⇒ biến mất; tier ON/OFF đều đạt |
| **H4** | Storage tier, S3, jobs, sync/cluster, webhook/Kafka, DB latency, restore failures; component `storage`, `cluster` | `TierStorageManager`, `SyncManager`, `ClusterManager`, Kafka producer | build `build-tier-off`, `build-sync-off`, Kafka OFF; cụm 2 node (`tests/integration/sync_cluster`): tắt node B ⇒ node A `cluster` warning; MinIO dừng ⇒ `storage` critical **Xong 2026-10-16**: `run_cluster.sh <dir> monitor` (thêm `fake_s3.py` đóng vai MinIO). Không làm: `sync.lag_seq` (phải quét transaction_log như `getSyncStatus`), `s3_error_rate_1h` (chưa có bộ đếm, sức khỏe pool đã báo mất kết nối), `jobs.oldest_pending_age_sec` (chỉ trả số pending/running). Webhook chỉ đếm lời gọi tới `hook.api_url`. |
| **H5** | Lịch sử `health_metrics` + `/media/monitor/history`; coverage/gaps 24 h từ timeline; rò rỉ RSS; đếm restart; `open_latency_p95` | migration 23, `manager/Storage/` (Repository + Imp), `HealthMonitor` | unit repository + migration; chạy thật 24 h trên rig stress (`tests/integration/stress_vms`) không tăng RSS, kích thước DB đúng ước lượng **Code xong 2026-10-16**: `run_cluster.sh <dir> history` 9/9; restart chỉ báo động khi lần chạy trước không thoát sạch (khởi động lại theo lịch không tính); `open_latency_p95` đo ở `MediaSource::findAsync`, chỉ luồng live mở được; baseline lưu bảng `stream_baselines`. Còn chạy 24 h trên rig stress. |
| **H6** | Tài liệu hợp đồng cuối, ví dụ cho app, bảng ngưỡng vận hành; đo với `ccu_test.sh --suite live500` để hiệu chỉnh ngưỡng mặc định | docs | ngưỡng không báo giả trên máy đích ở tải định mức |

Thứ tự ưu tiên nếu cần cắt: H1 → H3 (ghi hình) → H2 → H4 → H5.

---

## 9. Quyết định đã chốt, rủi ro và câu hỏi còn mở

Đã chốt 2026-10-15:

1. **Khoá**: dùng `api.secret` (`CHECK_SECRET`) cho mọi API monitor, đọc lẫn cấu hình; không thêm secret key mới (đổi 2026-10-16) — mục 6.1, 6.2.
2. **HTTP status**: luôn 200, trạng thái đọc ở `data.overall`.
3. **fps kỳ vọng**: trung bình lúc ổn định, cho mọi camera — mục 4.3.
4. **Lịch ghi**: theo giờ từng camera, `recordSchedules` trong `deviceStatistic`, đánh giá qua `RecordScheduler` — mục 4.4.

Rủi ro và câu hỏi còn mở:

5. **Chi phí `last_frame_ms` mỗi frame** với 600 luồng: một atomic store relaxed — không đáng kể, nhưng đo lại trên live500.
6. **Thông tin nhạy cảm**: response có tên/IP camera; nếu app desktop nằm ngoài mạng nội bộ cần thêm `monitor.hideDeviceIp`.
7. App cần **gộp nhiều node**: hiện chọn app tự gọi từng node; nếu cần một điểm vào, dùng lại hướng proxy trong cụm (docs/cluster-media-forward-plan.md) ở giai đoạn sau.

## Tham khảo

- Milestone XProtect – System monitor / thresholds: https://doc.milestonesys.com/2025r1/en-US/standard_features/sf_sc/sf_funda/sc_typtaskmonitorsystem.htm , https://doc.milestonesys.com/2020R3/en-US/feature_flags/ff_systemmonitor/sc_monitoryoursystem.htm
- IPVM – Recording status / VMS health check: https://ipvm.com/discussions/recording-status , https://ipvm.com/discussions/vms-health-check-status
- The Boring Lab – VMS dashboard: https://theboringlab.com/top-6-features-every-vms-dashboard-should-have-in-2024/
- USE / RED / Golden Signals: https://clickhouse.com/resources/engineering/golden-signals
- Health Check Response Format (IETF draft): https://www.ietf.org/archive/id/draft-inadarei-api-health-check-06.html
- RFC 4445 (Media Delivery Index): https://www.ietf.org/rfc/rfc4445.txt ; RFC 8451 (RTP/RTCP metrics): https://www.rfc-editor.org/rfc/rfc8451.html
