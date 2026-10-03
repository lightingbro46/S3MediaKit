# Plan: camera RTSP MJPEG — transcode sang H264 ngay lúc kéo luồng

Ngày lập: 2026-10-03. Nhánh: `feature/mjpeg-ingest-transcode` (tách từ `develop` 34a2b3c0).
Trạng thái: Q1–Q3 đã chốt 2026-10-03 (Q1 theo đề xuất, Q2 báo `JPEG`, Q3 chỉ config toàn cục); P1–P6 đã làm
2026-10-03, chưa commit. Kết quả và chỗ khác plan ở mục 8. Tài liệu cho team: [mjpeg-camera.md](./mjpeg-camera.md).

## 1. Mục tiêu

1. Camera RTSP có video MJPEG (RTP/JPEG, RFC 2435, PT 26) xem trực tiếp được trên **mọi** giao thức client đang
   dùng (WebRTC, HTTP-FLV/RTMP, HLS, fMP4, RTSP), và bản ghi xem lại được như camera H264.
2. Cách làm: giải mã MJPEG → mã hoá H264 **một lần ngay khi kéo luồng**, trước `MultiMediaSourceMuxer`. Mọi thứ phía
   sau (output giao thức, ghi MP4, motion, snapshot, reduce quality, overlay) chỉ thấy H264, không cần sửa.
3. **Vẫn giữ codec gốc** để báo đúng lên client: API trả cả codec đang phát (H264) và codec của camera (JPEG),
   kèm cờ `transcoded`.

Ngoài phạm vi:
- MJPEG đi vào bằng đường khác ngoài kéo RTSP (RTSP push, RTP/GB28181, FFmpegSource, plugin SDK hãng).
- Transcode khi xem lại bản ghi MJPEG cũ. Trước nhánh này chưa có camera MJPEG nào được dùng, nên coi như không có.
- Giải mã MJPEG bằng GPU. HwCodec hiện chỉ có hwaccel cho h264/hevc, MJPEG giải mã bằng CPU. **Mã hoá** H264 thì
  dùng lại `FFmpegEncoder` nên tự được hưởng GPU encode của HwCodec.

## 2. Vì sao chọn hướng này

- Trình duyệt không giải mã MJPEG qua WebRTC hay MSE. RTMP/FLV và MPEG-TS cũng không có chỗ cho MJPEG
  (xem báo cáo review mục 1–4). Muốn tất cả các giao thức chạy được thì chỉ còn cách đổi codec.
- Transcode một lần lúc kéo luồng rẻ hơn transcode theo từng người xem. Bản ghi H264 cũng nhỏ hơn MJPEG từ 5 đến
  10 lần.
- Các thành phần cần dùng đều đã có: `FFmpegDecoder` (`AV_CODEC_ID_MJPEG`, `src/Codec/Transcode.cpp:438`) và
  `FFmpegEncoder` (H264/H265, có GPU và tự lùi về x264).

## 3. Hiện trạng liên quan

| Thành phần | Vị trí | Ghi chú |
|---|---|---|
| Kéo camera | `StreamSource` → `addStreamProxy` → `PlayerProxy` (`manager/Camera/StreamSource.cpp:235`) | |
| Gắn track vào muxer | `PlayerProxy::onPlaySuccess` (`src/Player/PlayerProxy.cpp:274`) | `videoTrack->addDelegate(_muxer)` |
| RTSP direct proxy | `PlayerProxy::setDirectProxy` + `RtspPlayerImp::onCheckSDP` (`src/Rtsp/RtspPlayer.cpp:883`) | Output RTSP là RTP gốc của camera, còn muxer chạy với `enable_rtsp=false`. **Phải tắt cho camera MJPEG**, nếu không client RTSP vẫn nhận JPEG |
| Đăng ký RtspMediaSource | `RtspMediaSource::setSdp` / `onWrite` (`src/Rtsp/RtspMediaSourceImp.cpp:5,72`) | Chỉ `regist()` khi đã có cả SDP và ring. Không gọi `setSdp` thì source không bao giờ được đăng ký |
| Giải mã nền | `FFmpegDecoder(track, 0)` + `inputFrame(frame, live=true, async=true)` (`src/Processor/MediaSourceDecoder.cpp:39`) | `TaskManager` có thread riêng, hàng đợi tối đa 30 task |
| Mã hoá | `FFmpegEncoder::inputFrame(frame, output_pts)` + `setOnEncode` | Callback chạy đồng bộ trên thread gọi. SPS/PPS in-band, không có B-frame |
| Báo codec | `PlayerProxy::setTranslationInfo`, `StreamSource::getTranslationInfo` → `CameraStatistic` (`StreamStatistic::vcodec`) → `Manager.cpp:1274` `makeStreamStatisticJson`; `Manager.cpp:1248` `makeMediaSourceJson`; `WebApi.cpp:478` `codec_id_name` | Đều đọc `track->getCodecName()` của track trong muxer |
| Lưu thống kê | `CameraStatistic` lưu JSON (`makeStreamStatisticJson`/`getStreamStatistic`, `CameraStatistic.cpp:170-210`) | Thêm field JSON, **không cần migration** |
| Lỗi đã thấy ở codec JPEG | `JPEGRtp.cpp:810,821` dùng `getStamp()/90`; `Frame.h:36` map TS sang `PSI_STREAM_JPEG_2000` | Xem P1 |

## 4. Thiết kế

### 4.1 Luồng dữ liệu

```
RtspPlayer ──JPEG frame──► MjpegIngestTranscoder ──H264 frame──► H264Track (origin=JPEG) ──► MultiMediaSourceMuxer
   (poller P)   │ thread decoder riêng: FFmpegDecoder(MJPEG) → FFmpegEncoder(H264)
                └──► kết quả encode được P->async(...) đưa về poller P (giữ thread affinity của muxer)
audio track ─────────────────────────────────────────────────────────────────────► MultiMediaSourceMuxer (đi thẳng)
```

- Class mới **`MjpegIngestTranscoder`** (`src/Transcode/MjpegIngestTranscoder.{h,cpp}`, trong `#if defined(ENABLE_FFMPEG)`):
  - Là `FrameWriterInterface`, được gắn làm delegate của JPEGTrack gốc.
  - Sở hữu `FFmpegDecoder` chạy async trên thread riêng. Encode làm luôn trong `onDecode` trên chính thread đó, không
    chạy trên poller. Đây là cùng cách với `MediaSourceDecoder`: việc CPU liên tục nằm ở thread riêng của decoder,
    không đẩy sang `WorkThreadPool` hay `BlockingExecutors`.
  - Frame H264 ra được đưa về poller của player bằng `weak_ptr` + `poller->async` rồi `outputTrack()->inputFrame(...)`.
  - `outputTrack()` là `H264Track` tạo bằng `Factory::getTrackByCodecId(CodecH264)`, đặt `setOriginCodecId(CodecJPEG)`
    (mục 4.3) và cùng `index` với track gốc.
  - Timestamp: lấy dts (ms) của frame JPEG làm `output_pts` cho encoder, tức giữ timeline nguồn. Không có B-frame
    nên dts = pts.
  - GOP mặc định 2 s (`2 * fps`) để giữ nhịp segment HLS và record như camera H264. fps lấy từ JPEGTrack.
  - `requestKeyFrame()` cho frame đầu tiên. Đóng (shutdown, đổi track, huỷ) thì `stopThread(true)` + `flush()`.
- **`PlayerProxy::onPlaySuccess`**: nếu track video là `CodecJPEG` và config cho phép (mục 4.4):
  - tạo `MjpegIngestTranscoder`, rồi `_muxer->addTrack(transcoder->outputTrack())` thay cho JPEGTrack;
  - JPEGTrack `addDelegate(transcoder)`, còn output track `addDelegate(_muxer)`;
  - bỏ direct proxy: `setMediaSource(nullptr)`, và muxer giữ `enable_rtsp` theo `_option` (không ép `false`).
  - Không bật được (`ENABLE_FFMPEG=OFF`, config tắt, hết `maxSessions`) thì giữ hành vi cũ (pass-through JPEG)
    và `WarnL` một lần.
- **`RtspPlayerImp::onCheckSDP`**: nếu SDP có video `JPEG` (PT 26 / rtpmap JPEG) và player có cờ
  `Client::kMjpegTranscode` (PlayerProxy đặt khi bật tính năng) thì **không** `setSdp` vào `_rtsp_media_src` và đặt
  nó về `nullptr`. Source direct proxy khi đó không bao giờ được đăng ký nên không có giai đoạn RTSP phát JPEG.
- **Shutdown/replay**: trong `setOnShutdown` của `PlayerProxy`, gỡ delegate của transcoder và reset nó cùng lúc với
  `_muxer->resetTracks()`. Lần play sau tạo transcoder mới. Nhánh `reset_when_replay=false` dùng lại muxer, còn
  track H264 mới vẫn khớp `index` và codec nên `MediaSink` nhận lại bình thường.

### 4.2 Hành vi khi lỗi hoặc quá tải

- Hàng đợi decoder đầy (CPU không kịp) thì `TaskManager` bỏ frame. Mỗi frame MJPEG đều là keyframe nên bỏ frame
  không làm vỡ ảnh. H264 vẫn liền mạch vì chỉ bị giảm fps. Đếm số frame bị bỏ vào thống kê.
- Encoder mở lỗi: `FFmpegEncoder` tự lùi về libx264. libx264 cũng lỗi thì log `ErrorL` và stream không có video.
  `MediaSink` sẽ bỏ track sau thời gian chờ như hiện tại. Không lùi về pass-through giữa chừng vì track đã đăng ký
  với muxer.
- Đổi độ phân giải giữa chừng: decoder trả frame có kích thước mới thì mở lại encoder và gửi keyframe mới có SPS mới.
  Cần kiểm tra `FFmpegEncoder` có tự xử lý không, nếu không thì transcoder tạo lại encoder.

### 4.3 Giữ codec gốc để báo client

- `Track` thêm `CodecId _origin_codec = CodecInvalid` cùng `setOriginCodecId()` và
  `getOriginCodecId()` (trả `getCodecId()` khi chưa đặt). Đây là metadata codec, được `clone()` copy theo đúng
  invariant "clone chỉ copy codec metadata".
- Báo lên client. Các field mới chỉ được **thêm**, không đổi nghĩa field cũ (xem Q1):

| Nơi | Field hiện có | Thêm |
|---|---|---|
| `getMediaList` / `getMediaInfo` (`WebApi.cpp:478`) | `codec_id`, `codec_id_name` | `origin_codec_id`, `origin_codec_id_name`, `transcoded` |
| `makeMediaSourceJson` (`Manager.cpp:1248`) | `vcodec` | `sourceVcodec`, `transcoded` |
| `StreamInfo` / `TranslationInfo` (`PlayerProxy.h:11`) | `codec_name` | `origin_codec_name` |
| `StreamStatistic` + JSON lưu (`CameraStatistic.h:33`, `.cpp:170-210`) | `vcodec` | `source_vcodec`, `transcoded` |
| `makeStreamStatisticJson` (`Manager.cpp:1274`) | `vcodec` | `sourceVcodec`, `transcoded` |

- Webhook `on_stream_changed` và các API dùng chung JSON `MediaSource` sẽ tự có các field này nếu dùng chung hàm.
  Cần rà lúc làm.
- `ReduceQualityCapabilityBuilder` (`CameraStatistic.cpp:70`) đọc `vcodec`. Vì `vcodec` là H264 nên camera MJPEG
  tự có profile reduce quality. Đúng mong muốn, không cần sửa.

### 4.4 Config (mặc định đăng ký bằng `onceToken`, đọc bằng `GET_CONFIG`)

| Key | Mặc định | Ý nghĩa |
|---|---|---|
| `mjpeg.ingestTranscode` | `1` | Bật transcode MJPEG → H264 khi kéo luồng |
| `mjpeg.ingestCodec` | `H264` | Codec đích (`H264`/`H265`). Mặc định H264 vì trình duyệt hỗ trợ rộng nhất |
| `mjpeg.ingestBitrate` | `0` | bit/s; `0` = tự chọn theo độ phân giải (logic sẵn có của `FFmpegEncoder`) |
| `mjpeg.ingestGopSec` | `2` | Khoảng keyframe tính bằng giây |
| `mjpeg.ingestMaxSessions` | `0` | Số camera MJPEG được transcode cùng lúc; `0` = không giới hạn. Vượt thì pass-through + cảnh báo |

Theo memory "config production sinh từ default trong code": default phải đúng ngay trong code, `conf/config.ini`
chỉ để tài liệu.

### 4.5 Thống kê

`getStatistic` thêm `MjpegIngestTranscoder` (ObjectStatistic). Mỗi phiên ghi log lúc tắt: số frame vào, số frame ra,
số frame bị bỏ, encoder đã dùng (GPU hay x264).

## 5. Các bước

| # | Việc | File chính | Kiểm tra |
|---|---|---|---|
| **P1** | Sửa lỗi nền của codec JPEG: decoder RTP dùng `rtp->getStampMS()` (NTP/RTCP, xử lý quay vòng); TS map `CodecJPEG` → `PSI_STREAM_RESERVED` để HLS/TS bỏ track thay vì ghi sai thành JPEG 2000; `JPEGTrack` cập nhật width/height khi SOF đổi và chặn chia 0 khi tính fps | `ext-codec/JPEGRtp.cpp`, `src/Extension/Frame.h`, `ext-codec/JPEG.cpp` | Unit test: RTP JPEG → frame (stamp, kích thước), TS bỏ track JPEG |
| **P2** | `Track` origin codec + toàn bộ chỗ báo codec ở mục 4.3 | `src/Extension/Track.h`, `server/WebApi.cpp`, `server/Manager.cpp`, `src/Player/PlayerProxy.{h,cpp}`, `manager/Camera/StreamSource.cpp`, `manager/Camera/CameraStatistic.{h,cpp}` | Unit test clone giữ origin; JSON statistic đọc/ghi đủ field mới, đọc JSON cũ không có field vẫn chạy |
| **P3** | `MjpegIngestTranscoder` + config `mjpeg.*` | `src/Transcode/MjpegIngestTranscoder.{h,cpp}`, `src/Common/config.{h,cpp}` | Unit test: tạo JPEG bằng encoder MJPEG của FFmpeg, đưa vào, kiểm tra ra H264 có keyframe + SPS/PPS, track ready, timestamp tăng đều, origin = JPEG, đóng không treo/không rò thread |
| **P4** | Gắn vào `PlayerProxy` + bỏ direct proxy trong `RtspPlayerImp::onCheckSDP`; xử lý shutdown/replay; `ingestMaxSessions` | `src/Player/PlayerProxy.{h,cpp}`, `src/Rtsp/RtspPlayer.cpp`, `src/Rtsp/RtspPlayerImp.h` | Build `build/` và `build-sync-off/`; build `ENABLE_FFMPEG=OFF` (memory ffmpeg-off-stack-guard) vẫn pass-through |
| **P5** | Thống kê `getStatistic` + docs: API media list, camera statistic, config, đoạn MJPEG trong `docs/hw-codec.md` | `server/WebApi.cpp`, `docs/*` | |
| **P6** | Chạy thật với camera giả MJPEG: xem trực tiếp RTSP/WebRTC/FLV/HLS/fMP4; ghi → xem lại (timeline, seek, chuyển Main/Sub); motion, snapshot, overlay/reduce quality; ngắt và kết nối lại camera; codec báo lên đúng; đo CPU mỗi camera 1080p/720p | `tests/integration/e2e_real` (camera giả: `ffmpeg -re -i clip -c:v mjpeg -f rtsp`) | Kết quả ghi vào mục 8 |

Mỗi bước build `cmake --build build -j2` (memory: máy ~7 GB RAM) và chạy `ctest -R unit.` trong `build-test`.
Thêm file `.cpp` mới dưới `src/` thì phải configure lại build dir (memory manager-glob-needs-reconfigure).

## 6. Rủi ro

- **CPU**: mã hoá x264 1080p25 tốn khoảng 0,5–1 core mỗi camera, cộng thêm giải mã MJPEG. Có GPU thì encode chạy
  NVENC/VAAPI qua HwCodec. Cần `ingestMaxSessions` và số đo thật ở P6 để ghi vào docs.
- **Độ trễ**: thêm khoảng 1–3 frame (decode + encode, `zerolatency` của x264 đã được bật sẵn trong `FFmpegEncoder`
  nếu có, cần kiểm tra).
- **Motion giải mã hai lần**: một lần MJPEG ở ingest, một lần H264 trong `MultiMediaSourceProcessor`. Có thể tối ưu
  sau bằng cách đưa frame đã giải mã cho motion, nhưng không làm trong nhánh này.
- **RFC 2435 tối đa 2040 px**: camera MJPEG 4K không kéo được qua RTSP. Giới hạn này nằm ở giao thức, không sửa.

## 7. Câu hỏi cần chốt

- **Q1 — field `vcodec` báo gì?** Đề xuất: `vcodec`/`codec_id_name` = codec **client nhận được** (H264), vì player
  phía client khởi tạo decoder theo field này. Codec gốc của camera nằm ở field mới `sourceVcodec`/`origin_codec_id_name`
  cùng `transcoded=true`. Cách ngược lại (`vcodec`=JPEG) sẽ làm client nào đang dựa vào `vcodec` để chọn player bị sai.
- **Q2 — tên codec gốc**: `getCodecName()` trả `"JPEG"`, giống ONVIF `Encoding=JPEG`. Đề xuất báo `"JPEG"` để khớp
  ONVIF. Nếu client cần `"MJPEG"` thì chỉ map ở tầng JSON.
- **Q3 — bật/tắt theo camera?** Đề xuất: chỉ config toàn cục ở nhánh này. Bật/tắt theo camera cần thêm cột
  trong DB, có migration và sync, nên chỉ làm khi có yêu cầu.

## 8. Kết quả

### Chỗ khác plan

- `Manager.cpp` `makeMediaSourceJson` (static) không được ai gọi, nên không sửa. Webhook `on_stream_changed` dùng
  `makeMediaSourceJson` của `WebApi.cpp`, nên tự có `origin_codec_id(_name)`/`transcoded`.
- Encoder chạy trên thread của decoder, không cần `FFmpegFrameRateFilter`. Frame H264 ra mang timestamp nguồn.
- Đổi độ phân giải: transcoder tự mở lại encoder (không dựa vào `FFmpegEncoder`, vì encoder chỉ scale về kích
  thước cũ).
- Thêm một sửa nằm ngoài plan: `JPEGRtpDecoder::inputRtp` luôn trả `key_pos=false`, nên GOP cache RTSP của mọi
  nguồn JPEG đi thẳng không bao giờ bị cắt. Đo được người xem mới nhận ~250 frame (~10 s) ảnh cũ trong 3 s đầu.
  Nay gói đầu của mỗi ảnh (fragment offset 0) là key position, và đo lại còn 75 frame trong 3 s.
- `getStatistic` thêm `MjpegIngestTranscode`.

### Kiểm tra

- Build `cmake --build build --target MediaServer -j2`: đạt. Kiểm tra cú pháp (`-fsyntax-only`) các file đã sửa
  khi bỏ `ENABLE_FFMPEG`/`ENABLE_X264`/`ENABLE_MOTION` và khi bỏ `ENABLE_SYNC_CHANGES`: đạt. Chưa build trọn bộ
  `build-sync-off` hay bản `ENABLE_FFMPEG=OFF`.
- Unit test `tests/unit/codec/test_mjpeg_ingest.cpp` (7 case: origin codec qua clone, TS không map JPEG,
  JPEGTrack đọc/đổi độ phân giải, chặn stamp không tăng, RTP JPEG round trip với stamp ms và key position,
  config/giới hạn phiên, JPEG → H264 trên poller với đúng index/thread). Test đã bắt được lỗi frame đầu ra mang
  index 0, đã sửa. `ctest --test-dir build-test -R unit.`: 201/201 đạt.
- Chạy thật, MediaServer Debug trong thư mục tạm. "Camera" là ffmpeg đẩy clip MJPEG 1280x720 25 fps + PCMA vào
  `src/cam`, rồi `addStreamProxy` kéo về `live/cam2`:
  - `getMediaList`: rtsp/rtmp/hls/fmp4/ts có track `H264`, `origin_codec_id_name=JPEG`, `transcoded=true`,
    1280x720 25 fps; audio PCMA đi thẳng.
  - ffprobe/giải mã: RTSP, RTMP, HTTP-FLV, HTTP-TS, fMP4 (`/media/live/live/cam2.live.mp4`) và HLS đều ra H264
    Main 1280x720. Giải mã 10 s FLV và RTSP không có lỗi, khoảng 24–25 fps. TS/HLS không có audio vì MPEG-TS không
    chứa được G711, giống camera H264.
  - Ghi MP4 (`mp4_max_second=10`): segment H264 + PCMA dài ~12 s (GOP 2 s), giải mã không lỗi. Khoảng 2,6 Mbps so
    với ~11,4 Mbps của MJPEG gốc.
  - Ngắt camera 8 s rồi nối lại: transcoder cũ đóng (log số frame vào/ra), lần play sau tạo transcoder mới. Bộ đếm
    `MjpegIngestTranscode`/`VideoDecoder`/`VideoEncoder` vẫn là 1/1/1. Segment trước/sau khi ngắt liền mạch.
  - Xem lại: `loadMP4File` file đã ghi, rồi phát qua RTSP, fMP4 và HLS: giải mã được H264 1280x720. Bản ghi là H264
    nên khi xem lại `origin_codec_id_name` là `H264`; MP4 không lưu codec gốc.
  - `mjpeg.ingestTranscode=0`: proxy mới đi thẳng JPEG qua direct proxy RTSP (`transcoded=false`). Proxy đang
    transcode vẫn chạy.
  - CPU: một camera 720p25, giải mã MJPEG + libx264 veryfast trên thread decoder, khoảng 23% một core (bản Debug).

### Chưa kiểm

- WebRTC: bản build ở đây không bật `ENABLE_WEBRTC`. Luồng là H264 Main không B-frame nên về lý thuyết phát được,
  nhưng chưa chạy thật.
- `sourceVcodec` trong thống kê camera (`primaryStream`/`secondaryStream`) chưa chạy end-to-end vì cần camera
  trong VMS. Hai chỗ đọc/ghi JSON đã được build, nhưng chưa có test riêng.
- Camera MJPEG thật, GPU encode, và nhiều camera MJPEG cùng lúc.
