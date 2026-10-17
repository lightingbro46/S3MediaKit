# Thay www/console bằng app desktop giám sát: rà soát API và plan

Ngày: 2026-10-17. `www/console` sẽ bị bỏ, thay bằng một app desktop. App desktop **không** tạo/sửa/xoá dữ
liệu; chỉ được: kiểm tra kết nối, đọc, đặt cấu hình, khởi động lại server. Tài liệu này:

1. rà soát các API theo dõi trạng thái console đang dùng (mục 1–2);
2. trả lời `/media/monitor/*` đã thay được các API đó chưa (mục 3);
3. chốt danh sách API cho app desktop (mục 4) và bộ thông số giám sát (mục 5);
4. plan thực hiện: G1–G3 + healthcheck (đã làm), phương án gỡ console và `/media/api/*` (mục 6).

Hợp đồng `/media/monitor/*`: [api-health-monitor.md](api-health-monitor.md) (`schemaVersion: 1`).

## 1. Console hiện gọi gì

| Trang | API | Chu kỳ | Đăng ký ở |
|---|---|---|---|
| Header (`index.html:885-898`) | `/media/mserver/healthcheck` | 10 s | `WebApiConfig.cpp:141` |
| Dashboard (`dashboard.js:299-345`) | `/media/api/getStatistic`, `getThreadsLoad`, `getWorkThreadsLoad`, `systemStatistic` | 3 s, không dừng khi rời trang | `WebApi.cpp:2705-2741` |
| | `/media/api/systemStatisticHistory` | khi đổi tab Ngày/Tuần/Tháng | `WebApi.cpp:2743` |
| Cluster (`cluster.js:102,186-217`) | `healthcheck` rồi `description`, `getThreadsLoad`, `getWorkThreadsLoad`, `getStatistic`, `systemStatistic`, `getSyncStatus` | 5 s × mỗi node, không dừng | `WebApi.cpp:2483` (`getSyncStatus`) |
| Cluster, Config, header | `/index/api/getServerConfig`, `setServerConfig`, `restartServer` | khi mở / bấm lưu / bấm restart | `WebApi.cpp:894-980` |
| Devices (`devices.js:67`) | `/media/api/device/statisticsList` | khi bấm tải | `WebApi.cpp:2758` |
| Files (`files.js:190-199`) | `/media/api/listFiles`, `serveFile` | khi duyệt | `WebApi.cpp:2584-2653` |
| Storage (`storage-dashboard.js`) | 13 API đọc `/media/api/storage/*` + `pool/testConnection` + các API ghi | khi bấm tải / bấm nút | `WebApi.cpp` từ dòng 2779 |
| Player (`player/*.js`) | `/media/live/…`, `/media/record/…`, `/media/esc/recordedTimePeriod`, `/media/mserver/device/statistics`, `restoreJob/create|detail` | khi xem | — |

Tải khi để console mở (cụm 3 node, 1 tab): ~340 request/phút, trong đó `getSyncStatus` quét SQLite (mọi cursor
sync + một lần quét `transaction_log` mỗi peer) 36 lần/phút trong cụm.

## 2. Đánh giá các API theo dõi

| API | Chi phí | Nhận xét |
|---|---|---|
| `healthcheck` | rất nhẹ, không cần khoá | chỉ biết "HTTP trả lời" |
| `description` | nhẹ, nhưng console gọi lại 5 s/node dù dữ liệu tĩnh | version, OS, `maxDevice` |
| `getStatistic` | nhẹ (`ObjectStatistic::count()`) | số đếm đối tượng nội bộ, dễ hiểu sai: "Active Streams" = `MediaSource` đếm **theo từng giao thức đầu ra** (luồng × số giao thức); "TCP Sessions" gồm cả request polling của console. Giá trị cho gỡ lỗi rò rỉ, không cho giám sát |
| `getThreadsLoad` / `getWorkThreadsLoad` | mỗi lần gọi post một task đo trễ vào **mọi** poller (`WebApi.cpp:806`) — HealthMonitor đã làm việc này mỗi 10 s | console còn cộng `fd_count` các poller và gọi là "File Descriptors" — không phải số fd của tiến trình |
| `systemStatistic` | nhẹ (GlobalMonitor đọc cache) | tốt; gauge Disk của console lấy `disks[0]` (thường `/`), không phải ổ bản ghi; ngưỡng màu cứng trong JS, không theo cấu hình |
| `systemStatisticHistory` | SQLite trên pool `api` | tốt |
| `getSyncStatus` | **nặng nhất**, quét SQLite | hợp cho gỡ lỗi sync khi cần, không hợp để gọi định kỳ |
| `device/statisticsList` | duyệt bộ nhớ | trạng thái luồng/controller từng camera |

Kết luận: mọi số liệu giám sát định kỳ chuyển sang `/media/monitor/*` (snapshot tính sẵn mỗi 10 s, có ngưỡng,
hysteresis, `rev`/`since` để gọi theo thay đổi). Các API cũ chỉ còn dùng **theo yêu cầu** khi người vận hành mở
màn hình chi tiết.

## 3. `/media/monitor/*` đã thay được chưa?

**Gần đủ, còn thiếu 2 nhóm số liệu cho giám sát (G1–G2) và 1 nhóm hiển thị (G3).** Đối chiếu từng field console
đang hiển thị:

| API cũ → field | Thay bằng | Tình trạng |
|---|---|---|
| `healthcheck` | `/health` (+ `stale`, `overall`) | ✅ (app vẫn có thể dùng `healthcheck` để thử kết nối khi chưa có khoá) |
| `description.version`, `mediaServerId` | `/health.version`, `mediaServerId`, `uptimeSec` | ✅ |
| `description.osInfo`, `maxDevice`, `httpPort` | — | ⚪ thông tin tĩnh: gọi `description` một lần khi thêm node, không cần đưa vào monitor |
| `getStatistic.MediaSource/TcpSession/UdpSession/Socket/RtpPacket/Buffer…` | `summary.ingest.online`, `delivery.viewers` | ⚪ cố ý không thay (số đối tượng nội bộ, sai nghĩa) |
| `getStatistic.VideoDecoder/Encoder/Hw*/MjpegIngestTranscode/HwCodecLoad/Backends` | `summary.transcode.*`, `summary.gpu.*` | ✅ |
| `getThreadsLoad` (tải, trễ từng thread) | `summary.process.pollers.{count,delay_max_ms,delay_avg_ms,load_max}` | ✅ mức tổng có ngưỡng; ⚪ từng thread: gọi `getThreadsLoad` khi mở chi tiết |
| `getThreadsLoad.fd_count` | `summary.host.fd.{used,limit,used_pct}` | ✅ (số đúng hơn) |
| `getWorkThreadsLoad` | `summary.process.work_pollers`, `process.blocking[]` | ✅ |
| `systemStatistic.cpu/ram/nets` | `summary.host.cpu/ram/swap/nets` | ✅ (thêm load, swap, util NIC, lỗi/drop) |
| `systemStatistic.disks/disk_io/storage_write` | `summary.storage.disks[]` (chỉ mount bản ghi/DB/pool), `disk_io`, `storage_write` | ✅ (mount không liên quan cố ý bỏ) |
| `systemStatistic.threshold` | `/media/monitor/config/get` → `effective` | ✅ |
| `systemStatistic.reader.{total,live,playback}Count` | `summary.delivery.viewers` | ✅ |
| `systemStatistic.reader.devices[]` (người xem theo camera) | — | ❌ **G3** |
| `systemStatistic.record_write` (tốc độ ghi, số camera đang ghi, dự báo retention) | — | ❌ **G1** |
| Dự báo "còn giữ được bao nhiêu ngày" có mức cảnh báo | chỉ có sự kiện `RetentionCapacityLow` trong `/alerts`, không vào `components`/`reasons` | ❌ **G2** |
| `systemStatisticHistory` | `/history?fields=…,system` | ✅ (thêm chuỗi score/camera/ghi hình) |
| `getSyncStatus` | `summary.cluster.sync.{bootstrapped,decommissioned,peers[].last_ok_age_sec}` (có ngưỡng) | ✅ cho giám sát; ⚪ cursor chi tiết: gọi `getSyncStatus` khi gỡ lỗi |
| `device/statisticsList` | `/cameras?health=all` (online, luồng, controller, fps/bitrate/baseline, ghi hình, lý do) | ✅ |

✅ thay được · ⚪ không cần thay, giữ API cũ cho xem chi tiết · ❌ thiếu, cần bổ sung.

Không thuộc phạm vi monitor: dữ liệu quản lý storage — danh sách pool, policy, job, segment hết hạn, video bảo
vệ, timeline camera. Đây là việc quản trị trên VMS, app desktop không có (mục 4).

## 4. API cho app desktop

Mọi API dưới đây xác thực bằng `api.secret` **của từng node** (app lưu khoá theo node), gửi qua header
`secret` (`ArgsMap` đọc cả header, `WebApi.h:110`; app native không vướng CORS; không đưa khoá lên URL). IP
app phải nằm trong `http.allow_ip_range`. App không dùng cơ chế `authorId` (một khoá gọi sang node khác) — xem
mục 6.2.

| Nhóm | API | Cách gọi |
|---|---|---|
| A. Giám sát | `/media/monitor/health?rev=`, `/alerts?since=` | 10 s, mỗi node một vòng |
| | `/media/monitor/summary` | 30 s, khi mở trang node |
| | `/media/monitor/cameras?health=` | khi `ingest`/`recording` ≠ `ok` hoặc mở trang camera |
| | `/media/monitor/history` | khi mở biểu đồ |
| B. Xem chi tiết (đọc, theo yêu cầu, không định kỳ) | `/index/api/getThreadsLoad`, `getWorkThreadsLoad`, `getBlockingExecutors`, `getStatistic` | khi mở panel thread / gỡ lỗi |
| | `/media/api/getSyncStatus` | khi mở bảng sync một node (quét SQLite) |
| | `/media/api/listFiles`, `serveFile` | xem/tải log, file |
| C. Kiểm tra kết nối | `/media/mserver/healthcheck` (không khoá): node trả lời, `mediaServerId`, `version`, `serverLocationId` | khi thêm/sửa node, trước khi có khoá |
| | `/media/monitor/health` | khoá + IP đúng |
| D. Cấu hình | `/media/monitor/config/get`, `config/set` | ngưỡng giám sát (có kiểm tra khoá, giá trị) |
| | `/index/api/getServerConfig`, `setServerConfig` | cấu hình chung |
| E. Restart | `/index/api/restartServer` | |

Storage chỉ xem ở mức giám sát qua `/summary` (`storage.disks`, `storage.record_write`, `storage.tier.pools`,
`storage.tier.jobs`) và `/alerts`. Danh sách pool/policy/job chi tiết, timeline camera và thử kết nối pool là
việc quản trị, làm trên VMS (họ `/media/mserver/storage/*`, JWT) — app desktop không có.

Lưu ý cho app:

- `setServerConfig` nhận **mọi** khoá, kể cả `api.secret`, `http.port`, `http.allow_ip_range` — đổi sai là app
  mất kết nối tới node. App giới hạn danh sách khoá được sửa, hoặc cảnh báo + xác nhận với các khoá này.
- `restartServer` ngắt mọi luồng xem/ghi của node: bắt buộc hộp xác nhận ghi rõ tên node.
- Ngưỡng giám sát đặt qua `/media/monitor/config/set` thay vì `setServerConfig` (có kiểm tra warn < crit, báo
  `restartRequired`).
- Danh sách node: lấy từ `getServerConfig` của một node (peer list như `cluster.js:67-95`) hoặc nhập tay; khoá
  từng node do người vận hành nhập.
- Nhóm node theo `serverLocationId` (healthcheck): cùng giá trị = cùng site/nhóm đồng bộ.

## 5. Bộ thông số giám sát cho app

Nguyên tắc: **mức và ngưỡng do server tính** (`level`, `reasons[]`, hysteresis); app chỉ hiển thị và gộp nhiều
node, không tự áp ngưỡng lên số thô (trừ liên lạc/RTT do app tự đo). Vòng liên tục duy nhất là `/health?rev=` +
`/alerts?since=` mỗi 10 s; còn lại chỉ gọi khi đang mở màn hình tương ứng.

### 5.1 Tầng 0 — Liên lạc node (app tự đo)

| Chỉ số | Nguồn | Luật |
|---|---|---|
| `reachable` | kết quả gọi `/health` | 3 lần lỗi liên tiếp = **mất liên lạc** (khác `critical`) |
| `rejected` | `code≠0` / HTTP 401 | sai `api.secret` hoặc IP ngoài `http.allow_ip_range`; không thử lại dồn dập |
| `rtt_ms` | thời gian gọi `/health` | hiển thị |
| `stale`, `snapshotAgeSec` | `/health` | `stale=true` → node coi như `unknown` |
| `apiSecretIsDefault` | `/health` | nhắc đổi khoá |

### 5.2 Tầng 1 — Trạng thái node (`/health?rev=`, 10 s)

`overall`, `score`, `components.{host,process,storage,ingest,recording,delivery,cluster}`, `reasons[]`,
`alerts.{active,critical}`, `uptimeSec`, `version`. Đủ cho màn hình tổng quan cụm.

### 5.3 Tầng 2 — KPI node (`/summary`, 30 s khi mở trang node)

| Câu hỏi | Field | Chỉ số có mức |
|---|---|---|
| Camera có hình không? | `ingest.{cameras,online,offline,offline_pct,no_video,no_video_pct,health}` | `cameras.offline_pct`, `cameras.no_video_pct` |
| Có ghi đủ không? | `recording.{expected,active,starting,stalled,stalled_pct,write_errors_cycle}`, `recording.coverage.{avg_pct,low_pct,below_crit}` | `recording.*` |
| Ghi bao nhiêu, còn giữ được bao lâu? | `storage.disks[].{mount,usage_pct,free_bytes,retention_limit_pct}`, `storage.storage_write[]`, **G1** `storage.record_write.{ingest_mbps,recording_cameras,state,…}` | `disk.usage_pct`, `storage_write.writable`, **G2** `storage.retention` |
| Ổ/DB có nghẽn không? | `storage.disks[].{io_util_pct,await_ms,io_state}`, `storage.disk_io`, `storage.database.write_ms` | `disk_io.*`, `db.write_ms` |
| Tier/S3 có theo kịp không? | `storage.tier.pools[]`, `storage.tier.jobs` | `pool.*`, `jobs.tiering_failed_24h` |
| Người xem có xem được không? | `delivery.viewers`, `delivery.play.{failure_pct,failures_by_reason}`, `delivery.play.open.{live_p50_ms,live_p95_ms}`, `delivery.restore.failed_1h` | `viewers.limit_used_pct`, `play.*`, `replay.restore_failures_1h` |
| Máy có đủ tài nguyên? | `host.cpu`, `host.ram` (+ `proc_rss_growth_mb_per_h`), `host.swap`, `host.nets[]`, `host.fd` | `cpu.*`, `ram.*`, `swap.*`, `net.*`, `proc.fd_used_pct` |
| Tiến trình có nghẽn trong? | `process.pollers`, `process.work_pollers`, `process.blocking[]`, `process.monitor.{collect_ms,lag_ms}`, `process.unclean_restarts_24h` | `pollers.*`, `work_pollers.*`, `blocking.*`, `process.unclean_restarts_24h` |
| Transcode/GPU? | `transcode.*`, `gpu.*` | `gpu.fallbacks` |
| Cụm/tích hợp? | `cluster.peers_unreachable`, `cluster.sync`, `cluster.webhook`, `cluster.kafka` | `cluster.*`, `sync.*`, `webhook.*`, `kafka.*` |

### 5.4 Tầng 3 — Camera (`/cameras`)

`health`, `reasons[]`, `online`, `recording.{state,coverage_pct_24h,gaps_24h,lastWriteAt}`,
`controller.connected`, **G3** `viewers.{live,playback}`, và cho `main`/`sub`: `fps`/`fps_ratio`,
`bitrate_kbps`/`bitrate_ratio`, `loss_pct`, `last_frame_age_ms`, `disconnects_1h`, `uptime_pct_24h`.

### 5.5 Tầng 4 — Sự kiện (`/alerts?since=`, 10 s)

`active[]` = bảng cảnh báo đang mở; `recent[]` = nhật ký + thông báo. App lưu `seq` theo node, kể cả khi tắt.

### 5.6 Tầng 5 — Xu hướng (`/history`, khi mở biểu đồ)

| Biểu đồ | Field |
|---|---|
| Sức khỏe | `score`, `score_min`, `overall`, `alerts_active` |
| Camera / ghi hình | `cameras_total`, `cameras_online`, `rec_expected`, `rec_active`, `rec_stalled` |
| Người xem | `viewers_total` |
| Tiến trình | `poller_delay_max_ms`, `proc_rss` |
| Tài nguyên (`fields=…,system`) | `cpu_usage_pct`, `ram_usage_pct`, `hdd_usage_pct`, `disk_io_util_pct`, `net_rx_mbps`, `net_tx_mbps` |

`points` giữ 30 ngày (`monitor.historyRetentionDays`), `system` 7 ngày (`globalmonitor.metrics_retention_days`).

### 5.7 Gộp cụm (app tự tính, không gọi thêm)

Số node theo `overall` / mất liên lạc / bị từ chối; node `cluster.decommissioned`; lệch `version.commitHash`
giữa các node; tổng camera online/offline, ghi hình stalled, người xem (cộng từ `summary` hoặc điểm `history`
mới nhất của các node).

### 5.8 Tải

| Vòng | Console hiện tại | App desktop |
|---|---|---|
| Liên tục, mỗi node | 80 + 84 req/phút, 12 lần quét sync/phút | 12 req/phút (`/health` + `/alerts`), phần lớn `notModified` |
| Trang node đang mở | — | + 2 req/phút (`/summary`) |
| Quét SQLite do client gây ra | 12/phút/node | 0 (monitor tự đọc DB mỗi 60 s, dùng chung cho mọi client) |

## 6. Plan thực hiện

### 6.1 G1–G3 và healthcheck — ĐÃ LÀM (2026-10-17, chưa commit)

Chỉ thêm field/metric, không đổi `schemaVersion: 1`.

| # | Thay đổi | Chỗ |
|---|---|---|
| G1 | `summary.storage.record_write`: cùng object `record_write` của `systemStatistic` (`makeHotWriteJson()` khi build có tier storage, `makeRecordWriteJson()` khi không). Chỉ đọc cache RAM, chạy trên `monitor poller` | `HealthMonitor::collectStorage()` |
| G2 | Chỉ số `storage/storage.retention`: thư mục record (`detail` = đường dẫn) hoặc từng pool HOT (`detail` = tên pool). Mức = `RetentionForecast.level` đã sustain (1 → warning, 2 → critical), chưa có dự báo → unknown, `sustained=true`; `value` = `retention_days_estimate`, `threshold` = `required_days`. `storage.retention_forecast_alert_enabled=0` tắt cả chỉ số | `HealthMonitor.cpp` (`retentionLevel()`), `StorageManager::getRecordRetentionForecast()` mới |
| G3 | `/cameras`: `viewers.{live,playback}` mỗi camera; `/summary`: `delivery.viewers.cameras_viewed` | `collectIngest()`, `collectDelivery()` |
| H | `/media/mserver/healthcheck` thêm `version` (`kServerVersion`, như `description`) và `serverLocationId` (`cluster.serverLocationId`) | `server/WebApiConfig.cpp` |

Tài liệu: [api-health-monitor.md](api-health-monitor.md) mục 3.2, 3.3, 3.6; [health-monitor-runbook.md](health-monitor-runbook.md) mục 2.3.

Đã kiểm (2026-10-17): build `MediaServer` Debug tier ON đạt; tắt tier (`build-tier-off2`) biên dịch đạt các object
đổi (không link, tránh ghi đè output chung); `run_cluster.sh <dir> workerapis` ALL PASSED; chạy node-a không camera:
healthcheck trả `version`, `serverLocationId`; `/summary.storage.record_write` khớp `systemStatistic.record_write`
(chu kỳ đầu sau khởi động `pools` rỗng vì cache pool chưa nạp, chu kỳ sau đủ); `delivery.viewers.cameras_viewed`
có; chưa có dự báo nên không có `storage.retention`, `storage` = ok.

Còn phải chạy thật (tier OFF và ON, có camera ghi; dự báo cần ≥ 15 phút đo sau khởi động):

1. `/summary.storage.record_write` trùng `/media/api/systemStatistic` `record_write` cùng thời điểm.
2. Đặt thời gian lưu vượt dung lượng → `RetentionCapacityLow` trong `/alerts` **và** `reasons[]` có
   `storage/storage.retention` cùng mức; hạ xuống → cả hai hồi phục.
3. 1 người xem live + 1 xem lại một camera → `/cameras?health=all` có `viewers` = {1, 1},
   `cameras_viewed` = 1, khớp `systemStatistic.reader.devices`.

### 6.2 Gỡ `www/console` và các API `/media/api/*` — CHỐT, làm khi app desktop hoàn thiện

Điều kiện bắt đầu: app desktop đã chạy thật trên cụm, team vận hành xác nhận không còn script/tích hợp nào gọi
các API dưới đây (đã kiểm trong repo: ngoài console chỉ có `check_worker_apis.py` và tài liệu).

Hiện có 38 route `/media/api/*`, tất cả trong `server/WebApi.cpp`.

| API | Ngoài console ai dùng | Quyết định | Thay bằng |
|---|---|---|---|
| `/media/api/storage/*` (30 route: dashboard, pool, policy, job, segment, protected, mountpoint, camera summary/timeline, testConnection, encrypt/changePassword) | `check_worker_apis.py` mục 11 | **Gỡ hết** | VMS dùng `/media/mserver/storage/*` (JWT, cùng thân hàm trong `WebApiStorage.cpp`, giữ nguyên); app: `/summary.storage.*` |
| `/media/api/device/statisticsList` | không | **Gỡ** | `/media/monitor/cameras?health=all` |
| `/media/api/getStatistic` | `docs/hw-codec.md` | **Gỡ** | `/index/api/getStatistic` (CHECK_SECRET, cùng hàm); sửa tài liệu |
| `/media/api/getThreadsLoad`, `getWorkThreadsLoad` | không | **Gỡ** | `/index/api/getThreadsLoad`, `getWorkThreadsLoad` (cùng hàm) |
| `/media/api/systemStatistic`, `systemStatisticHistory` | `check_worker_apis.py`, `api-system-alert-events.md` | **Gỡ** | `/media/monitor/summary`, `/history`; VMS giữ `/media/mserver/systemStatistic*` (JWT) |
| `/media/api/cluster/access` + guard `CHECK_CLUSTER_AUTHOR_ASYNC` | chỉ các route trên | **Gỡ** | không cần: app có khoá từng node |
| `/media/api/getSyncStatus` | `check_worker_apis.py`; app nhóm B | **Giữ**, đổi guard sang `CHECK_SECRET()` (bỏ `authorId`) | — |
| `/media/api/listFiles`, `serveFile` | `check_worker_apis.py`; app nhóm B | **Giữ** | — |

Sau khi gỡ, `/media/api/*` còn 3 route: `getSyncStatus`, `listFiles`, `serveFile`.

Các bước (một commit, build cả bản bật/tắt tier storage và bản tắt sync):

1. Xoá thư mục `www/console/` (CMake chỉ `file(COPY www ...)`, `CMakeLists.txt:647`; không có code server nào
   trỏ tới `console`). Giữ phần còn lại của `www/`.
2. `server/WebApi.cpp`: xoá các `api_regist` "Gỡ" ở bảng trên (khối `#ifdef ENABLE_TIER_STORAGE` của họ
   `/media/api/storage/*` từ dòng ~2779, và các route quanh dòng 2553, 2705-2770). `getSyncStatus`: thay
   `CHECK_CLUSTER_AUTHOR_ASYNC(on_access)` bằng `CHECK_SECRET(); on_access();`.
3. Gỡ cơ chế cluster-across-auth đã hết người dùng: macro `CHECK_CLUSTER_AUTHOR_ASYNC` (`server/WebApi.h:324`),
   listener `kBroadcastClusterAcrossAccess` (`server/WebHook.cpp:2527`), khoá `hook.on_cluster_across_auth`
   (`WebHook.cpp:82,134`), broadcast `kBroadcastClusterAcrossAccess` và `BroadcastClusterAcrossAccessArgs`,
   cache `UserAuthorManager::{get,add,cleanExpired}ClusterAuthorCache`. Bỏ khoá config không cần migration
   (mINI bỏ qua khoá lạ trong file cũ).
4. Chú thích đầu `server/WebApiStorage.h`, `WebApiStorage.cpp`, `WebApiMonitor.h`: bỏ phần nói về họ
   `/media/api/*` (thân hàm giờ chỉ có một họ gọi; không gộp/inline lại để giữ diff nhỏ).
5. `tests/integration/sync_cluster/check_worker_apis.py`: bỏ mục 11 phần `/media/api/storage/*`,
   `systemStatistic*`; kiểm tương đương qua `/media/mserver/storage/*` (node chạy `auth.enableAuthorize=0`) và
   `/media/monitor/summary|history`; `getSyncStatus` gửi `secret`.
6. Tài liệu: `docs/hw-codec.md` (`/index/api/getStatistic`), `docs/api-system-alert-events.md` (bỏ
   `/media/api/systemStatistic`, thêm `/media/monitor/summary.storage.record_write`),
   `docs/vms-health-monitor-plan.md` nếu còn dẫn, tài liệu này (trạng thái).
7. Kiểm: build `MediaServer` (Debug, tier ON/OFF, sync OFF); `run_cluster.sh` mục worker APIs đạt; gọi một route
   đã gỡ trả 404; `getSyncStatus` thiếu/sai `secret` bị từ chối.

Không gỡ: `/index/api/*` (ZLMediaKit gốc, app nhóm B/D/E dùng), `/media/mserver/*` (VMS),
`/media/monitor/*`.

### 6.3 Bàn giao team app

Mục 4 (danh sách API) và mục 5 (bộ thông số), kèm [api-health-monitor.md](api-health-monitor.md) đã cập nhật.
