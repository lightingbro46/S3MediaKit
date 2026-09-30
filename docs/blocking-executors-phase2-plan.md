# Plan chuyển việc blocking còn lại ra khỏi poller (đợt 2)

Lập 2026-09-29 trên `develop` `458fa9cd`, sau khi rà toàn bộ chỗ còn dùng `WorkThreadPool` /
poller cho việc blocking. Đợt 1 (FFmpegSource, restore, tier, `rec close`, `replay open`) đã xong;
xem [threading-blocking-work.md](threading-blocking-work.md).

Phạm vi: các mục mức **cao** và **trung bình**. Mục mức thấp liệt kê ở cuối để tham khảo.

## Quy trình

- Nhánh `fix/blocking-executors-2` từ `develop`, mỗi phase một commit (tiếng Việt).
- Mỗi phase: `cmake --build build-test -j2`, `ctest --test-dir build-test --output-on-failure -R unit.`,
  cập nhật `docs/threading-blocking-work.md` (bảng executor ở §1 và §6 nếu có hạn chế còn lại).
- Thứ tự: **P3 → P2 → P4 → P1 → P5** (nhỏ, độc lập trước; P1 rủi ro cao nhất; P5 phải tái cấu trúc).

## Cần chốt trước khi làm

| # | Câu hỏi | Đề xuất | Phương án khác |
|---|---|---|---|
| D1 | Pool cho gap-fill (P1) | Pool mới `BlockingExecutors::gapFill()`, `blocking.gapFillThreads` mặc định 2, thread `gapfill N` | `discovery()`: diff nhỏ hơn nhưng quét dải IP và gap-fill chặn lẫn nhau. **Không** dùng `device()`: sau khi mất mạng mọi camera cùng reconnect và cùng tìm kiếm thẻ nhớ, chiếm hết pool làm PTZ kẹt (xem lỗi V4 `68827c1e`) |
| D2 | Pool cho ESC (P4) và HddMonitor (P2) | `api()`, không thêm pool (cùng mẫu `VideoConfigJobManager`) | Một pool nhỏ riêng nếu không muốn việc nền dùng chung với REST |

## P1 — Gap-fill SD card (mức cao)

**Hiện trạng**

- `fillDevice` chạy trên `WorkThreadPool::getExecutor()` ở 4 chỗ:
  `MediaGapFillManager.cpp` dòng 476 (`onManager`), 923 (`resetFailedSession`), 972
  (`resetFailedSegment`), 1068 (`onDeviceStateChanged`). Bên trong: `provider.discover(gap)` gọi SOAP
  ONVIF `findVideoSegments` + `getRecordingInformation` tới camera (callback đăng ký ở dòng ~373),
  cộng nhiều truy vấn SQLite (`LocalResourceStatusGapImp`, `MediaGapFillSessionImpl`,
  `MediaGapFillTaskImpl`). Camera chậm giữ poller đang chạy timer của `SyncManager`, ESC…
- `_progress_timer` 5 s (constructor, dòng ~230) và `_retry_timer` (`armRetryTimer`, dòng ~1188)
  chạy trên **`EventPollerPool`** (poller mạng). Cả hai gọi `checkpointProgress()` / `schedule()`,
  mà hai hàm này gọi `persistTask` (SQLite: `findById` + `addOrUpdate`) trong lúc giữ `_mtx`.
- `onConnectionClosed` được gọi từ `player->setOnClose` (poller của player), ghi `persistTask` rồi
  `schedule()`.

**Sửa**

1. Thêm pool theo D1 vào `manager/Common/BlockingExecutors.{h,cpp}` (khai báo, `*ThreadCount()`,
   key config `Blocking::kGapFillThreads` đăng ký bằng `onceToken`), cập nhật
   `test_blocking_executors.cpp`.
2. Đổi 4 chỗ gọi `fillDevice` sang pool đó. `claimDiscovery` đã chặn hai lần discover cùng session,
   nên các device chạy song song là an toàn.
3. Thêm `SerialTaskQueue::Ptr _state_queue` trên cùng pool (tên `"gapfill state"`):
   - `_progress_timer`: chỉ `post` `checkpointProgress(); schedule();`, bỏ lượt nếu
     `_state_queue->pending() > 0` để không dồn việc.
   - `_retry_timer`: chỉ `post` `schedule()`.
   - Callback `closed` trong `schedule()`: `post` `onConnectionClosed(...)` thay vì gọi trực tiếp.
   - Singleton nên capture `this` được (ngoại lệ trong CLAUDE.md), nhưng giữ đúng mẫu đang có.
4. Giữ nguyên `_mtx` và logic; chỉ đổi nơi chạy.
5. **Kiểm lúc làm:** `provider.start` (`makeRtspPlaybackProvider`, tạo `PlayerProxy` qua factory
   callback rồi `player->play`) giờ được gọi từ thread pool — xác nhận nó không đòi chạy trên poller;
   nếu có thì nhảy sang poller của player trước khi `play`.
6. Các API đọc (`makeRemainingSegmentsJson`, `makeDisconnectSession*Json`) đã đi qua
   `ApiWorkDispatcher` — kiểm lại, không đổi.

**Test**

- Mới: `tests/unit/manager/test_media_gap_fill_threading.cpp`. Đăng ký source có `discover` bị chặn
  (chờ một flag) và ghi tên thread; kích `onDeviceStateChanged` (hoặc `resetFailedSession`) với một gap
  hợp lệ trong DB test. Kỳ vọng: `discover` chạy trên thread `gapfill`; trong lúc bị chặn, một task đẩy
  vào `WorkThreadPool` và `EventPollerPool` vẫn chạy ngay. Mutation check: trả `fillDevice` về
  `WorkThreadPool` thì test fail.
- Nhớ thêm `add_test unit.<x>_all` nếu target link ext-codec (discovery gtest có thể rỗng im lặng).

## P2 — `HddMonitor` refresh (mức trung bình)

**Hiện trạng:** `HddMonitor::dispatchRefresh()` (`manager/Server/HddMonitor.cpp` dòng ~227) chạy
`refreshNow()` trên `WorkThreadPool::getPoller()`. `refreshNow` gọi `statvfs` cho từng mount; một mount
NFS/NAS treo làm thread kẹt ở trạng thái D vô thời hạn.

**Sửa:** đổi sang pool theo D2 (`BlockingExecutors::api()->async`). Cờ `_refresh_running` đã giới
hạn một lần refresh mỗi lúc, nên mount treo chỉ giữ nhiều nhất một thread. Hai chỗ refresh đồng bộ
`findMountPointInfo(..., true)` trong `TierFileStorageBase.cpp` (dòng 144, 379) chạy trên `tier()`,
giữ nguyên.

**Test:** thêm case vào `test_resource_monitor.cpp`: provider bị chặn, gọi đường timer, poller vẫn
chạy task khác ngay; mở chặn thì snapshot được cập nhật.

## P3 — `startRecord` có thời lượng (mức trung bình)

**Hiện trạng:** `MultiMediaSourceMuxer::startRecord` (`src/Common/MultiMediaSourceMuxer.cpp` dòng
~481) đóng `MP4Muxer` bằng `WorkThreadPool::Instance().getPoller()->async([muxer]{ muxer->closeMP4(); })`
— ghi moov, file càng dài càng lâu. Người gọi: `/index/api/startRecord` (`WebApi.cpp:1759`) và C API
`mk_recorder.cpp:88`.

**Sửa:** dùng `MP4Recorder::closeExecutorFor(<stream key>)` (lấy `_tuple.shortUrl()` ra biến trước
khi capture), cùng pool `rec close N` với đường ghi chính, giữ thứ tự theo stream. Dòng 612/623 chỉ
`reset()` một `MP4Recorder` (tự đóng file trên `rec close`) — giữ nguyên.

**Kiểm:** build, `unit.`; gọi tay `/index/api/startRecord?back_ms=…&forward_ms=…` trên luồng live,
`ffprobe` file ra hợp lệ.

## P4 — Ghi ESC của từng camera (mức trung bình)

**Hiện trạng:** `CameraStatisticImp` giữ `_esc_poller = WorkThreadPool::getPoller()`
(`manager/Camera/CameraStatistic.cpp` dòng 846) và `postEscTask` (dòng 1570) đẩy việc ghi SQLite
lên poller đó để giữ thứ tự theo camera. DB bị khoá thì lệnh chờ busy_timeout block poller, trễ luôn
`SyncManager` và timer khác trên cùng thread.

**Sửa:** thay `toolkit::EventPoller::Ptr _esc_poller` (`CameraStatistic.h:211`) bằng
`SerialTaskQueue::Ptr _esc_queue` trên pool theo D2, tên `"esc " + <device>`; `postEscTask` gọi
`_esc_queue->post(task)`. Thứ tự theo camera vẫn giữ; các camera khác nhau chạy song song. Các task
hiện capture `weak_self` hoặc giá trị — không đổi.

**Test:** `test_camera_statistic_sync.cpp` chờ bằng cách poll DB (`waitFor`, `waitEscIdle`), không
phụ thuộc executor — chạy lại là đủ. Tài liệu: bỏ "ghi ESC của từng camera" khỏi dòng
`WorkThreadPool` ở §1.

## P5 — Resolve overlay trên poller HttpSession (mức trung bình–thấp)

**Hiện trạng:** trong `src/Http/HttpSession.cpp` (dòng ~484–499), trên poller của session:
- `OverlayPrivacyUtils::resolveLocalImages` → `resizeImageToPng` gọi `std::system("ffmpeg …")` khi
  cache ảnh trượt (`OverlayPrivacyUtils.cpp:244`).
- `resolveWatermarkAsset` đọc SVG và ghi file `.resolved.*.svg` ở mỗi request.

**Sửa:** `src/` (mediakit) không được phụ thuộc `BlockingExecutors` (managerkit), nên:
1. Thêm `OverlayPrivacyUtils::assetExecutor()`: `ThreadPool` tĩnh 2 thread, tên `overlay asset`,
   cố ý leak như pool `replay open` (`MediaSource.cpp:585`).
2. Trong `HttpSession`, chạy phần resolve (watermark asset / parse + local images) trên pool đó, rồi
   `self->async(..., false)` về poller session; phần tạo `variant_kind`, `stream_suffix` và
   `start_transcode` chuyển vào continuation. Lỗi resolve vẫn trả `403` như cũ.
3. `FFmpegSource.cpp:1215` (overlay cho extract/thumbnail) — kiểm lại là đã chạy trên `api()` sau
   `fcd18a7d`; nếu đúng thì không sửa.

**Kiểm:** E2E camera giả (`tests/integration/e2e_real`) phần overlay OVL-01..09, HLS có overlay,
thumbnail có overlay. Xoá cache `www/overlay/` trước để đi qua nhánh ffmpeg resize.

## Ngoài phạm vi (mức thấp, tham khảo)

| Vị trí | Việc | Gợi ý |
|---|---|---|
| `src/Backchannel/FileAudioSource.cpp:62,82` | Timer 20 ms trên poller `WorkThreadPool` `fread` blocking pipe ffmpeg | `O_NONBLOCK` cho pipe hoặc thread đọc riêng |
| `src/Motion/MotionProcessor.cpp:114,191` | Vẽ + encode JPEG + ghi file trên `WorkThreadPool` | `ffmpeg()`/`storage()` hoặc pool trong `src/` |
| `CameraManager.cpp:240`, `SpeakerManager.cpp:116` | Nạp camera/speaker từ SQLite lúc khởi động | Chạy một lần, sửa khi tiện |
| `TimeRecorder.cpp:23` | `closeFile` trên `WorkThreadPool` | Ngắn |
| `FFmpegSource.cpp:2039` | Audit log extract (listener ghi SQLite) trên `WorkThreadPool` | Ngắn |
| `FFmpegSource.cpp:2170` | `FFmpegProbe::makeProbe` mặc định `WorkThreadPool` (chưa có caller dùng mặc định) | Đổi mặc định sang `ffmpeg()` |
| `SyncManager` apply trên `WorkThreadPool` | SQLite trên poller, cố ý để giữ thứ tự | Có thể dùng cùng cách P4 sau này |

Giữ nguyên có chủ đích (§6 của threading doc): `getSnap async=0`, `find(from_mp4)` đồng bộ,
`openMP4` khi chuyển batch replay per-stream, `SubnetScan::scanRange` đồng bộ, vòng chờ restore trong
mã hoá lại (đã ở `encryption()`), DNS của `RtpSender` (code repo gốc).

Đã kiểm, không có vấn đề: handler REST đều qua `ApiWorkDispatcher`; health check `ClusterManager` là
HTTP async; listener `MediaPlayed`/`ViewOverlay` chỉ đọc bộ nhớ; `MP4Recorder` đóng file trên `rec close`.
