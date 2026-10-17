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

- **D1 — binary cho lần chạy lại.** Triển khai trên nhánh hiện có `fix/capacity-service`, đã có
  executor `rec close`. Nếu bản production phải là `feature/anhnq48-replay-timeline` thì chủ nhánh đó
  merge develop (để có `489f98e3`) và nhánh sửa này trước khi build. Ghi commit vào Bảng 0.
- **D2 — bỏ qua nạp cấu hình không đổi (P1.3).** Vòng nạp mỗi 10 s hiện cũng là vòng "đối soát"
  (bật lại `setSyncMode(true)`, áp lại option). Khuyến nghị: bỏ qua khi JSON không đổi nhưng vẫn áp
  lại toàn bộ định kỳ (`hook.configReapplySec`, mặc định 300 s).

## 2. Các phase

Làm trên nhánh `fix/capacity-service`. Tách commit theo phase khi có thể; P2–P4 dùng chung
MP4Recorder/config/regression có thể commit cùng nhau sau validation. Build `build/` + unit liên
quan; phase có chạy thật ghi kết quả vào commit message.

### P1 — Nạp cấu hình không chạy trên poller (K2, sửa A)

1. `server/WebHook.cpp` (`reportServerStatistic`): trong callback của `do_http_hook(server_load)` chỉ
   giải mã JSON rồi đẩy `loadServerConfigJson(config)` sang một `SerialTaskQueue` trên
   `BlockingExecutors::api()` (mẫu có ở `GlobalMonitor::_metrics_queue`). Giữ tối đa một lần
   đang chạy và một cấu hình mới nhất đang chờ; thay thế bản chờ cũ, không xếp chồng vô hạn.
   `pending()` không đếm task đang chạy nên không dùng nó làm cờ single-flight. Xử lý cả
   qualified/not-qualified trên cùng queue để không áp lại cấu hình sau khi decommission.
   Sửa comment sai về `WorkThreadPool`/`getCurrentPoller`.
2. Rà các chỗ khác dựa trên cùng giả định "chạy từ WorkThread thì HttpRequester dùng work poller"
   (`rg "isolated EventPoller|uncontested poller"`): callback nào làm việc nặng thì xử lý như trên.
3. Bỏ qua nạp khi không đổi (theo D2): so sánh JSON phần `devices`, `speakers`, `media_server`,
   `list_media_server`, `watermark_configs`, **`list_audio_file`** của lần áp dụng thành công gần
   nhất (so giá trị, tránh hash collision). Không đổi và chưa quá `hook.configReapplySec` thì
   chỉ log Trace. `loadServerConfigJson` cần trả kết quả: manager chưa ready hoặc lỗi không được
   ghi nhận là thành công; retry ở response tiếp theo. Không cần đợi mọi callback owner poller
   hoàn tất, nhưng phải dispatch thành công toàn bộ cấu hình. Khoá mới
   đăng ký bằng `onceToken` trong config, mô tả trong `conf/config.ini`.
4. Đo: log `Server configuration for camera loaded completed, took N ms` kèm tên thread (đã có).

Kiểm tra:
- Regression: task chạy trên worker, poller vẫn đáp ứng khi apply bị chặn; bản mới thay bản chờ
  cũ; exception/manager chưa ready được retry; audio-only update không bị bỏ qua; reapply định kỳ.
- Chạy thật trên máy dev bằng `tests/integration/stress_vms` (120 camera, mock API): mọi dòng
  `loadServerConfigJson` chạy trên `api worker`, không còn trên `event poller`; không có
  `receive rtp timeout`; sửa camera qua mock API vẫn được áp trong ≤ 10 s; cấu hình không đổi thì
  chỉ áp lại mỗi `configReapplySec`.

### P2 — Đóng segment theo kịp ở 300+ camera (K1 phần còn thiếu, sửa C)

1. Đo trước khi sửa: trong `MP4Recorder::asyncClose` (pool `rec close`) đo thời gian ghi moov/rename
   và thời gian các listener `kBroadcastRecordMP4`; log Warn khi > 1 s; thêm getter tĩnh số việc đang
   chờ của các pool (đếm `atomic` tăng khi post, giảm khi xong).
2. Công bố counters qua stats API hiện có: queued/running/completed, thời gian chờ và tuổi task
   cũ nhất **theo shard**. Đo ở executor để bao gồm cả event recording dùng `closeExecutorFor`.
   Dùng RAII để counter không kẹt khi return sớm hoặc exception. Checkout này có `GlobalMonitor`,
   không có `HealthMonitor` hay target `unit_health_monitor`; không tạo subsystem health mới.
3. Chỉ đổi mặc định `record.closeThreads` sau khi đo trên tải/đĩa đại diện (so 2/4/8). Giữ 2 trong
   bước instrumentation nếu chưa có bằng chứng. `registerHotSegmentRange` có mutex chung và SQLite
   chỉ có một writer, nên thêm thread không bảo đảm tăng thông lượng. Với 600 segment/phút cần
   xử lý bền vững >10 segment/s, queue age bị chặn và backlog không tăng qua nhiều chu kỳ.
   Thứ tự theo stream vẫn giữ vì chọn pool theo hash stream.
4. Nếu phần SQLite (`registerHotSegmentRange`, timefile) chiếm phần lớn thời gian: ghi nhận phương án
   gom nhiều range vào một transaction `beginWriteTxn()`; chỉ làm khi số đo cho thấy cần.

Kiểm tra: stress_vms 120 camera trên máy dev, segment 20 s để tăng tần suất: backlog về 0 sau mỗi
đợt, mọi `Record mp4 file` chạy trên `rec close N`; `unit_storage`, `unit.replay_all` đạt.
Thêm regression cho counters khi task đang chạy/chờ/throw và giữ thứ tự cùng stream.

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
3. Rà chỗ giả định segment đầu đủ `mp4_max_second` (timeline, retention). Guard `max_second < 2`
   để tránh modulo 0; giữ điều kiện cắt tại keyframe. Rải nửa chu kỳ giảm peak, không bảo đảm phân
   bố đều cả phút và không tăng thông lượng ghi bền vững.

Kiểm tra: `unit_storage`, `unit_timeline_query`; stress 120 camera: phân bố giây đóng segment đều
trên ít nhất nửa chu kỳ, có sai số GOP (đếm từ log `Record mp4 file`).

### P5 — Sửa harness (K6)

`tests/integration/stress_vms`:
1. `check_ccu.py`: đếm `receive rtp timeout` từ lúc khởi động node, bao gồm cả ramp; timeout RTP
   của phiên ingest đã PLAY thành công là lỗi ngay cả trước cuối ramp. Báo riêng cột "rớt ngoài
   cửa sổ" và đưa vào verdict. Kiểm tra khoảng hở từ đầu ramp; dùng duration media từ log segment
   đã đóng khi có, thay vì chỉ mtime (close chậm có thể che mất hở); giữ kiểm tra stale file.
   Có regression với log ramp timeout và segment close trễ.
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
4. Ghi kết quả G-r2 vào mục 10.6 của báo cáo, so với lượt sơ bộ; mục 10.5 dành cho kiểm chứng local.
5. Giữ một lượt baseline ngắn sau P1/P2 và trước P3/P4 để biết hiệu quả từng thay đổi. Ở mức mục
   tiêu, chạy soak ít nhất một giờ sau khi SSD đã vào vùng ghi bền vững; ghi queue age/backlog,
   TTFF/stall và rớt ingest. Test dev 120 camera không thay thế nghiệm thu 500–800 camera trên db3.

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

## 4. Quyết định triển khai sau review (2026-10-17)

- Bản plan và kết quả G ban đầu được lưu ở commit `6da9f076` (không gồm tarball).
- Triển khai tiếp trên nhánh hiện có `fix/capacity-service`; source đã có executor `rec close`.
  Binary và bundle lần chạy tiếp phải dựng từ cùng cây source/commit. Không merge nhánh khác tự động.
- Thứ tự: P5 → P1 → P2 instrumentation → baseline → quyết định P3/P4 → P7.
  P3/P4 có thể hoàn thiện và kiểm thử local, nhưng phải đo riêng baseline trước khi chốt hiệu quả.
  Baseline cùng binary đặt record.replayPollerThreads=0;record.segmentStartStagger=0;
  bật từng cờ ở lượt tiếp theo. Replay mặc định 8, stagger mặc định 1; không tự pin thêm CPU.
- Các file chính: harness `check_ccu.py`, `viewer_load.py`, `ccu_runner.sh`; server `WebHook.cpp`,
  `Manager.cpp/.h`; core `MP4Recorder`, `MP4Reader`, `MergedMP4Reader`, `Common/config`; stats API
  hiện có; regression tương ứng trong `tests/unit` và Python unittest của harness.
- Giữ C++11, owner-poller dispatch và API response envelope. Không đổi schema/REST contract hiện có;
  stats chỉ thêm trường. Config mới đăng ký bằng `onceToken`, ghi rõ đọc lúc khởi tạo hay reload.
- Build production: `cmake --build build -j2`. Unit dùng `build-test/` theo `tests/unit/README.md`.
  Chạy focused test trước, sau đó storage/replay/timeline và smoke 20/50 nếu môi trường hỗ trợ.
- P6 và P7 cần máy db3, endpoint/thư mục test được xác nhận; không tự chạy fio, đổi sysctl hoặc
  liên hệ admin từ workspace này. Ghi riêng phần đã kiểm chứng local và phần còn chờ db3.
- Giới hạn NIC/SSD là ước lượng từ lượt G cũ; không coi sửa thread là tăng trần phần cứng.

## 5. Nhật ký thực hiện

- 2026-10-17: lưu baseline tài liệu và bổ sung review; chưa có số đo performance sau sửa.
- P5: commit 4209b6c6, 5 regression Python đạt và bash -n đạt. Đã bổ sung lấy mẫu close shards.
- P1: d04e3b30, ec5ce710 và 88231d20; apply/catch-up/decommission tuần tự trên worker, retry khi
  chưa ready/thất bại, cache có audio, drain khi shutdown và loại response cũ kể cả startup.
- P2–P4: 6f7c6cea, có counters theo shard, poller replay riêng và stagger segment đầu (cờ A/B).
  record.closeThreads giữ 2; batching SQLite/prefetch chưa có bằng chứng cần thực hiện.
- Validation: MediaServer Debug build đạt; 140 test C++ trong bốn bộ replay/storage/timeline/config
  loader và 5 regression Python đạt. Smoke 20/50/120 camera light đạt, không timeout/hở/stale;
  queue cuối cửa sổ về 0 và thread đúng. Chi tiết ở báo cáo mục 10.5.
- Replay fMP4 local: 20 camera, 16 live + 4 replay, 4/4 replay đạt (xem 20 s, lùi 30–50 s),
  TTFF max 545 ms, không khựng/timeout/hở; OwnerPoller chuyển sang replay poller.
- Binary/bundle cho lượt đo phải có metadata Git khớp. Khi configure trong sandbox không đọc
  được refs, build vẫn thành công nhưng metadata trống: phải configure/build lại có quyền đọc Git.
  Các binary smoke cũ được nhận dạng bằng SHA256 ở báo cáo, không dùng để chốt G-r2.
- Lệnh validation đã dùng: cmake --build build-test --target unit_server_config_loader unit_replay
  unit_storage unit_timeline_query MediaServer -j2; CTest chạy tuần tự bốn test _all tương ứng.
  Bundle dựng bằng bash tests/integration/stress_vms/make_ccu_bundle.sh release/ccu_test.sh sau
  commit validation để phần BUNDLE_INFO chỉ đúng source được giao.
- P6/P7 và so A/B/soak trên tải bitrate thật cần db3; chưa kết luận tăng capacity.
- Review 2026-10-17 sau 82302316: clip sự kiện không rải segment đầu; replay poller về priority
  LOWEST như work pool (SCHED_FIFO HIGHEST ngang poller ingest, A/B lẫn hai biến); base64/AES/parse
  response server_load chuyển hẳn sang pool api; loader so sánh tại chỗ, chỉ copy khi apply thành công.
  unit_server_config_loader 6/6, unit_replay 37/37 đạt. Bundle cho G-r2 phải dựng sau commit này.
