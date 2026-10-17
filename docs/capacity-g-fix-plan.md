# Plan khắc phục sau lượt đo G trên db3

Nguồn: [recording-server-capacity-test.md](recording-server-capacity-test.md) mục 10.1–10.4
(G-a, G-b, G-c ngày 2026-10-16, binary `e331cbe`). Mục tiêu: hết rớt luồng khi nạp cấu hình,
replay không khựng khi segment đóng dồn, rồi chạy lại lượt G đúng điều kiện.

## 0. Nguyên nhân đã xác định (tóm tắt)

| # | Hiện tượng | Nguyên nhân trong code | Ở develop |
|---|---|---|---|
| A | G-a: 53–100 luồng `receive rtp timeout` ở 500/600/800 camera | Callback của hook `server_load` chạy `loadServerConfigJson` (addCamera cho mọi camera, SQLite policy mỗi camera) **trên event poller**. `WebHook.cpp` cố đẩy sang `WorkThreadPool`, nhưng work poller tạo với `ref_self=false` nên `EventPoller::getCurrentPoller()` rỗng, `HttpRequester` lấy event poller rảnh nhất của `EventPollerPool` | **còn lỗi** |
| B | G-c: replay khựng 2–8 s theo từng work poller | Đóng segment + listener `kBroadcastRecordMP4` chạy trên work poller, cùng chỗ `MP4Reader` | đã sửa ở `489f98e3` (pool `rec close N`); binary test thiếu commit này |
| C | Đợt đóng segment kéo dài nhiều giây | 600 segment đóng trong ~10 s mỗi phút (camera khởi động cùng lúc); mỗi lần đóng ~0,3–0,5 s khi SSD chậm | `record.closeThreads` mặc định **2**: ~180–300 s việc/phút cho 120 s thread/phút → hàng đợi có thể dồn mãi |
| D | `MP4Reader` replay dùng chung `WorkThreadPool` | mọi việc nặng khác còn trên work poller (ESC write, sync, tier queue) vẫn làm replay khựng | còn |
| E | SSD/RAID chậm dần khi ghi lâu | phần cứng + flush theo đợt (`dirty_ratio` 20 %) | ngoài code |

## 1. Quyết định cần chốt trước

- **D1 — binary cho lần chạy lại.** Khuyến nghị: build từ develop + nhánh sửa của plan này
  (`fix/capacity-g`). Nếu bản production phải là `feature/anhnq48-replay-timeline` thì chủ nhánh đó
  merge develop (để có `489f98e3`) và nhánh sửa này trước khi build. Ghi commit vào Bảng 0.
- **D2 — bỏ qua nạp cấu hình không đổi (P1.3).** Vòng nạp mỗi 10 s hiện cũng là vòng "đối soát"
  (bật lại `setSyncMode(true)`, áp lại option). Khuyến nghị: bỏ qua khi JSON không đổi nhưng vẫn áp
  lại toàn bộ định kỳ (`hook.configReapplySec`, mặc định 300 s).

## 2. Các phase

Làm trên nhánh `fix/capacity-g` tách từ develop. Mỗi phase một commit, build `build/` + unit liên
quan; phase có chạy thật ghi kết quả vào commit message.

### P1 — Nạp cấu hình không chạy trên poller (K2, sửa A)

1. `server/WebHook.cpp` (`reportServerStatistic`): trong callback của `do_http_hook(server_load)` chỉ
   giải mã JSON rồi đẩy `loadServerConfigJson(config)` sang một `SerialTaskQueue` trên
   `BlockingExecutors::api()` (mẫu đã dùng ở HealthMonitor H5). Nếu lần trước chưa xong thì bỏ lần này
   (không xếp chồng). Sửa comment sai về `WorkThreadPool`/`getCurrentPoller`.
2. Rà các chỗ khác dựa trên cùng giả định "chạy từ WorkThread thì HttpRequester dùng work poller"
   (`rg "isolated EventPoller|uncontested poller"`): callback nào làm việc nặng thì xử lý như trên.
3. Bỏ qua nạp khi không đổi (theo D2): giữ hash (`std::hash` của chuỗi JSON đã chuẩn hoá phần
   `devices`, `speakers`, `media_server`, `list_media_server`, `watermark_configs`) của lần áp dụng
   thành công gần nhất; trùng hash và chưa quá `hook.configReapplySec` thì chỉ log Trace. Khoá mới
   đăng ký bằng `onceToken` trong config, mô tả trong `conf/config.ini`.
4. Đo: log `Server configuration for camera loaded completed, took N ms` kèm tên thread (đã có).

Kiểm tra:
- Unit: không có test sẵn cho đường này; không tách API chỉ để test (theo nguyên tắc diff nhỏ).
- Chạy thật trên máy dev bằng `tests/integration/stress_vms` (120 camera, mock API): mọi dòng
  `loadServerConfigJson` chạy trên `api worker`, không còn trên `event poller`; không có
  `receive rtp timeout`; sửa camera qua mock API vẫn được áp trong ≤ 10 s; cấu hình không đổi thì
  chỉ áp lại mỗi `configReapplySec`.

### P2 — Đóng segment theo kịp ở 300+ camera (K1 phần còn thiếu, sửa C)

1. Đo trước khi sửa: trong `MP4Recorder::asyncClose` (pool `rec close`) đo thời gian ghi moov/rename
   và thời gian các listener `kBroadcastRecordMP4`; log Warn khi > 1 s; thêm getter tĩnh số việc đang
   chờ của các pool (đếm `atomic` tăng khi post, giảm khi xong).
2. Đưa số việc chờ vào HealthMonitor (nhóm process, ví dụ `record.close_backlog` cảnh báo khi > số
   segment của 1 phút), vì lỗi dồn hàng đợi này hiện không ai thấy.
3. Đổi mặc định `record.closeThreads` theo kết quả đo (dự kiến 8). Thread bị chặn chủ yếu vì
   I/O/SQLite nên thêm thread không tốn CPU; thứ tự theo stream vẫn giữ vì chọn pool theo hash stream.
4. Nếu phần SQLite (`registerHotSegmentRange`, timefile) chiếm phần lớn thời gian: ghi nhận phương án
   gom nhiều range vào một transaction `beginWriteTxn()`; chỉ làm khi số đo cho thấy cần.

Kiểm tra: stress_vms 120 camera trên máy dev, segment 20 s để tăng tần suất: backlog về 0 sau mỗi
đợt, mọi `Record mp4 file` chạy trên `rec close N`; `unit_storage`, `unit_health_monitor` đạt.

### P3 — Replay có nhóm poller riêng (K3, sửa D)

1. Thêm nhóm poller riêng cho replay trong `src/Record/` (mediakit, không phụ thuộc managerkit):
   `TaskExecutorGetterImp` với `addPoller("replay poller", N, PRIORITY_HIGHEST, true, …)`, khoá
   `record.replayPollerThreads` (mặc định 8).
2. `MP4Reader::setup` (`src/Record/MP4Reader.cpp:32`) và `MergedMP4Reader` (`:96`) dùng nhóm này khi
   không được truyền poller, thay cho `WorkThreadPool`.
3. Đọc trước (prefetch) vài giây media chỉ làm nếu sau P1–P3 replay vẫn còn khựng ở lần chạy lại.

Kiểm tra: `unit.replay_all` (lưu ý có lúc abort khi chạy -j2, chạy riêng), E2E replay trên máy dev
(fMP4/HLS/RTSP, seek, per-stream `?duration=`); log `OwnerPoller changed … -> replay poller N`.

### P4 — Rải thời điểm đóng segment (K4, giảm C)

1. `MP4Recorder`: segment **đầu tiên** sau khi recorder bắt đầu dài
   `max_second/2 + hash(stream) % (max_second/2)` giây thay vì `max_second`; các segment sau giữ
   nguyên. Sau khởi động lại 300 camera, các đợt đóng trải đều khoảng nửa phút thay vì dồn vào ~10 s.
2. Không áp cho recorder `continuous` của replay/gap fill (`StreamSource.cpp:149`), vì thời gian file
   ở đó do nguồn quyết định.
3. Rà chỗ giả định segment đầu đủ `mp4_max_second` (timeline, retention, kiểm độ phủ của
   HealthMonitor dùng `2 × mp4_max_second` làm ngưỡng hở — vẫn đúng).

Kiểm tra: `unit_storage`, `unit_timeline_query`; stress 120 camera: phân bố giây đóng segment đều
trong phút (đếm từ log `Record mp4 file`).

### P5 — Sửa harness (K6)

`tests/integration/stress_vms`:
1. `check_ccu.py`: đếm `receive rtp timeout` và khoảng hở ghi hình từ lúc camera đã online (cuối
   ramp), không chỉ trong cửa sổ chấm; báo riêng cột "rớt ngoài cửa sổ".
2. `viewer_load.py`: ghi thời điểm (đồng hồ thực) của từng lần khựng > 1 s vào
   `ccu_replay_sessions.csv`.
3. `ccu_runner.sh`: chụp `nstat -az TcpRetransSegs TcpExtListenOverflows` và `/proc/net/softnet_stat`
   trước/sau mỗi bậc; cảnh báo khi `--ms-cpus` và `--gen-cpus` chứa hai luồng HT của cùng một lõi
   (đọc `thread_siblings_list`); cảnh báo khi còn `CAM_HOLD_SEC`/`PHASE_SEC` ở lượt không phải smoke.
4. Ghi thread của dòng `Record mp4 file` và `loadServerConfigJson` vào báo cáo (đếm theo nhóm thread)
   để lần sau thấy ngay việc chạy sai thread.
5. Đóng bundle mới `make_ccu_bundle.sh release/ccu_test.sh`.

Kiểm tra: smoke trên máy dev (20/50 camera), đọc `SUMMARY.md` có các cột mới.

### P6 — Máy và báo cáo (K5, K7)

Không sửa code; ghi vào `recording-server-capacity-test.md`:
1. Hỏi admin db3: model SSD, mức RAID, write-back cache/BBU của SMC3108; đo `fio --rw=write --bs=1M
   --size=200G --direct=1` trên `/work` khi máy rảnh.
2. Thử `vm.dirty_background_bytes=268435456`, `vm.dirty_bytes=2147483648` trong một lượt `cam_step`
   ngắn để so `w_await`/util với lượt G-b; chỉ đề xuất cho production nếu tốt hơn rõ.
3. Ghi giới hạn NIC db3 (~385 camera RX / ~1 100 viewer TX với `bond0` 2 Gbps) vào phần kết luận.

### P7 — Chạy lại lượt G trên db3

1. Build Release từ nhánh theo D1, image `s3mk-captest` mới, bundle mới (P5).
2. Lệnh: `--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31`, không đặt `CAM_HOLD_SEC`/`PHASE_SEC`;
   a → b (`--cameras 300`) → c, tag hậu tố `_r2`.
3. Tiêu chí ngoài cách chấm của bundle:
   - không `receive rtp timeout` sau khi camera đã online;
   - `loadServerConfigJson` chỉ trên `api worker`; `Record mp4 file` chỉ trên `rec close`;
   - backlog `rec close` về 0 sau mỗi phút;
   - replay ≥ 99 % phiên đạt;
   - ghi lại `w_await`/util SSD cuối mỗi bậc camera.
4. Ghi kết quả vào mục 10.5 của báo cáo, so với lượt sơ bộ.

## 3. Thứ tự và ước lượng

| Phase | Phụ thuộc | Ước lượng |
|---|---|---|
| P1 | – | 0,5 ngày + chạy thật |
| P2 | – | 1 ngày (đo trước, sửa sau) |
| P3 | – | 0,5–1 ngày |
| P4 | P2 (đo đợt đóng) | 0,5 ngày |
| P5 | – | 1 ngày |
| P6 | – | chờ admin db3 |
| P7 | P1–P5, D1 | ~6 giờ chạy trên db3 |

P1 và P2 là bắt buộc trước P7; P3, P4 nên có; P5 cần để đọc đúng kết quả lần chạy lại.
