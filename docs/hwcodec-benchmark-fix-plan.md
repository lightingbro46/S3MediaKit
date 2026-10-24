# Plan sửa lỗi phát hiện khi đo GPU/CPU (2026-10-24)

Nguồn: [hwcodec-benchmark-2026-10-24.md](hwcodec-benchmark-2026-10-24.md) mục 6–7 và các bẫy của harness
`tests/performance/hwcodec_bench`. Plan xếp theo mức độ nghiêm trọng, mỗi mục có nguyên nhân (đã đọc code),
cách sửa, test và tiêu chí đạt. Giữ diff nhỏ nhất, không thêm API hay file chỉ để test.

| # | Lỗi | Mức độ | Hướng xử lý |
|---|---|---|---|
| P1 | `FFmpegDecoder` bị huỷ trên chính decoder thread, `join()` chính nó → `std::terminate` | Crash server | Sửa |
| P2 | Viewer bắt buộc overlay có thể nhận luồng gốc trong lúc server vừa lên | Có thể lộ hình (privacy mask/watermark) | Xác minh, rồi sửa |
| P3 | Luồng camera khởi động lại bị kẹt khoảng 10 s, lặp lại vì nguồn HLS cũ còn đăng ký | Mất hình khoảng 10–12 s mỗi lần restart | Sửa |
| P4 | Giải mã GPU trong tiến trình (WSL/Mesa d3d12) segfault trong driver làm sập server, probe không bắt được | Crash khi bật GPU | Probe kỹ hơn + docs |
| P5 | RSS chế độ GPU rất lớn (5,5 GB với 4 luồng 1080p), motion 1080p CPU khoảng 190 MB/luồng | RAM | Điều tra rồi quyết định |
| P6 | Tier transcode HEVC → H.265: L1 lỗi 3/6 lần, chạy L2 | Hiệu năng | Điều tra |
| P7 | Harness và công cụ đo | Test | Commit harness, ghi giới hạn |
| — | `record/<cam>/info.txt` nạp cấu hình cũ trước cấu hình API | Đúng thiết kế | Không sửa |

## P1 — Decoder bị huỷ trên chính thread của nó (ưu tiên cao nhất)

**Hiện trạng.** `TaskManager::startThread` (src/Codec/Transcode.cpp:150) cài deleter cho thread. Deleter gọi
`join()`. Nếu tham chiếu cuối tới `FFmpegDecoder` mất ngay trên decoder thread, `join()` ném `std::system_error`
(EDEADLK) bên trong destructor, rồi `std::terminate`. Kể cả khi không `join`, stack vẫn quay lại
`FFmpegDecoder::decodeFrame` của một object đã huỷ (use-after-free). Vậy cách sửa duy nhất đúng là **không để
decoder bị huỷ trên chính thread của nó**.

Stack đã gặp (lượt CPU, motion Main 16 camera): callback ở src/Processor/MultiMediaSourceProcessor.cpp:294
`weak_self.lock()` → `onDecode()` → hết callback, `self` là tham chiếu cuối → `~MultiMediaSourceProcessor` →
`close()` → `MediaSourceDecoder::resetTracks()` (src/Processor/MediaSourceDecoder.cpp:86) → `~FFmpegDecoder` →
`stopThread` → `join` chính nó.

Những chỗ khác có cùng mẫu (decoder do một object giữ, và object đó có thể bị nhả bên trong callback giải mã):

| Chỗ | Cách nhả trên decoder thread |
|---|---|
| `MultiMediaSourceProcessor` (motion, transcode live) | `self` của callback là tham chiếu cuối (đã gặp) |
| `makeSnapAsync` (server/FFmpegSource.cpp:418–433) | callback `onDecode` gán `new_holder->player = nullptr`. Player giữ track, track giữ delegate, delegate giữ decoder |
| `VideoStack` (server/VideoStack.cpp:231–240) | `weakSelf.lock()` trên decoder thread. Stack giữ player, player giữ decoder qua delegate |
| `MjpegIngestTranscoder` | an toàn: `Impl::reset()` gọi `stopThread` từ thread khác trước khi huỷ |
| C API `mk_decoder_*` | người gọi tự quản lý vòng đời, ngoài phạm vi |

**Cách sửa.**

1. Thêm `bool TaskManager::isCurrentThread() const` (so `_thread->get_id()` với `this_thread::get_id()`), public
   trong `FFmpegDecoder`.
2. `MediaSourceDecoder::resetTracks()`: nếu có decoder nào `isCurrentThread()`, chuyển `_tracks` sang một holder rồi
   nhả holder trên poller khác (`EventPollerPool::Instance().getPoller()->async([holder] {})`). Còn lại giữ nguyên
   `clear()`. Như vậy decoder còn sống cho tới khi stack quay ra khỏi `decodeFrame`, và decoder thread bị `join` từ
   poller. Sửa ở đây là che được cho mọi lớp con của `MediaSourceDecoder`, không cần đoán tham chiếu cuối (không có
   race).
3. `makeSnapAsync`: trong callback, `std::move` player ra khỏi `new_holder` rồi nhả trên poller của player thay vì
   gán `nullptr` tại chỗ.
4. `VideoStack`: nhả player (đang giữ decoder) trên poller khi stack bị huỷ trên decoder thread. Đọc
   `~VideoStack`/`StackPlayer` trước để chọn chỗ nhả, theo cùng mẫu ở bước 2.
5. Deleter của `TaskManager`: nếu đang ở chính thread của nó thì ghi `ErrorL` kèm tên thread trước khi kết thúc
   (vẫn `terminate`), để lần sau log nói thẳng nguyên nhân thay vì chỉ có `system_error`.

**Test.**
- Unit `unit_codec` (tests/unit/codec/test_hw_codec.cpp, dùng helper `encodeThenDecode` có sẵn): một holder
  `MediaSourceDecoder` có `onDecode` nhả tham chiếu cuối tới chính holder trên decoder thread. Trước khi sửa, test
  chết vì `terminate`; sau khi sửa phải chạy hết, decoder thread được join từ poller.
- Chạy lại `hwbench.py live --mode cpu` với motion Main 16 camera, 3 lần, không có `## signal`.
- `getSnap` lặp 200 lần (CPU) không crash. VideoStack: chạy mở/đóng stack 50 lần nếu có test sẵn, không thì chạy tay
  qua API.

**Đạt:** unit mới qua và `ctest -R unit.` không hồi quy; 3 lượt live CPU 16 camera không crash.

**Kết quả (2026-10-24, nhánh `fix/decoder-self-join`).** Khác bản dự kiến ở bước 2: thay vì hoãn huỷ decoder trong
`resetTracks()`, `MultiMediaSourceProcessor::create()` (3 chỗ ở MultiMediaSourceMuxer.cpp) gắn deleter. Nếu tham chiếu
cuối mất trên decoder thread, cả processor được huỷ trên `_poller` của nó. Làm vậy vì huỷ processor trên decoder thread
còn huỷ MotionProcessor/MJPEG muxer ngoài poller. Comment cũ trong `close()` nhắc tới một `create()` không tồn tại.
- Unit `ProcessorReleasedWhileDecodingIsNotDestroyedOnTheDecoderThread` tái hiện tất định: một `LogChannel` của test
  giữ decoder thread ở `TraceL` trong `onDecode` rồi thread test nhả processor. Với `make_shared` (bản cũ) thì
  `terminate: Resource deadlock avoided`; với `create()` thì đạt. Thêm `DecodeCallbackSeesItsDecoderThread`.
- `ctest -R ^unit.`: 362/362 (một lần chạy khác `unit.sync_apply_all` fail; chạy lại 3/3 đạt, không liên quan).
- `getSnap async=1` 200 lần (200 decoder): 200 JPEG, không crash. VideoStack chưa chạy thật.
- 3 lượt live CPU motion Main 16 + transcode overlay 8: 0 crash, trong khi luồng restart 768 lần (P3).

## P2 — Viewer bắt buộc overlay nhận luồng gốc lúc vừa khởi động (xác minh trước)

**Hiện trạng.** Lần chạy thử đầu tiên, viewer role viewer (camera có watermark + privacy mask, role không được miễn)
kéo `/media/live/<cam>/main.live.mp4` khoảng 15 s sau khi đổi cấu hình và **nhận luồng gốc 25 fps trong 40 s**,
không có transcode derived. Lượt chính thức sau đó cũng phải mở lại viewer (lần 1 hết 30 s không thấy encoder), tức
là ở lần mở đầu, viewer không được chuyển sang derived. Nghi do policy overlay được tính lúc camera hay watermark
template (`loadWatermarkTemplateFromJson`, `prefetchWatermarkOverlayImages` ở server/Manager.cpp) chưa nạp xong.
Khi đó `ViewOverlayPolicy` rỗng, nghĩa là "không cần overlay", trong khi lẽ ra phải là "chưa sẵn sàng" (403/503).
**Chưa xác minh**: có thể chỉ là viewer vào trước khi camera kịp có cấu hình overlay.

**Bước làm.**
1. Tái hiện có kiểm soát: camera có overlay sẵn từ lúc khởi động (không đổi giữa chừng), viewer kết nối ở T+3, +6, …,
   +45 s sau khi server lên. Mỗi lần ghi: HTTP status, có ra derived hay không (`getMediaList`), và một frame
   (watermark có hay không), như `e2e_check.py g_overlay`.
2. Kiểm thêm khi đổi overlay nóng (bật overlay cho camera đang có viewer): viewer cũ có bị cắt hoặc chuyển không?
3. Nếu lộ: đường tính policy (`NOTICE_EMIT(BroadcastMediaViewOverlay…)` → listener trong server/WebHook.cpp hoặc
   Manager.cpp) phải trả `isUnavailable()` khi camera có watermark/privacy đã cấu hình nhưng ảnh overlay hoặc template
   chưa sẵn, và khi camera chưa nạp cấu hình mà nguồn đã online. Như vậy HTTP trả 403, HLS trả 403 như hiện có, client
   thử lại. Không đổi hành vi cho camera không có overlay.

**Test:** unit cho hàm tính policy (tests/unit/record/test_view_overlay_policy.cpp có sẵn) với trạng thái "template
chưa nạp"; E2E `check overlay` vẫn đạt; tái hiện ở bước 1 không còn frame không watermark.

**Đạt:** không có lần nào viewer bắt buộc overlay nhận được luồng gốc.

**Kết quả xác minh (2026-10-24).**
- Bước 1, lúc khởi động: **không lộ**. Có 16 camera bật watermark + privacy mask từ đầu; viewer role viewer kéo
  `main.live.mp4` của camera i ở T+3·i s (T+3 tới T+48 s). Ở T+3 s nhận 404 (luồng chưa có trong thời gian chờ). Từ
  T+6 s, server giữ request cho tới khi derived sẵn sàng rồi phát 1080p 5 fps có overlay (luồng gốc là 25 fps). Không
  lần nào nhận luồng gốc. Hiện tượng ở lượt đầu nhiều khả năng do cấu hình cũ nạp trước (lúc đó harness chưa xoá
  `db/` và `info.txt`). Không sửa.
- Phát hiện phụ: ở lần khởi động **đầu tiên** của một node (config.ini chưa có `manager.mediaServerProjectId`),
  request tới trước khi lấy được `projectId` từ API sẽ cache token là "No permission to access project". Cache này
  giữ tới khi JWT hết hạn (UserSessionCache.cpp:164–168), nên mọi request sau đó với token này đều nhận 401, dù server
  đã có `projectId`. Từ lần khởi động thứ hai trở đi không gặp, vì `projectId` đã được lưu vào config.ini.
- Bước 2, đổi nóng (theo đọc code, chưa chạy): bật watermark/privacy mask cho camera đang chạy **không** ngắt và
  không chuyển viewer đang xem luồng gốc. `StreamOption::operator==` không gồm trường overlay nên luồng không dựng
  lại, và không có chỗ nào đánh giá lại policy của session đang mở. Viewer cũ thấy hình không mask cho tới khi tự
  kết nối lại. Cần chốt cách xử lý (ngắt session nào, có miễn role không) trước khi sửa.

## P3 — HLS cũ còn đăng ký làm luồng khởi động lại thất bại lặp

**Hiện trạng.** Commit 10990836 (src/Record/HlsMakerImp.cpp:78–90) giữ `HlsMediaSource` live đã eof trong
`hls.deleteDelaySec` (mặc định 10 s), để client tải nốt playlist có `EXT-X-ENDLIST`. Khi `StreamSource` khởi động lại
luồng (đổi option camera, mất kết nối), muxer mới đăng ký HLS ở segment đầu tiên, khoảng 3–4 s sau. Lúc đó
`MediaSource::regist()` (src/Common/MediaSource.cpp:536–550) thấy nguồn cũ còn sống nên ném `invalid_argument`. Exception
lan lên `RtspPlayer::onPlayResult_l` làm cả lần play thất bại, `PlayerProxy` thử lại sau 2 s, và lặp tới khi nguồn cũ
hết 10 s. Đo được: mỗi lần restart mất hình khoảng 10–12 s và 2–4 vòng play hỏng mỗi luồng. Mỗi vòng tạo rồi huỷ
lại decoder motion/transcode (làm P1 và P4 dễ xảy ra hơn).

**Cách sửa** (giữ ý định của 10990836: client đang đọc playlist cuối vẫn đọc được):
1. Đánh dấu nguồn HLS đã kết thúc khi `clearCache(eof=true)` giữ nó lại (cờ trong `HlsMediaSource`, đặt trước
   `doDelayTask`).
2. `MediaSource::regist()`: khi khoá đã có nguồn khác, nếu nguồn cũ là HLS đã kết thúc thì **nhường chỗ**: ghi đè
   map bằng nguồn mới và log Info. Nguồn cũ vẫn sống trong delay task, và session đang giữ con trỏ tới nó vẫn đọc tiếp
   được. Lúc nguồn cũ huỷ, `erase_media_source` chỉ xoá đúng con trỏ của nó, nên không đụng tới nguồn mới (kiểm lại
   khi sửa).
3. Không đổi trường hợp hai nguồn live thật sự trùng tên (vẫn ném).

**Test.** Unit trong tests/unit/record/test_hls_media_source.cpp: nguồn A đăng ký rồi đánh dấu kết thúc, nguồn B cùng
khoá đăng ký thành công, `find` trả B; A huỷ thì B vẫn còn; hai nguồn live trùng thì vẫn ném. Chạy lại harness: cột
`restart_churn` về 0 (chỉ còn 1 lần restart) và hình có lại trong ≤ 5 s sau restart.

**Đạt:** không còn `media source already existed: hls` khi luồng khởi động lại.

**Kết quả (2026-10-24, nhánh `fix/decoder-self-join`).** Làm đúng như dự kiến. Cờ được khai báo là
`virtual bool MediaSource::isFinished()` (mặc định false) và được `HlsMediaSource` override. Khi thay chỗ, `regist()`
phát `Media Cancellation` cho nguồn cũ trước `Media Registration` của nguồn mới, để listener thấy các sự kiện theo đúng
thứ tự. Nguồn cũ huỷ sau 10 s thì `unregist()` không tìm thấy chính nó trong map nên không phát sự kiện nào nữa.
- Unit `RestartedStreamReplacesFinishedSource`, `DuplicateLiveSourceIsRejected` (unit_hls 23/23).
- Live CPU motion Sub/Main 1 và 16 camera: `restart_churn` ở mọi bậc là 0 (lần đo trước 4–63). Bậc motion Main 16
  có 60 lần `Replace finished media source`; mỗi luồng chỉ restart 1 lần, HLS đăng ký lại khoảng 3,5 s sau RTSP; 0 crash.
- Giới hạn: delay task của nguồn cũ vẫn xoá file `hls.m3u8` trên đĩa sau 10 s, trong khi muxer mới đã ghi đè file
  này. Playlist live được phục vụ từ bộ nhớ và được ghi lại ở segment kế tiếp, nên file trên đĩa chỉ vắng tối đa
  một segment.

## P4 — Giải mã GPU trong tiến trình làm sập server

**Hiện trạng.** WSL2 + Mesa d3d12 (Intel): segfault trong `vaEndPicture` ở 18/26 bậc đo. ffmpeg thuần cho thấy driver
chịu được nhiều decoder chung một device, nhưng chết khi có nhiều device, hoặc khi decoder bị tạo/huỷ liên tục (đúng
những gì P3 gây ra). Breaker chỉ xử lý mã lỗi; probe trong tiến trình con chỉ thử 5 frame mỗi codec.

**Cách sửa.**
1. Sau P3, chạy lại `hwbench.py live --mode gpu` để xem còn crash bao nhiêu khi không còn churn. Kết quả quyết định
   các bước sau.
2. Probe giải mã kỹ hơn trong tiến trình con: thêm một phần thử `h264.decode-stress` khoảng 3–5 s (4 decoder chung
   device, mỗi decoder mở/giải mã/đóng vài vòng, 1080p). Phần này crash hoặc treo thì tắt giải mã GPU của backend đó,
   như cơ chế `runHwTestsInChildren` đang làm. Probe chạy trên `BlockingExecutors::ffmpeg()` nên không chặn khởi
   động, chỉ chậm thêm vài giây mới có GPU.
3. docs/hw-codec.md, mục WSL2: ghi kết quả đo, khuyến nghị `hwcodec.decode=0` trên WSL/d3d12, và giữ GPU cho tier
   transcode/extract (chạy ngoài tiến trình).
4. Không làm trong plan này: tách giải mã GPU ra tiến trình con riêng. Đó là thay đổi kiến trúc lớn, chỉ cân nhắc nếu
   máy Linux thật (iHD/NVIDIA) cũng crash.

**Test:** unit `HwChildTestRunner` có sẵn cho phần thử mới (crash/treo chỉ tắt phần đó). Trên WSL: probe phải tự tắt
`h264.decode` nếu stress crash, và `hwbench.py live --mode gpu` không còn `## signal`.

**Đạt:** không crash trên WSL với cấu hình mặc định; máy có GPU ổn định vẫn dùng GPU như trước.

**Kết quả (2026-10-24, nhánh `fix/decoder-self-join`).** Khác bản dự kiến: bỏ bước 2 (probe stress) và thêm khoá
`hwcodec.inProcess`.
- Bước 1, chạy lại `--mode gpu` sau P3 (churn = 0): vẫn crash 6/10 bậc live. Mọi crash đều trong
  `libigd12dxva64.so` (`vaEndPicture`). Có lần crash khi chỉ có 1 decoder 1080p mở, không có tạo/huỷ nào, nên churn
  không phải điều kiện cần. Thêm `hwcodec.decode=0`: mã hoá `h264_vaapi` trong tiến trình cũng crash, cùng offset.
- Đã thử loại từng giả thuyết, đều không ra crash: bitstream bị drop kiểu `addDecodeTask` (bỏ cả SPS/PPS) qua
  ffmpeg CLI; cờ `low_delay`/`fast`; chỉ backend vaapi (không fork probe qsv sau khi mở device); `FFmpegDecoder` thật
  trong unit test với 1–4 decoder theo nhịp 25 fps, consumer chậm 50–80 ms, 1100 fd mở sẵn (tổng cộng khoảng
  4 phút giải mã GPU). Nguyên nhân nằm trong driver và chỉ lộ ra trong ngữ cảnh đầy đủ của MediaServer; chưa tách được.
- Vì vậy probe stress trong tiến trình con (bước 2) sẽ không bắt được lỗi; không làm.
- Cấu hình mặc định trên WSL đã an toàn: không có `LIBVA_DRIVER_NAME=d3d12` thì libva không khởi tạo, server chạy CPU.
- Thêm `hwcodec.inProcess` (mặc định `1`). Đặt `0` thì `acquireDecode`/`acquireEncode` (live trong MediaServer)
  trả CPU, còn `acquire*Offline` (trích xuất overlay, tier transcode, chạy bằng tiến trình `ffmpeg` con) vẫn dùng GPU.
  Unit `InProcessOffLeavesGpuToChildProcessJobs`. docs/hw-codec.md: thêm dòng vào bảng cấu hình và kết quả đo ở
  mục WSL2.
- Harness `--mode gpu --ini hwcodec.inProcess=0`: 0 crash / 11 bậc. Live chạy CPU, extract dùng `vaapi`
  (1 job 30 s, 3 job 75 s).
- Phát hiện phụ (P5): mỗi decoder GPU 1080p trong MediaServer làm RSS tăng khoảng 1,5 GB, nằm trong các arena malloc
  64 MB (anon), không phải vùng map của driver. `FFmpegDecoder` chạy riêng chỉ dùng 272 MB.
- Phát hiện phụ (harness): ở bậc motion-main, khoảng 17 s đầu vẫn chạy motion trên Sub theo cấu hình của bậc trước;
  vẫn còn một nguồn cấu hình cũ ngoài `db/` và `info.txt`.

## P5 — Bộ nhớ của motion và của giải mã GPU

**Hiện trạng.** Motion Main 1080p (CPU): RSS từ 665 MB (1 luồng) lên 3,1 GB (16 luồng), khoảng 165 MB mỗi luồng.
Chế độ GPU: 4 luồng 1080p lên 5,5 GB, so với 0,76 GB ở CPU. Nghi vấn: ring 512 frame decode
(`MultiMediaSourceProcessor`, `RingType(512, …)`), pool surface VAAPI mỗi decoder (iGPU nằm trong RAM chung, GPU-PV
map vào tiến trình), và `docs/hwcodec-gpu-frames-plan.md` đã nêu rủi ro ring 512.

**Bước làm.** Đo RSS theo số luồng ở 3 cấu hình: CPU; GPU; GPU có giới hạn `extra_hw_frames`/kích thước pool. Đếm số
frame thật sự nằm trong ring (thêm vào log debug tạm, không commit). Nếu ring giữ frame quá mức cần cho motion
(`intervalMS=200`), giảm kích thước ring hoặc chỉ giữ frame cần phân tích. Quyết định sau khi có số.

**Đạt:** có số đo và quyết định (sửa hoặc ghi giới hạn vào docs/hw-codec.md).

## P6 — Tier transcode HEVC → H.265, L1 lỗi

**Bước làm.** Chạy lại riêng ca này (`TIER_BENCH_SOURCE=…hevc.mp4 TIER_BENCH_CODEC=H265`, REPS=6), giữ stdout để
lấy dòng `Tier transcode: GPU decode on … failed` / `GPU encoder … failed` (manager/Local/TierMediaTransformer.cpp:511–516)
kèm stderr ffmpeg. Phân loại: lỗi driver WSL (giữ fallback, ghi docs) hay sai tham số lệnh L1 (`scale_vaapi`, format,
`hevc_vaapi` profile). Nếu là sai tham số thì sửa argv L1 và thêm ca vào integration test.

**Đạt:** 6/6 lần ra L1, hoặc nguyên nhân đã ghi vào docs/hw-codec.md mục "Transcode khi chuyển tầng".

## P7 — Harness và công cụ đo

1. Commit `tests/performance/hwcodec_bench/` (harness, README, results-2026-10-24), bench `BenchmarkFromEnv` trong
   tests/integration/tier_storage/test_tier_media_transformer.cpp và báo cáo docs/hwcodec-benchmark-2026-10-24.md.
2. `rtsp_source.py` đánh timestamp RTP theo thứ tự giải mã nên không phát được clip có B-frame. Ghi giới hạn này vào
   đầu file và vào tests/integration/stress_vms/README.md. Không sửa công cụ (cần tách POC để có pts), vì camera thật
   phát đúng pts. Nếu cần kiểm nguồn B-frame thì dùng nguồn RTSP thật hoặc ffmpeg `-f rtsp`.
3. Sau P1/P3/P4, chạy lại toàn bộ harness (CPU và GPU) và cập nhật báo cáo.

## Không sửa: nạp `record/<cam>/info.txt` trước cấu hình API

`StatisticRecorder::loadSavedCameraStatistics` (manager/Local/StatisticRecorder.cpp:149) cho server chạy tiếp camera
ngay khi lên, không cần chờ API. Lúc API trả cấu hình khác thì camera khởi động lại một lần. Đó là đúng thiết kế; tác
hại thật nằm ở P3 (lần restart đó bị kẹt khoảng 10 s). Harness tự xoá `info.txt` để đo sạch.

## Thứ tự và ước lượng

1. P1 (crash, độc lập): khoảng 0,5–1 ngày, gồm unit test và 3 lượt live CPU.
2. P3, rồi chạy lại harness để có số mới cho P4.
3. P2 xác minh (0,5 ngày), sửa nếu lộ.
4. P4 dựa trên số liệu sau P3.
5. P5, P6 điều tra, P7 commit dần theo từng phần.

Mỗi phần một commit (tiếng Việt), build `-j2`, `ctest -R unit.` trên `build-test`; phần nào đụng `src/Record`,
`src/Processor` hay `src/Codec` thì chạy thêm `unit.codec`/`unit.record` và harness live tương ứng.
