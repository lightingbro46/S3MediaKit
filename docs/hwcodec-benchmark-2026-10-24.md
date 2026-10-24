# Báo cáo đo GPU so với CPU (hwcodec), máy dev WSL2, 2026-10-24

Đo chi phí CPU, thông lượng và độ ổn định của từng tính năng dùng GPU ([hw-codec.md](hw-codec.md)): chạy
`hwcodec.enable=0` (CPU) rồi `hwcodec.enable=1` (GPU), cùng nguồn và cùng bậc tải. Thiết kế bài đo và cách chạy
lại nằm ở [tests/performance/hwcodec_bench/README.md](../tests/performance/hwcodec_bench/README.md). Số liệu thô ở
`tests/performance/hwcodec_bench/results-2026-10-24/`.

## 1. Tóm tắt

| Tính năng | GPU tiết kiệm CPU | Thời gian / thông lượng | Ổn định trên máy này | Kết luận cho máy này |
|---|---|---|---|---|
| Transcode khi chuyển tầng (L1: giải mã + scale + mã hoá GPU) | **7–19 lần** | đích H.265: nhanh hơn 1,2–1,5 lần; đích H.264: chậm hơn 1,5–2,7 lần (~9–14 s cho segment 60 s) | đạt 24/24 lần; HEVC→H.265 có 3/6 lần L1 lỗi (driver segfault, mục 10) rồi chạy L2 | **Nên dùng GPU** |
| Trích xuất có overlay (mã hoá GPU, giải mã + overlay CPU) | 20–27 % | 1 job như nhau (35 s); 3 job nhanh hơn 18 % (70 s so với 86 s) | đạt 4/4 job | Nên dùng, lợi ích vừa phải |
| Transcode live overlay (giải mã + mã hoá trong tiến trình) | ~50 % mỗi luồng (rig 2 camera) | đủ 5 fps | **crash** ở 4/6 bậc GPU (16 và 4 camera) | Không dùng được trên WSL |
| Motion trên Main 1080p | ~23 % (rig 4 camera, 4 luồng) | GPU **bỏ frame** (19 lần trong 40 s), RSS 5,5 GB so với 0,76 GB (đã sửa, mục 10) | **crash** ở 8/9 bậc | Không dùng được trên WSL |
| Motion trên Sub 360p | **tốn hơn** CPU (5,4 % so với 3,0 % mỗi camera) | — | crash ở 6/8 bậc | Không có lợi cả khi ổn định |

Ba ý chính:

1. **GPU chỉ đáng dùng cho việc chạy ngoài tiến trình** (tier transcode, extract). Ở đó mỗi job là một tiến trình
   ffmpeg riêng, frame ở lại trên GPU (L1) và driver lỗi chỉ làm hỏng job đó.
2. **Trên WSL2 + Mesa d3d12, giải mã GPU trong tiến trình MediaServer không chạy ổn định**: 18/26 bậc chết vì
   segfault trong `vaEndPicture` (driver `libigd12dxva64.so`). Một lỗi driver làm sập cả server. Probe lúc khởi
   động không bắt được lỗi này. Sau khi sửa lỗi restart ở mục 7.2, server vẫn crash (mục 10), nên việc decoder bị
   tạo/huỷ không phải điều kiện cần. Trên WSL2 dùng `hwcodec.inProcess=0`.
3. Ngay cả khi chạy được, **giải mã H.264 trên GPU WSL không rẻ CPU hơn**: chép frame về RAM và đổi NV12→YUV420P
   tốn 5,3 ms CPU/frame 1080p, trong khi giải mã CPU tốn 4,6 ms. Mã hoá thì có lợi (6–7 ms so với 18–23 ms/frame).

Ngoài ra đợt đo tìm ra 3 lỗi của server, không liên quan tới GPU (mục 7). Kết quả đo lại sau khi sửa ở mục 10.

## 2. Môi trường

| Mục | Giá trị |
|---|---|
| Máy | Laptop i7-12700H, WSL2 (kernel 6.18.33.2-microsoft-standard-WSL2), **8 vCPU, 7,7 GB RAM** |
| GPU | Intel Iris Xe (iGPU) qua GPU-PV: `modprobe vgem`, `LIBVA_DRIVER_NAME=d3d12`, Mesa 23.2.1 D3D12 |
| Probe của MediaServer | `vaapi:/dev/dri/renderD128: decode h264=1 h265=1, encode h264=1 h265=1`; cuda/qsv không có |
| FFmpeg | n7.1.5 đi kèm (`release/linux/3rdpart/ffmpeg`); `hevc_vaapi` nay chạy được (với 4.4 từng segfault) |
| MediaServer | Debug, `build-test` tại develop `07c742c9`; không `-t` (poller = số lõi), `-l 2` |
| Nguồn | testsrc2 + nhiễu, **không B-frame**, GOP 2 s: Main 1080p25 4 Mbps, Sub 360p25 512 kbps (`rtsp_source.py`); segment 60 s 1080p25 H.264 4 Mbps / H.265 3 Mbps |
| Tải GPU | bộ đếm Windows `GPU Engine` cộng theo engine; mã hoá Intel chạy trên engine `videodecode` (VDBox) |

## 3. L0 — năng lực thô của ffmpeg đi kèm

Tham số giống trong server: giải mã CPU `threads=auto`; GPU `threads=1`, tải về RAM rồi đổi YUV420P. Mã hoá libx264
`veryfast zerolatency main bf=0` so với `h264_vaapi`. 1500 frame giải mã, 600 frame mã hoá.

| Ca | Chế độ | Song song | fps tổng | Lõi CPU | ms CPU/frame | Engine GPU bận |
|---|---|---|---|---|---|---|
| Giải mã H.264 1080p | CPU | 1 / 4 | 987 / 1539 | 4,6 / 7,0 | 4,6 / 4,5 | — |
| | GPU | 1 / 4 | 64 / 416 | 0,34 / 1,85 | **5,3 / 4,4** | videodecode 15 % / 44 %, 3d 29 % / 78 % |
| Giải mã HEVC 1080p | CPU | 1 / 4 | 372 / 746 | 2,6 / 6,8 | 6,9 / 9,1 | — |
| | GPU | 1 / 4 | 73 / 434 | 0,33 / 1,9 | **4,6 / 4,4** | videodecode 13 % / 24 % |
| Mã hoá H.264 1080p | CPU | 1 / 4 | 199 / 280 | 3,6 / 6,4 | 18,2 / 22,9 | — |
| | GPU | 1 / 4 | 57 / 333 | 0,39 / 1,9 | **6,8 / 5,7** | videodecode 28 % / **97 %** |

Đọc bảng:

- Một luồng GPU bị giới hạn bởi độ trễ của lớp d3d12 (64 fps giải mã, 57 fps mã hoá), nhưng thông lượng tăng gần
  tuyến tính theo số luồng. Trần mã hoá H.264 1080p của iGPU này khoảng **330 fps** (engine đã 97 %), tức khoảng 13
  luồng 1080p25 hoặc 66 luồng 1080p5. Đây là số nên dùng để đặt `hwcodec.encodeBudget` (`vaapi:300`).
- **Giải mã GPU không giảm CPU mỗi frame với H.264** (5,3 so với 4,6 ms). Phần CPU còn lại là chép frame qua GPU-PV
  và đổi định dạng (engine `3d` bận 29–78 % là để chép/convert). Với HEVC thì có lợi (4,4–4,6 so với 6,9–9,1 ms). Mã
  hoá có lợi rõ nhất (giảm khoảng 3 lần).

## 4. Tính năng chạy ngoài tiến trình

### 4.1 Transcode khi chuyển tầng (`FfmpegTierMediaTransformer::transform`)

Gọi thật qua `integration_tier_transcode_ffmpeg --gtest_filter=*BenchmarkFromEnv*`. Mỗi ca chạy 3 lần trên segment
60 s 1080p25; trung bình (thời gian / CPU-giây của tiến trình ffmpeg con). Mức đọc từ argv của ffmpeg.

| Nguồn → đích | CPU (L3) | GPU | CPU giảm | Thời gian |
|---|---|---|---|---|
| H.264 → H.264 720p15 1,5 Mbps | 7,8 s / 53,3 | L1 13,6 s / 4,7 | 11,4× | chậm 1,7× |
| H.264 → H.264 360p10 0,5 Mbps | 3,4 s / 21,8 | L1 9,0 s / 3,5 | 6,3× | chậm 2,7× |
| H.264 → H.265 720p15 | 20,9 s / 94,5 | L1 13,7 s / 5,0 | **19×** | **nhanh 1,5×** |
| H.264 → H.265 360p10 | 7,5 s / 25,7 | L1 9,0 s / 3,5 | 7,3× | chậm 1,2× |
| H.265 → H.264 720p15 | 8,8 s / 61,1 | L1 13,3 s / 4,4 | 13,8× | chậm 1,5× |
| H.265 → H.264 360p10 | 5,2 s / 25,0 | L1 9,3 s / 3,5 | 7,1× | chậm 1,8× |
| H.265 → H.265 720p15 | 21,2 s / 94,5 | L1 13,2 s / 4,6; **2/3 lần L2** 9,6–23,1 s / 18,5 | 5–20× | nhanh 1,6× (L1) |
| H.265 → H.265 360p10 | 8,1 s / 31,1 | L1 9,9 s / 3,9; **1/3 lần L2** 14,2 s / 16,0 | 2–8× | ≈ |

- GPU mất tối thiểu khoảng 9 s cho mỗi segment, bất kể kích thước đầu ra. Đó là chi phí cố định của lớp d3d12 (tạo
  device, đồng bộ), tức khoảng 4,4–6,7× thời gian thực cho một job. Với CPU thì bản nhỏ 360p H.264 nhanh hơn GPU,
  nhưng tốn gấp 6 lần CPU.
- **Mọi ca đích H.265 đều có lợi về cả hai mặt** (libx265 rất nặng).
- H.265 → H.265: L1 lỗi ở 3/6 lần, chạy lại L2 (giải mã/scale CPU, mã hoá GPU), đúng thiết kế fallback (L1 của
  backend nghỉ `cooldownSec`). Chưa tìm nguyên nhân L1 lỗi với tổ hợp `hevc` giải mã GPU + `hevc_vaapi`.

### 4.2 Trích xuất có overlay (`extractArchived`, JWT viewer)

Clip 60 s Main 1080p25 của camera 0; mã hoá GPU qua `HwCodecManager` (log `overlay re-encode on GPU backend vaapi`).

| | CPU | GPU | Chênh lệch |
|---|---|---|---|
| 1 job: thời gian / CPU-giây ffmpeg | 35,5 s / 187 (≈5,3 lõi) | 35,3 s / 137 (≈3,9 lõi) | CPU −27 % |
| 3 job đồng thời: thời gian mỗi job / tổng CPU-giây | 86 s / 624 (bão hoà 8 vCPU) | 70,5 s / 497 | thời gian −18 %, CPU −20 % |
| Đầu ra | h264 1920×1080 25 fps 60,0 s | như CPU | — |

Giải mã 1080p và vẽ overlay vẫn chạy CPU, nên đây là phần chiếm phần lớn. Muốn giảm thêm phải giải mã GPU + overlay
trên GPU (`overlay_vaapi`), hiện chưa làm.

## 5. Tính năng trong tiến trình (MediaServer live)

### 5.1 CPU — rig 16 camera (số sạch, có warmup 45 s)

CPU server tính theo % một lõi. Nền idle (16 camera chỉ proxy, 32 luồng) là 25,8 %.

| Kịch bản | N | CPU server % | Tăng so với idle, mỗi luồng | RSS MB | Bỏ frame |
|---|---|---|---|---|---|
| Motion Sub 360p25 | 1 / 4 / 8 / 16 | 33,0 / 37,0 / 54,8 / 71,8 | **≈2,9 %** | 259 → 708 | 0 |
| Motion Main 1080p25 | 1 / 4 / 8 / 16 | 46,8 / 93,4 / 150,8 / 290,3 | **≈16,5 %** | 665 → 3 149 | 0 |
| Transcode overlay Main → 1080p 5 fps | 1 / 2 / 4 / 8 | 60,1 / 74,8 / 104,1 / 197,8 | **≈21,5 %** | 379 → 931 | 0, viewer đủ 5 fps |

Theo đó, với 8 vCPU, chạy hết bằng CPU thì một máy như vậy gánh được khoảng 200 camera motion Sub, khoảng 35
camera motion Main, hoặc khoảng 30 luồng transcode overlay (để chừa 20 % CPU). Motion Main 16 camera tốn 3,1 GB
RSS (ring 512 frame decode cho mỗi luồng; đã sửa, còn 533 MB, mục 10).

### 5.2 GPU so với CPU — rig nhỏ (cùng warmup)

| Kịch bản | CPU | GPU | Ghi chú GPU |
|---|---|---|---|
| Motion Sub n=2 (rig 4 camera, idle 10 %) | 15,9 % (+3,0 %/luồng), RSS 190 MB | 20,7 % (+5,4 %/luồng), RSS 652 MB | 2/2 decoder GPU; **tốn CPU hơn** |
| Motion Main n=4 (rig 4 camera) | 64,7 % (+13,7 %/luồng), RSS 762 MB | 52,4 % (+10,6 %/luồng), **RSS 5 468 MB** | 4/4 GPU, **19 lần bỏ frame** trong 40 s |
| Transcode overlay n=1 / n=2 (rig 2 camera, idle ≈7 %) | 39,1 % (+32 %/luồng) | 22,3 % / 39,2 % (+15–16 %/luồng) | 1/1, 2/2 decoder + encoder GPU (lượt thử, chưa có warmup) |
| Transcode overlay n=4 (rig 4 camera) | 99,1 % | 95,1 % | 0/4 GPU: đã rơi về CPU sau lỗi GPU |

Kết luận: trên máy này, transcode live có lợi khoảng 50 % CPU mỗi luồng khi GPU chạy được; motion Main có lợi
khoảng 23 % nhưng GPU không theo kịp 4 × 25 fps (bỏ frame); motion Sub thì GPU tệ hơn. Cả ba đều bị chặn bởi mục 6.

## 6. Ổn định của GPU trong tiến trình

| Lượt | Bậc GPU (không tính idle) | Crash | Bậc sống |
|---|---|---|---|
| 16 camera, mặc định | 9 | 8 | transcode n=2 (nhưng viewer 0 fps, 40 lần bỏ frame) |
| 16 camera, `maxDecodeSessions=2;maxEncodeSessions=2` | 8 | 4 | motion Sub 8 (2 GPU), motion Main 4/8/16 (≤2 GPU) |
| 4 camera, mặc định | 9 | 6 | motion Sub 2, motion Main 4, transcode 4 (đã về CPU) |
| **Tổng** | **26** | **18** | |
| CPU (5 lượt) | 51 | 1 (lỗi 7.1, không phải GPU) | |

Mọi crash GPU đều là `SIGSEGV` với cùng một stack: `avcodec_send_packet` → VAAPI → `vaEndPicture` →
`d3d12_drv_video.so` → `libd3d12core.so` → `libigd12dxva64.so` (driver Intel của Windows trong WSL). Đã tách khỏi
code server bằng ffmpeg thuần:

| ffmpeg n7.1.5, một tiến trình, file mp4 60 s, `-re` | Kết quả |
|---|---|
| 1, 2, 4 decoder 360p hoặc 1080p, **chung một device** (`-init_hw_device`) | chạy hết 60 s |
| 4 decoder 360p, **mỗi input một device** | segfault sau 9 s |
| 4 tiến trình riêng (L0) | không lỗi |

MediaServer dùng một device chung. Giả thuyết ban đầu là crash do decoder bị tạo/huỷ liên tục trong lúc luồng
restart (lỗi 7.2). Sau khi sửa 7.2, server vẫn crash, có lần chỉ với một decoder 1080p. Mã hoá `h264_vaapi` trong
tiến trình cũng crash ở cùng offset. Không tái hiện được bằng ffmpeg CLI, cũng không bằng `FFmpegDecoder` chạy riêng
(mục 10), nên nguyên nhân trong driver chưa tách được. Một số
decoder còn gặp `av_hwframe_transfer_data: Input/output error` 3 lần liên tiếp rồi tự chuyển CPU (fallback chạy
đúng), nhưng segfault thì không fallback được.

Hệ quả cho production:

- **Driver GPU lỗi = cả MediaServer sập** (mọi camera, ghi hình, viewer). Probe trong tiến trình con chỉ bảo vệ lúc
  khởi động. `hwcodec.failThreshold`/breaker chỉ xử lý mã lỗi, không xử lý segfault.
- Không dùng `hwcodec.enable=1` cho pipeline trong tiến trình trên WSL2/Mesa d3d12. Trên máy Linux có driver `iHD`
  hoặc NVIDIA thật vẫn **chưa đo** (docs/pending-real-device-checks.md); phải đo lại bằng harness này trước khi bật.

## 7. Lỗi server phát hiện trong lúc đo (không phụ thuộc GPU)

1. **Crash `terminate` khi processor bị huỷ trên chính decoder thread** (lượt CPU, motion Main 16 camera, 1/51 bậc
   CPU). Callback giải mã ở `MultiMediaSourceProcessor::addTrack` (src/Processor/MultiMediaSourceProcessor.cpp:299)
   `lock()` processor trên decoder thread. Nếu phía chủ sở hữu đã nhả processor (nguồn đóng hoặc luồng khởi động lại),
   tham chiếu cuối mất ngay trên decoder thread: `~MultiMediaSourceProcessor` → `close()` →
   `MediaSourceDecoder::resetTracks()` → `~FFmpegDecoder` → `TaskManager::stopThread()` → deleter của thread gọi
   `join()` chính thread đó (src/Codec/Transcode.cpp:156) → `std::system_error` (EDEADLK) trong destructor →
   `std::terminate` (SIGABRT). Khối `try/catch` trong `~MultiMediaSourceProcessor` không cứu được. Dễ xảy ra hơn khi
   decoder trễ (nhiều luồng 1080p giải mã CPU). **Đã sửa** (22628e24): nếu tham chiếu cuối mất trên decoder thread,
   processor được huỷ trên poller của nó.
2. **Luồng camera khởi động lại liên tục khoảng 30 s sau khi đổi cấu hình**: play lại thất bại với
   `media source already existed: hls://<cam>/<stream>` cho tới khi nguồn HLS cũ hết hạn. Đo được 2–64 lần/bậc
   (`restart_churn` trong JSON). Trong khoảng đó camera không có hình và decoder bị tạo/huỷ liên tục (nguồn của
   crash 7.1 và mục 6). **Đã sửa** (b26a05a0): nguồn HLS đã eof nhường chỗ cho nguồn mới cùng tên.
3. **Cấu hình camera cũ được nạp trước cấu hình mới khi khởi động**: `StatisticRecorder` nạp
   `record/<cam>/info.txt` (gồm option motion/stream) trước khi đọc cấu hình từ API, nên khoảng 20 s đầu server chạy
   motion theo cấu hình lần chạy trước rồi mới đổi. Có thể đúng thiết kế (chạy tiếp khi API chậm), nhưng nó kéo theo
   lần khởi động lại ở mục 2. Giữ nguyên theo thiết kế. Harness trước đây xoá nhầm đường dẫn (`record/<cam>/` thay vì
   `record/record/<cam>/`), nên cấu hình của bậc trước vẫn chạy ở đầu bậc sau; đã sửa ở P7.

## 8. Khuyến nghị

1. Giữ GPU cho **tier transcode** (`storage.tier_transcode_hwaccel=1`, mặc định) và **extract overlay**. Đặt
   `hwcodec.encodeBudget` theo trần đo được (`vaapi:300` cho iGPU loại này) để job offline không chiếm hết engine.
2. Với máy chỉ có GPU qua WSL/d3d12: đặt `hwcodec.inProcess=0` (live chạy CPU, GPU chỉ dùng cho tier/extract).
   Đặt `hwcodec.decode=0` thôi là không đủ, vì mã hoá trong tiến trình cũng crash.
3. Lỗi 7.1 và 7.2 đã sửa (mục 10).
4. Cân nhắc cách ly giải mã GPU khỏi tiến trình chính (tiến trình con giải mã, hoặc chạy motion/transcode live bằng
   ffmpeg ngoài) nếu muốn bật GPU trên driver chưa kiểm chứng.
5. Đo lại trên máy Linux có Intel `iHD` hoặc NVIDIA thật bằng cùng harness, trước khi khuyến nghị GPU cho
   motion/transcode live trong production.

## 9. Giới hạn của đợt đo

- MediaServer bản Debug: glue và phân tích motion chạy -O0, nên con số tuyệt đối của motion cao hơn Release. Codec
  (libavcodec/libx264/libx265) là bản tối ưu.
- WSL2 GPU-PV có chi phí chép frame và đồng bộ riêng, không đại diện cho Linux thật.
- Bộ đếm GPU của Windows cộng mọi tiến trình Windows, nên có nhiễu nền vài %.
- Số GPU của transcode live lấy từ rig 2 camera (lượt thử, cửa sổ 20 s, chưa có warmup), vì ở rig lớn hơn GPU
  không sống qua lúc khởi động.
- Nguồn tổng hợp không B-frame: `rtsp_source.py` đánh timestamp RTP theo thứ tự giải mã. Clip có B-frame làm pts
  đi lùi và `FFmpegFrameRateFilter` reset ở mỗi B-frame (đã thấy khi thử), nên camera thật dùng B-frame cần đo riêng.

## 10. Đo lại sau khi sửa (docs/hwcodec-benchmark-fix-plan.md)

Cùng máy, cùng harness, sau các bản sửa P1 (22628e24), P3 (b26a05a0), P4 (7ba32286), P5 (831cc564), P6 (e719a8b8),
`Process` (c70e03c9), và sửa harness xoá đúng `record/record/<cam>/info.txt`. Số liệu: `results-2026-10-24/p7-*.json`.
Nền idle lần này là 38 % (trước đây 26 %): trong lúc đo có một MediaServer của phiên khác chạy trên máy. Vì vậy nên so
mức tăng mỗi luồng, không so số tuyệt đối.

### 10.1 CPU, 16 camera

| Kịch bản | N | CPU server % | Tăng mỗi luồng | RSS MB (trước khi sửa) | Bỏ frame |
|---|---|---|---|---|---|
| Motion Sub 360p25 | 1 / 4 / 8 / 16 | 40,0 / 57,1 / 74,0 / 99,1 | ≈3,8 % | 217 → 274 (259 → 708) | 0 |
| Motion Main 1080p25 | 1 / 4 / 8 / 16 | 56,9 / 104,7 / 192,1 / 267,2 | ≈14,3 % | 234 → 533 (665 → 3 149) | 0 |
| Transcode overlay → 1080p 5 fps | 1 / 2 / 4 / 8 | 62,3 / 80,0 / 134,6 / 237,0 | ≈24,8 % | 316 → 828 (379 → 931) | 0, viewer đủ 5 fps |

`restart_churn` bằng 0 ở mọi bậc (trước đây 2–64). Không crash ở bậc nào (trước đây 1/51 bậc CPU crash, lỗi 7.1).
RSS motion giảm 3–6 lần nhờ P5: ring frame đã giải mã không còn giữ cả GOP.

### 10.2 GPU

| Cấu hình | Bậc live | Crash | Ghi chú |
|---|---|---|---|
| `hwcodec.enable=1` mặc định (motion Sub/Main, transcode; 1, 2, 4) | 9 | **5** | Vẫn segfault trong `libigd12dxva64.so` dù đã hết churn |
| `hwcodec.inProcess=0`, đủ các bậc như CPU | 12 | **0** | Live chạy CPU (CPU/RSS ngang 10.1); GPU chỉ dùng cho extract |

Lỗi driver chỉ lộ ra trong ngữ cảnh đầy đủ của MediaServer. Các cách sau đều **không** tái hiện được: ffmpeg CLI với
cùng bitstream, kể cả khi bỏ frame kiểu `addDecodeTask`; các cờ `low_delay`/`fast`; `FFmpegDecoder` thật trong unit
test với 1–4 decoder, consumer chậm, 1100 fd mở sẵn. Vì vậy probe stress trong tiến trình con cũng không bắt được.

### 10.3 Trích xuất có overlay (`hwcodec.inProcess=0`)

| | CPU | GPU vaapi |
|---|---|---|
| 1 job: thời gian / CPU ffmpeg | 35,0 s / 187 s | 35,3 s / 143 s (−23 %) |
| 3 job đồng thời: thời gian / CPU ffmpeg | 85,9 s / 630 s | 65–70 s / 479–485 s (nhanh hơn khoảng 18 %, −23 % CPU) |

Trong 5 lượt 3 job trên GPU (15 job), có 2 lần ffmpeg segfault. Trước khi sửa `Process`, tiến trình chết vì signal
bị báo exit code 0, nên job báo SUCCESS và giao file mp4 thiếu moov. Lỗi này gặp 2/4 lượt 3 job trong đợt đo trước.
Sau khi sửa, server thấy `exit code = 139` và chạy lại job bằng libx264: job đó mất 221 s, nhưng file vẫn đúng.

### 10.4 Tier transcode HEVC → H.265

L1 lỗi là do driver: chạy tay đúng lệnh L1 bằng ffmpeg 10 lần thì 2 lần segfault (exit 139). Sau lần lỗi đầu, các
segment kế tiếp chạy L2 trong `hwcodec.cooldownSec` (khoảng 17 s CPU thay vì 4,6 s cho mỗi đoạn 60 s). Lỗi trong
job/log nay có `exit code 139:` ở đầu. Các tổ hợp còn lại chạy L1 ổn định.

### 10.5 Kết luận sau khi sửa

1. Chế độ CPU ổn định: không crash, không restart lặp, RAM motion giảm 3–6 lần.
2. Trên WSL2/d3d12, đặt `hwcodec.inProcess=0`: 0 crash, và vẫn được lợi GPU cho tier (7–19 lần) và extract (−23 % CPU).
3. Việc chạy ngoài tiến trình vẫn có lúc gặp segfault driver. Nay cả tier lẫn extract đều phát hiện được và tự chạy
   lại bằng cách khác.
4. Vẫn cần đo trên máy Linux có Intel `iHD` hoặc NVIDIA thật trước khi bật GPU cho live.
