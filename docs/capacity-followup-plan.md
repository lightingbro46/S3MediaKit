# Plan nâng cấp sau các lượt đo đợt 2 trên db3

Nguồn: [recording-server-capacity-test.md](recording-server-capacity-test.md), mục 10.6–10.14. Đó là
các lượt smoke, G-a (lần 2, lần 3), G-b, G-c, C4-a, C4-b, C8-a, C8-b, chạy ngày 2026-10-19/20 với binary và bundle
`3388afc`. Plan trước, [capacity-g-fix-plan.md](capacity-g-fix-plan.md) (K1–K7, P1–P7), đã xong và
được kiểm trên db3. Plan này gom những việc **còn mở** sau các lượt đó.

Mã L1–L8 giữ nguyên như trong báo cáo. Các mục mới đánh tiếp từ L9.

**Trạng thái (2026-10-20, nhánh `feature/capacity-followup`):** đã code L8, L1 bước 1 (đo), L3 bước 1,
L6 bước 1–2, L7, L9, L10, L11 (sửa Dockerfile trong báo cáo mục 3.2), L12, L13. Còn: L1 bước 2 (chờ số đo),
L3 bước 2 (giới hạn cứng, chưa làm: cảnh báo + log lỗi thay thế), L6 bước 3, và L2, L4, L5, các lượt đo
mục 4 trên db3.

## 0. Phát hiện chính (tóm tắt)

| # | Hiện tượng | Nguyên nhân | Mục báo cáo |
|---|---|---|---|
| A | C4 (4 poller): 352 luồng timeout cùng lúc ở 300 camera, khoảng hở 5–7 s; C4-b 200 camera: 26/200 viewer khựng > 2 s ở một bậc; ở G, `pollers.delay_max_ms` tăng ~2,8 ms mỗi camera | `CameraStatisticImp::save()` ghi `info.txt` (mở `trunc` + ghi + đóng) **đồng bộ trên event poller**, mỗi phút cho từng luồng; khi đĩa bận mỗi lần 60–130 ms, cộng dồn ~6 s mỗi poller | 10.11, 10.12 |
| B | Đóng segment chậm 1–5 s, hàng đợi `rec close` dồn tới 27 s (700 camera, đĩa nhanh) hoặc tăng không giới hạn (600 camera, đĩa chậm) | listener `kBroadcastRecordMP4` (timefile, SQLite tier, archive size) ghi trên cùng ổ với video; IO file chỉ ≤ 100 ms | 10.6, 10.10 |
| C | Bậc vượt trần đĩa: event poller kẹt chờ ghi, timeout hàng loạt, fd/RSS tăng tuyến tính | page cache bẩn đầy (`dirty_ratio` 20 %), kernel chặn tiến trình ghi; hàng đợi `rec close` không có giới hạn | 10.6 |
| D | Cùng 150 MB/s, util SSD lúc 17 %, lúc 44–50 %; trần ghi 350 → 300 MB/s | nghi SSD sau RAID SMC3108 không nhận TRIM, hoặc trạng thái cache RAID; chưa đo | 10.7, 10.10 |
| E | Harness chấm sai hoặc bỏ sót | gán "không hợp lệ (bộ sinh tải)" khi MediaServer đã sập; không bắt hàng đợi `rec close` tăng; dòng tổng `live_replay` sai; image thiếu `perf` | 10.6, 10.8, 10.10 |
| F | Monitor báo critical ở mọi bậc | `pollers.load_max` đo thời gian poller thức (kể cả lúc kẹt syscall), không phải CPU; `delay_max` là thật (do A) nhưng không có tên poller | 10.6 |

## 1. MediaServer

### L8 — Ghi `info.txt` khỏi event poller (ưu tiên 1)

Chỗ sửa: `manager/Camera/CameraStatistic.cpp` (`save()`, `addStreamStatistic` và các hàm gọi `save()`
trên poller), `manager/Local/FileRecorder.h`.

1. Chuyển `_file->save(...)` sang hàng đợi ESC của camera (`postEscTask`, đã giữ thứ tự cho từng
   camera) hoặc `BlockingExecutors::device()`. Chụp snapshot dữ liệu dưới `_mtx`, ghi ngoài khoá.
2. Gộp các lần ghi: nếu đã có một lần ghi đang chờ thì chỉ cập nhật snapshot, không xếp thêm việc.
3. `FileRecorder::save_string()`: ghi file tạm rồi `rename`, thay cho `trunc` (tránh file rỗng nếu
   crash giữa chừng, và tránh XFS đẩy ghi lúc đóng file vừa truncate).
4. Bỏ ghi khi chỉ có `byte_speed` đổi, vì đây là số đo, không phải trạng thái cần khôi phục sau
   restart. Cần kiểm xem có chỗ nào đọc lại `byte_speed` từ `info.txt` không.

**Đã làm:** `save()` mặc định xếp một lần ghi lên hàng đợi ESC của camera; lần ghi lấy snapshot lúc
chạy, nên nhiều cập nhật liên tiếp gộp lại thành một lần ghi. Ba setter cấu hình (`setDeviceTuple`,
`setStreamTuples`, `setCameraOption`) vẫn ghi đồng bộ, không chạy trên poller. Hàm huỷ ghi nốt lần ghi
còn đang chờ. `remove()` và các lần ghi dùng chung `_file_mtx`, nên lần ghi xếp hàng trước đó không tạo
lại file đã xoá. Số thứ tự snapshot chặn ghi đè bản mới bằng bản cũ. `FileRecorder` ghi `*.tmp` rồi
`rename`. Không có chỗ nào đọc lại `byte_speed` từ `info.txt` (`load()` có nạp, nhưng chỉ để trả
API). Unit test: `CameraStatisticSyncTest.{StatisticSaveIsCoalescedOffTheCallerThread,
QueuedSaveDoesNotRecreateRemovedFile, DestructorFlushesQueuedSave, ByteSpeedOnlyUpdateDoesNotRewriteFile}`.
Còn chạy lại C4-a, C4-b.

Kiểm tra: unit test cho việc gộp ghi và cho thứ tự ghi trên ESC queue. Trên log, khoảng cách
`addStreamStatistic` → `syncResourceStatus` trên event poller ≤ 1 ms. Chạy lại C4-a và C4-b (200 camera): kỳ vọng hết
timeout ở 300 camera, hết khựng theo nhịp phút của viewer, giới hạn chuyển sang CPU (~300 camera). Ở G, `pollers.delay_max_ms` giảm rõ.

### L1 — Đo và giảm thời gian listener đóng segment (ưu tiên 1)

Chỗ sửa: `src/Record/MP4Recorder.cpp`, `server/Manager.cpp` (listener timefile, `addArchiveSize`),
`TierStorageManager` (`registerHotSegmentRange`).

1. Ghi thời gian riêng của từng listener vào log `MP4 close timing` (hoặc metrics close shard) để
   biết phần nào chậm.
2. Theo kết quả: gộp ghi SQLite của nhiều segment vào một transaction (`beginWriteTxn`), hoặc chuyển
   phần không cần đồng bộ (archive size) sang ghi định kỳ.

**Đã làm bước 1:** `MP4Recorder::addCloseStep()`. Log `MP4 close timing` (Debug) và `Slow MP4 close`
(Warn, > 1 s, kèm đường dẫn) có thêm `timefile_ms`, `archive_size_ms`, `tier_register_ms`,
`encryption_ms`, `motion_ms`. Bước 2 chờ số đo G-a.

Kiểm tra: G-a 500–700 camera, listener p99 giảm so với 775–1 558 ms; hàng đợi `rec close` không dồn
quá vài giây.

### L3 — Giới hạn và cảnh báo hàng đợi `rec close` (ưu tiên 2)

Chỗ sửa: `MP4Recorder::closeExecutorFor`, `manager/Server/HealthMonitor.cpp`.

1. Monitor thêm chỉ số từ metrics close shard: `oldest_queued_ms` (warning > 30 s, critical > 60 s)
   và `queued`.
2. Cân nhắc giới hạn cứng: khi hàng đợi vượt ngưỡng, ghi log lỗi và cảnh báo thay vì tăng RSS/fd vô
   hạn. Không được bỏ segment.

**Đã làm bước 1:** `summary.recording.close` (`queued`, `oldest_queued_ms`, `oldest_pool`), ngưỡng
`recording.close_oldest_queued_ms` 30 000 / 60 000 (`monitor.recordCloseQueuedMsWarn/Crit`). Ở mức
critical, mỗi chu kỳ ghi một dòng `ErrorL`. Chưa có giới hạn cứng.

### L6 — Sửa chỉ số poller của monitor (ưu tiên 2)

Chỗ sửa: `manager/Server/HealthMonitor.cpp`.

1. Ghi tên poller có `delay_max` lớn nhất vào summary.
2. `pollers.load_max`: dùng CPU thật của thread (như harness đọc từ `/proc`), hoặc đổi tên và nới
   ngưỡng. Hiện chỉ số này báo 95–100 % khi CPU thật chỉ 9–31 %.
3. Xem lại ngưỡng `delay_max_ms` 50/200 ms sau khi có L8.

**Đã làm bước 1–2:** thêm `delay_max_name` cho `pollers` và `work_pollers`. Ngưỡng `pollerLoadPct`
nay áp cho chỉ số mới `pollers.cpu_max_pct`: CPU thật của event poller bận nhất, đọc
`/proc/self/task/*/stat` giữa hai chu kỳ, kèm `cpu_max_name`. `load_max` vẫn có trong summary nhưng
không còn ngưỡng. App monitor nếu đang lọc theo khoá `pollers.load_max` thì phải đổi sang khoá mới.

## 2. Triển khai và cấu hình

### L4 — Tách SQLite khỏi ổ ghi hình (ưu tiên 2)

Đặt `esc.sqlite`/`mserver.sqlite` và `info.txt` trên ổ khác ổ ghi hình (ổ hệ điều hành hoặc ổ nhỏ
riêng), để commit SQLite không phải chờ sau dòng ghi video. Kiểm trên db3 bằng một lượt G-a 500–700 có
DB ở ổ khác, so listener p99 với lượt cùng điều kiện.

### L5 — Đĩa: trần thật, TRIM, cache RAID, sysctl (ưu tiên 1, chặn việc chốt ngưỡng)

Quy trình chạy: báo cáo mục 5.6.1 và 5.6.3.

1. Thông tin RAID/SSD (`storcli64`, `smartctl`), kiểm TRIM (`lsblk -D`, `fstrim`).
2. `fio` ghi tuần tự trực tiếp, chạy hai lần: lúc đĩa sạch và sau khi xoá nhiều dữ liệu.
3. Thử `vm.dirty_background_bytes=256 MB`, `vm.dirty_bytes=2 GB`, `vm.swappiness=10` trong lượt
   `Ga_dirty`.

Kết quả dùng để chốt ngưỡng camera cho db3 (hiện tạm chốt ~400) và đề xuất sysctl cho production.

### L2 — Số thread `rec close` (ưu tiên 2)

Lượt `Ga_close4`, `Ga_close8` ở 700 camera (báo cáo mục 5.6.3), so với 2 thread. Nên chạy sau L1/L8,
vì listener chậm đang là phần lớn thời gian của mỗi lần đóng.

## 3. Harness (`tests/integration/stress_vms/`)

| # | Việc | Chỗ sửa | Ưu tiên |
|---|---|---|---|
| L7 | Không gán "không hợp lệ (bộ sinh tải quá tải)" khi CPU nguồn dưới ngưỡng và MediaServer đã có timeout/kéo lại trước khi nguồn trễ nhịp. Khi đó gán "trượt" theo yếu tố giới hạn | `check_ccu.py` | 2 |
| L9 | Thêm tiêu chí trượt: `oldest_queued_ms` cuối cửa sổ > 60 s, hoặc hàng đợi `rec close` tăng đều suốt cửa sổ (bậc 600 của G-a lần 3 bị chấm "đạt" trong khi hàng đợi tăng 1 448 → 5 355) | `check_ccu.py` | 1 |
| L10 | Dòng `live_replay` trong bảng kịch bản đầu `SUMMARY.md` ghi `chua chay / khong co bao cao` dù có báo cáo `ccu_replay` | `ccu_runner.sh` (phần dựng SUMMARY) | 3 |
| L11 | Thêm `linux-tools` vào image `s3mk-captest` để có mục hàm nóng (`perf`) | Dockerfile captest (báo cáo mục 3.2) | 3 |
| L12 | Ghi `stall_events` cho mọi lần khựng ≥ 0,3 s (hiện chỉ > 1 s), để đối chiếu khựng ngắn của replay với lúc đóng segment | `viewer_load.py` | 3 |
| L13 | Summary của `live_step` ghi "mọi bậc đều đạt, chưa chạm trần" khi có bậc trượt lẻ (C8-b: bậc 100 và 600 trượt, mục 10.14). Ghi thêm số bậc trượt lẻ và danh sách bậc vào summary và dòng tổng của SUMMARY | `check_ccu.py` (`scenario_step`) | 2 |

Kiểm tra: regression Python của harness (`test_capacity.py`) thêm case cho L7, L9, L13; dựng lại bundle
bằng `make_ccu_bundle.sh`.

## 4. Đo tiếp trên db3

Theo thứ tự. Mỗi lượt dùng binary và bundle cùng commit.

1. **L5 bước 1–2** (thông tin đĩa, `fio`): biết trần đĩa trước.
2. **Baseline A/B** (báo cáo mục 5.6.2): `Gc_base`/`Gc_new`, `Ga_base`/`Ga_new`, chạy liền nhau.
   Dùng được binary `3388afc`.
3. **Build binary mới có L8** (và L1 nếu kịp), bundle có L7, L9. Chạy lại **C4-a**, rồi **C4-b**
   (`--cameras` theo ngưỡng mới, hoặc 200 nếu vẫn ≤ 200). Chạy lại cả **C8-a**, **C8-b**: với `3388afc`,
   C8-a ra 400 (bậc 500 trượt healthcheck do L8, 600 vượt trần đĩa), C8-b có 2 bậc trượt lẻ do L8
   (báo cáo mục 10.13, 10.14).
4. **L5 bước 3** (`Ga_dirty`) và **L2** (`Ga_close4`, `Ga_close8`) với binary mới.
5. Các lượt còn lại của ma trận: C24, M16, M8, H (báo cáo Bảng 1).
6. Bổ sung, ưu tiên thấp:
   - Replay đọc lạnh từ đĩa: bản ghi cũ hơn 1 giờ, page cache đã bị đẩy ra, khi SSD đang ghi ở mức
     gần ngưỡng (G-c đợt 2 chủ yếu đọc từ page cache).
   - HTTP-fMP4 ở 2 500 / 3 000 viewer để tìm trần trên loopback (ở 2 000 đã trôi 1,4 s).
   - NIC 10 Gbps: chỉ đo được với máy viewer thứ hai (báo cáo mục 5.4). db3 có `bond0` 2 Gbps,
     giới hạn ~385 camera / ~1 100 viewer.

## 5. Thứ tự và ước lượng

| Việc | Phụ thuộc | Ước lượng |
|---|---|---|
| L8 | – | 1 ngày + test |
| L1 (đo) | – | 0,5 ngày; phần sửa tuỳ kết quả đo |
| L9, L7 | – | 0,5 ngày |
| L3, L6 | L1 (metrics) | 1 ngày |
| L10, L11, L12 | – | 0,5 ngày |
| L5, A/B | admin db3 | ~4 giờ chạy |
| C4 chạy lại, L2, `Ga_dirty` | L8, bundle mới | ~4 giờ chạy |
| C8 … H | các mục trên | ~3 giờ mỗi lượt |
