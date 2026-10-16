# Ngưỡng giám sát MediaServer: ý nghĩa và cách xử lý

Dành cho người vận hành và đội viết app monitor. Hợp đồng API (field, endpoint, khoá cấu hình) ở
[api-health-monitor.md](api-health-monitor.md); tài liệu này nói mỗi cảnh báo nghĩa là gì, thường do
đâu, cần kiểm gì, và cách hiệu chỉnh ngưỡng mặc định trên máy đích.

## 1. Đọc trạng thái

- Mỗi `monitor.intervalSec` (10 s) MediaServer đánh giá lại. Một mức chỉ đổi sau `monitor.sustainSamples`
  (3) chu kỳ liên tiếp, nên cảnh báo xuất hiện/biến mất chậm khoảng 30 s so với số đo.
- `overall` = mức xấu nhất của các thành phần; `ingest` và `delivery` (camera, người xem bên ngoài) chỉ
  đẩy `overall` lên tối đa `warning`.
- `unknown` = chưa đủ số liệu (vừa khởi động, chưa có baseline, số liệu chậm quá 3 chu kỳ đo). Không
  phải lỗi; kéo dài quá 5 phút sau khởi động thì xem log của thành phần đó.
- App không gọi được node (timeout, từ chối kết nối) là trạng thái riêng "mất liên lạc", không phải
  `critical`: node có thể vẫn ghi hình bình thường, chỉ API không tới được.
- Nhiều lý do cùng lúc thì xử lý từ gốc: đĩa đầy/chậm → ghi hình dừng → độ phủ thấp; poller nghẽn →
  mở luồng chậm → người xem lỗi.

## 2. Bảng chỉ số

Cột "Báo giả ở tải định mức" là nhận định trước khi đo, cần xác nhận bằng mục 3.

### 2.1 `host`: máy chủ

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `cpu.usage_pct` | 80 / 90 | Quá số camera/người xem của máy, transcode CPU, tiến trình khác trên máy | `summary.host.cpu.proc_usage_pct` để biết MediaServer chiếm bao nhiêu; `top`. Giảm camera, bật GPU cho transcode | Có thể, nếu máy được định cỡ sát (CPU 75–85 % lúc cao điểm). Ngưỡng do VMS đặt qua cấu hình server |
| `cpu.load1_per_core` | 1.5 / 3 | Nhiều tác vụ chờ CPU hoặc chờ IO (load tính cả tác vụ chờ đĩa) | Đọc cùng `storage.disk_io`: load cao mà CPU thấp = nghẽn đĩa | Thấp |
| `ram.usage_pct` | 80 / 90 | Cache file của kernel không tính; RSS MediaServer lớn, tiến trình khác | `summary.host.ram.proc_rss`; so với `history` để thấy tăng dần hay nhảy bậc | Thấp |
| `ram.proc_rss_growth_mb_per_h` | 200 / 500 | Rò rỉ bộ nhớ, hàng đợi phình (người xem chậm, đĩa chậm) | Đổi tải gần đây? Có thì chờ thêm 2 h; vẫn tăng thì gửi log + `getStatistic` cho đội phát triển, lên lịch khởi động lại | Thấp sau 30 phút đầu (đã bỏ qua) |
| `swap.used_pct` | 20 / 50 | Thiếu RAM | Thêm RAM hoặc giảm tải; swap làm poller trễ | Thấp |
| `net.util_pct` | 70 / 90 | NIC gần trần: camera vào + người xem ra | `summary.host.nets[]`; tách NIC camera/người xem, giảm người xem trực tiếp (dùng proxy) | Có thể ở máy NIC 1 Gbps (300 camera ≈ 1,2 Gbps vào) |
| `net.errors_per_sec` | 0.01 / 100 | Cáp, card mạng, driver | `ip -s link`, `ethtool -S`; lỗi đều đặn là lỗi phần cứng | Thấp |
| `net.drops_per_sec` | 10 / 100 | Buffer socket nhỏ, burst UDP/multicast, NIC ảo | `netstat -su` (receive buffer errors); tăng `net.core.rmem_max` | Có thể với RTP/UDP nhiều camera |
| `proc.fd_used_pct` | 70 / 90 | `ulimit -n` thấp hoặc rò fd | `ls /proc/<pid>/fd | wc -l`; đặt `LimitNOFILE` ≥ 65536 | Thấp nếu limit đã nâng |

### 2.2 `process`: bên trong MediaServer

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `pollers.delay_max_ms` | 50 / 200 | Một poller bị chặn bởi tác vụ nặng/blocking, hoặc CPU bão hoà | `/index/api/getThreadsLoad`; poller treo hẳn thì `delay_max_ms` tăng đều theo thời gian chờ ⇒ lấy `gstack`/`gdb -p` rồi gửi đội phát triển | Thấp; giá trị đỉnh lúc khởi động bị hysteresis lọc |
| `pollers.load_max` | 80 / 95 | Poller nóng nhất: phân bổ luồng không đều, quá số luồng/người xem | So `load_max` với trung bình; lệch nhiều = vài luồng nặng dồn một poller. Tăng số poller (`-t`) hoặc giảm tải | **Cao nhất**: poller ở tải định mức có thể quanh 70–85 % |
| `work_pollers.delay_max_ms` | 100 / 500 | Hàng đợi tác vụ nền nghẽn | Như trên | Thấp |
| `blocking.wait_max_ms` (theo pool) | 1000 / 5000 | Pool `api`/`device`/`discovery`/`ffmpeg` hết luồng: SQLite chậm, camera ONVIF chậm, ffmpeg kẹt | `summary.process.blocking[]` (`queued`, `busy_pct`); pool `device` nghẽn thường do vài camera không trả lời | Có thể với pool `device` khi nhiều camera chậm cùng lúc |
| `blocking.busy_pct` (theo pool) | 80 / 95 | Như trên | Như trên | Có thể |
| `gpu.fallbacks` | warning 300 s | GPU hết phiên/lỗi driver, transcode rơi về CPU | `summary.gpu`; log `HwCodec` | Không, nếu GPU đủ phiên |
| `process.unclean_restarts_24h` | 1 / 3 | Crash, bị kill (OOM, watchdog) | `journalctl`, `dmesg | grep -i oom`, file `crash.<pid>` cạnh binary; gửi đội phát triển | Không |

### 2.3 `storage`: ổ đĩa, DB, pool tier

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `disk.usage_pct` (theo mount) | 80 / 90; mount retention tự xoá: max(80, L + 3) / max(90, L + 6) | Retention không kịp xoá, ổ không thuộc retention bị đầy (log, DB), cấu hình retention sai | `summary.storage.disks[]` (`retention_limit_pct`, `warn_pct`, `crit_pct`); mount có retention mà vẫn vượt ngưỡng = retention không chạy được, xem log `StorageManager`/`TierStorageManager` | Không ở mount retention (đã tính L). Mount khác chứa bản ghi mà không có retention thì đúng là sẽ đầy |
| `disk_io.mount`, `disk_io.host` | theo `globalmonitor.disk_io_*` | Đĩa bão hoà (`io_util_pct`, `await_ms` cao), HDD chậm | `iostat -x 5`; ghi hình nhiều luồng trên HDD dễ chạm trần; tách DB khỏi ổ ghi hình | Có thể trên HDD; máy kernel cũ (không có PSI) chỉ dùng `iowait` |
| `storage_write.writable` | không ghi được = critical | Ổ read-only (lỗi FS), mất mount NAS, quyền thư mục | `dmesg`, `mount`, thử `touch` trong thư mục | Không |
| `db.write_ms` | 500 / 2000 | SQLite bị khoá lâu (tác vụ ghi dài), đĩa chứa DB chậm | Log `database is locked`; DB nên nằm trên SSD, không chung ổ ghi hình HDD | Có thể khi DB chung ổ HDD đang ghi hình |
| `pool.health` (theo pool) | WARNING/HIGH = W, CRITICAL/OFFLINE = C | Pool gần đầy, NAS/S3 mất kết nối | `summary.storage.tier.pools[]` (`message`); kiểm mạng tới NAS/MinIO | HIGH có thể bật/tắt quanh high watermark pool |
| `pool.throughput_low` | sustain trong PoolThroughputTracker | Pool đích nhận chậm hơn tốc độ dữ liệu cần chuyển | Băng thông tới NAS/S3; xem [pool-throughput-alert](api-system-alert-events.md) | Không, nếu đã định cỡ băng thông |
| `pool.backlog_eta_sec` | 6 h / 24 h | Dữ liệu chờ chuyển tier tồn đọng | Như trên; backlog tăng mãi thì pool HOT sẽ đầy | Thấp |
| `jobs.tiering_failed_24h` | 1 / 10 | Lỗi ghi pool đích, file nguồn mất | Log `TierStorageManager`; một vài lỗi lẻ sau sự cố mạng là bình thường | Thấp |

### 2.4 `ingest`: camera

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `cameras.offline_pct` | 5 / 20 | Camera mất điện/mạng, sai mật khẩu, switch lỗi | `/media/monitor/cameras?health=critical`: nhiều camera cùng dải IP = lỗi mạng/switch | Không |
| `cameras.no_video_pct` | > 0 / 5 | Camera giữ kết nối mà không gửi hình (treo encoder) | Camera trong danh sách với `<luồng>.no_video`; khởi động lại camera | Có thể: một camera treo ngắn đã là warning |
| Từng camera: `fps_ratio`, `bitrate_ratio` | ≤ 0.8 / 0.5, ≤ 0.5 / 0.2 | Ai đó đổi cấu hình camera, mạng camera nghẽn, camera quá nhiệt | So `fps`/`fps_baseline`; đổi chủ động thì baseline tự học lại sau 1 h | Có thể với camera bitrate thay đổi (VBR, cảnh đêm): bitrate thấp ban đêm là bình thường |
| Từng camera: `loss_pct`, `disconnects_1h`, `uptime_pct_24h` | 1 / 5, 3 / 10, ≤ 99 / 95 | Mạng camera chập chờn | Chuyển camera sang TCP, kiểm switch/cáp | Không |

### 2.5 `recording`: ghi hình

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `recording.stalled_pct` | > 0 / 5 | Camera lẽ ra ghi theo lịch mà không có segment mới: camera mất hình, đĩa lỗi, recorder lỗi | `summary.recording.stalled_cameras[]`; đi kèm `storage` lỗi thì sửa đĩa trước | Không |
| `recording.write_errors` | lỗi mới = W, 3 chu kỳ = C | Không mở/rename được file ghi | Như `storage_write.writable` | Không |
| `recording.storage_writable` | critical | Thư mục bản ghi chính không ghi được | Như trên | Không |
| `recording.coverage_low_pct` | > 0 / 5 | Bản ghi 24 h có lỗ (camera mất kết nối, node khởi động lại, đĩa đầy) | `/media/monitor/cameras` cột `coverage_pct_24h`, `gaps_24h`; chỉ là hệ quả, tìm nguyên nhân trong `history` | Không |

### 2.6 `delivery`: người xem

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `viewers.limit_used_pct` | 80 / 95 | Gần giới hạn người xem của license | Mua thêm/chia node | Không |
| `play.failure_pct` | 5 / 20 | Token hết hạn, sai quyền, vượt giới hạn | `summary.delivery.play.failures_by_reason`: `Unauthorized` = phía app/VMS, `MaxRequest` = giới hạn | Có thể khi app giữ token hết hạn và thử lại liên tục |
| `play.open_latency_p95_ms` | 2000 / 5000 | Kéo luồng theo yêu cầu chậm, poller nghẽn | `summary.delivery.play.open` | Thấp với camera ghi 24/7 (luồng đã mở sẵn) |
| `replay.restore_failures_1h` | 1 / 10 | Restore từ S3/NAS lỗi | Log restore, kết nối tới pool lạnh | Không |

### 2.7 `cluster`: cụm và tích hợp

| Chỉ số | W / C | Thường do | Kiểm tra / xử lý | Báo giả ở tải định mức |
|---|---|---|---|---|
| `cluster.peers_unreachable` | warning | Node khác tắt hoặc mất mạng giữa các node | Kiểm node đó từ app (mất liên lạc?) | Không |
| `cluster.decommissioned` | critical | Node đã bị bỏ khỏi cụm trên ESC | Thêm lại trên ESC hoặc tắt node | Không |
| `sync.bootstrapped` | warning | Node mới chưa đồng bộ xong dữ liệu ESC | Chờ; kéo dài thì xem log `SyncManager` | Không |
| `sync.last_ok_age_sec` (theo peer) | 120 / 600 | Đồng bộ với peer bị kẹt/lỗi | Log `SyncManager`, mạng giữa các node | Không |
| `webhook.failure_pct` | > 0 / 20 | Backend VMS/ESC (`hook.api_url`) không trả lời/lỗi 5xx | Kiểm backend; dưới 10 lần gọi thì chỉ warning | Có thể khi backend bận (một lần timeout đã là warning) |
| `kafka.spool`, `kafka.dropped` | warning | Mất kết nối broker, spool đầy | Kiểm broker Kafka | Không |

## 3. Hiệu chỉnh ngưỡng mặc định trên máy đích

Mục tiêu: ở tải định mức (300 camera ghi 24/7 cả hai luồng + 500 người xem) monitor không báo gì,
trong khi vẫn báo khi bậc tải thật sự trượt.

1. Chạy suite live500 bằng bundle `ccu_test.sh` (hướng dẫn: [live500-perf-test.md](live500-perf-test.md)).
   Mỗi bậc tải lấy mẫu `/media/monitor/health` mỗi 10 s trong cửa sổ đo.
2. Đọc `SUMMARY.md`: dòng **Monitor ở bậc 500** = `đúng (không báo)` là đạt; `báo giả?` kèm danh sách
   chỉ số cần xem. Bảng "Monitor `/media/monitor/health` trong cửa sổ đo" của từng kịch bản cho mọi bậc:
   - `báo giả?`: bậc đạt (người xem và ghi hình tốt) mà monitor báo.
   - `bắt được`: bậc trượt và monitor cũng báo.
   - `bỏ sót`: bậc trượt mà monitor không báo gì: thiếu chỉ số hoặc ngưỡng quá rộng.
3. Lọc báo giả do chính bộ sinh tải: harness chạy nguồn RTSP và người xem trên cùng máy, nên
   `cpu.usage_pct`, `cpu.load1_per_core`, `ram.usage_pct` (đo cả máy) bị đội lên. Chỉ hiệu chỉnh các
   chỉ số đó khi bộ sinh tải chạy máy khác (`--viewer-hosts`) hoặc so với `proc_usage_pct`.
4. Với mỗi báo giả còn lại:
   - Chỉ số tài nguyên (poller load, busy pool, NIC, IO): nâng warn lên trên giá trị max ở bậc 500
     khoảng 10 % nhưng vẫn dưới giá trị ở bậc trượt đầu tiên; giữ crit dưới bậc trượt.
   - Chỉ số chức năng (ghi hình, không ghi được, camera offline, crash): **không nới**; đó là lỗi thật
     của bậc, tìm nguyên nhân.
5. Thử ngưỡng mới không cần build lại: `POST /media/monitor/config/set` (hoặc `--set monitor.<khoá>=` của
   bundle) rồi chạy lại `--only live_step`. Đạt thì sửa giá trị mặc định trong
   `manager/Server/HealthMonitor.cpp` và bảng 3.3 của [api-health-monitor.md](api-health-monitor.md).
6. Ghi kết quả vào bảng dưới.

| Ngày | Máy | Binary | Bậc 500 | Báo giả | Đã đổi |
|---|---|---|---|---|---|
| (chưa chạy) | | | | | |
