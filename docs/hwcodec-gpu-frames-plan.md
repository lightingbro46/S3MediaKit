# Plan: giữ khung hình trên GPU khi transcode (decode → scale → encode không qua RAM)

Trạng thái 2026-10-09: **chưa làm**, chỉ làm khi đo thấy cần (bước G0). Tài liệu tính năng GPU hiện
có: [hw-codec.md](hw-codec.md) (budget theo tải, nhiều GPU, chọn render node vaapi đã có).

## Bối cảnh

Hiện mọi khung GPU đều được tải về RAM ngay sau khi decode:

- `FFmpegDecoder::downloadFrame` (`src/Codec/Transcode.cpp`): `av_hwframe_transfer_data` (NV12/P010
  về RAM), rồi `toYuv420p` đổi NV12 → YUV420P bằng sws trên CPU.
- Một decoder cho mỗi nguồn (`MultiMediaSourceProcessor`, kế thừa `MediaSourceDecoder`). Khung giải
  mã được chia cho:
  - motion: ring `_ring` (512 phần tử), reader trên `_motion_poller`;
  - các `TranscodeProcessor`: `_transcode_queue` rồi `drainTranscodeFrames` trên `_transcode_poller`.
- `TranscodeProcessor::processFilteredVideoFrame`: `FFmpegFrameRateFilter` → (overlay: sws về kích
  thước đích + `TranscodeOverlay` trên CPU) → `FFmpegEncoder::inputFrame`.
- `FFmpegEncoder::inputFrame`: sws về kích thước/định dạng của encoder (hoặc `clone()`), rồi nvenc nhận
  YUV420P từ RAM; vaapi upload qua `hw_frames_ctx`.

Mỗi khung 1080p đi PCIe hai lần (tải về, đẩy lên lại) và qua hai lần sws trên CPU. Với motion trên
luồng Sub thì chi phí này nhỏ. Nó thành nút cổ chai khi transcode nhiều luồng Main trên GPU: CPU bận
copy và đổi định dạng trong khi NVDEC/NVENC còn rảnh.

## Mục tiêu và phạm vi

- Transcode **không overlay**, decoder và encoder **cùng backend, cùng device**: khung ở lại GPU
  từ decode tới encode, scale trên GPU.
- Chỉ tải về RAM khi có người cần RAM: motion, overlay, JPEG/snapshot, encoder CPU, encoder khác
  device. Mỗi khung tải về **tối đa một lần** dù nhiều người cần.
- Mọi lỗi trên đường GPU → quay về đường RAM hiện tại cho phiên đó, như cơ chế fallback đang có.
- Thứ tự: cuda trước (G2), vaapi sau (G3). qsv, videotoolbox, d3d11va: ngoài phạm vi.
- Ngoài phạm vi: overlay trên GPU (`overlay_cuda`/`overlay_vaapi`), tier transcode, ffmpeg dòng lệnh.

## Các bước

Mỗi bước một commit, diff nhỏ nhất có thể, không tách file hay API chỉ để test.

### G0 — đo trước khi làm (không sửa code)

- Trên máy đích có NVIDIA, chạy `ccu_test.sh` với nhiều transcode Main cùng lúc, bật perf
  (`CCU_PERF_SEC`). Làm tiếp chỉ khi hàm nóng nằm ở `av_hwframe_transfer_data`, `sws_scale`, memcpy
  của khung, hoặc CPU là giới hạn trước NVDEC/NVENC (`nvidia-smi dmon -s u`).
- Ghi lại: số luồng transcode tối đa hiện tại, CPU mỗi luồng, % dec/enc. Đây là mốc để so ở G4.
- Kiểm version FFmpeg trên máy đích và trong bản build:
  - FFmpeg 7.x: `scale_cuda` vừa đổi kích thước vừa đổi định dạng.
  - FFmpeg 4.4: `scale_cuda` có đổi kích thước (giữ định dạng). nvenc nhận NV12 CUDA trực tiếp nên
    có thể không cần đổi định dạng; **phải thử** `ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i <rtsp>
    -vf scale_cuda=1280:720 -c:v h264_nvenc -f null -` trên cả hai version.
  - Nếu 4.4 không chạy được thì đường GPU chỉ bật với `LIBAVFILTER_VERSION_INT` của 7.x; 4.4 giữ đường RAM.

### G1 — khung GPU tải về RAM khi cần (lazy download)

- `FFmpegDecoder` có chế độ giữ khung GPU: thay vì gọi `downloadFrame` ngay, `onDecode` nhận một
  `FFmpegFrame` còn trên GPU. `FFmpegFrame` thêm:
  - `bool isHw() const` (định dạng có `AV_PIX_FMT_FLAG_HWACCEL`);
  - `FFmpegFrame::Ptr toRam()`: tải về + đổi YUV420P một lần, lưu kết quả trong frame (mutex nhỏ,
    vì motion và transcode chạy trên hai poller khác nhau), lần gọi sau trả lại bản đã tải.
- Chế độ này chỉ bật khi decoder biết người nhận chịu được khung GPU (cờ do
  `MultiMediaSourceProcessor` đặt). Mặc định giữ hành vi cũ: mọi người nhận khác (`VideoStack`,
  `MediaSourceDecoder` khác, snapshot, C API `mk_decoder`) vẫn nhận RAM.
- Motion reader, overlay, `FFmpegEncoder` chạy CPU: gọi `toRam()` trước khi dùng.
- **Pool surface của decoder** — rủi ro lớn nhất. Khung GPU giữ surface của decoder đến khi được
  giải phóng; ring motion 512 phần tử, `_transcode_queue`, look-ahead của `FFmpegFrameRateFilter` giữ
  khung lâu hơn pool (mặc định khoảng 20 surface) → decoder hết surface và đứng. Cách xử lý:
  - Khung vào ring/hàng đợi là **bản copy GPU → GPU** sang pool riêng của processor
    (`av_hwframe_ctx_alloc` + `av_hwframe_transfer_data` giữa hai khung GPU: copy trong VRAM, không qua
    PCIe), trả surface của decoder ngay như hiện nay (`av_frame_unref(src)`).
  - Pool riêng có kích thước cố định; hết chỗ thì khung đó đi đường RAM (đếm vào thống kê), không
    chặn decoder.
  - Đặt `extra_hw_frames` của decoder chỉ khi đo thấy cần.
- Test (CPU, không cần GPU): decoder ở chế độ mặc định vẫn ra YUV420P như cũ; `toRam()` trên khung
  RAM trả chính nó; gọi `toRam()` từ hai thread cho cùng một kết quả.
- Test vaapi trên WSL (vgem + d3d12, xem `hw-codec.md`): decode vaapi, giữ 600 khung qua ring mà
  decoder không đứng.

### G2 — cuda: scale và encode trên GPU

- `FFmpegEncoder::inputFrame` nhận khung CUDA khi encoder là nvenc **cùng device** với khung:
  - Scale bằng một filter graph `scale_cuda=w:h[:format=nv12]` (`avfilter`, `hw_frames_ctx` từ
    khung vào), dựng lại khi kích thước vào đổi. Thay cho `FFmpegSws`.
  - Mở nvenc với `pix_fmt = AV_PIX_FMT_CUDA` và `hw_frames_ctx` của đầu ra filter, thay cho YUV420P từ RAM
    (`openHwEncoder` thêm nhánh cho đầu vào GPU).
  - Khung không phải CUDA, khác device, 10-bit (P010) mà codec đích là H.264 8-bit, hoặc filter
    lỗi → đường RAM (`toRam()` + sws như hiện tại). Lỗi lặp lại thì phiên đó ở hẳn đường RAM.
- **Cùng device:** với nhiều card (`hwcodec.device=cuda:0;1`) encoder phải mở trên card của decoder.
  `HwCodecManager::acquireEncode` thêm tham số device ưu tiên (tên device của phiên decode). Card đó
  hết budget/phiên thì lấy card khác và đi đường RAM, không chặn.
- **Định dạng đầu ra:** encoder hiện nhận YUV420P; với khung GPU, nvenc nhận NV12 trực tiếp, không
  cần đổi định dạng trên CPU.
- **Thread:** filter graph và encoder sống trên `_transcode_poller` của processor như hiện nay; context
  CUDA do hwcontext của FFmpeg push/pop, không cần làm thêm. Kiểm lại khi một card phục vụ nhiều poller.
- Test trên máy NVIDIA: transcode Main 1080p → 720p không overlay, so ảnh đầu ra với đường RAM (PSNR),
  đổi độ phân giải nguồn giữa chừng, nguồn HEVC 10-bit đi đường RAM, decoder và encoder khác card.

### G3 — vaapi

- Cùng cách với G2: `scale_vaapi`, `h264_vaapi` nhận `AV_PIX_FMT_VAAPI` (bỏ bước upload hiện có
  trong `FFmpegEncoder`). Kiểm được trên WSL (d3d12, chỉ h264; `hevc_vaapi` segfault trong driver).
- iGPU Intel decode qua vaapi nhưng encode qua qsv thì khác backend → đường RAM (qsv nhận khung vaapi
  qua `hwmap` là việc sau, chỉ làm nếu đo thấy cần).

### G4 — đo lại và cấu hình

- Khoá `hwcodec.keepFramesOnGpu` (mặc định `1` sau khi G4 đạt, `0` trong lúc thử): `0` = mọi thứ như
  trước G1.
- Thống kê: số khung đi đường GPU, số khung phải tải về RAM, số lần hết pool (thêm vào `getStatistic`
  cạnh `HwCodecLoad`).
- Chạy lại kịch bản G0: so số luồng tối đa và CPU mỗi luồng. Đạt khi CPU mỗi luồng transcode không
  overlay giảm rõ (mục tiêu ≥ 50%) và không có khung bị mất hơn đường RAM.
- Cập nhật `hw-codec.md`: đường đi của khung, khi nào còn tải về RAM, khoá mới.

## File dự kiến sửa

`src/Codec/Transcode.{h,cpp}` (FFmpegFrame, FFmpegDecoder, FFmpegEncoder), `src/Codec/HwCodec.{h,cpp}`
(acquireEncode theo device, openHwEncoder đầu vào GPU), `src/Processor/MultiMediaSourceProcessor.cpp`
(bật chế độ giữ khung GPU, copy vào pool riêng), `src/Transcode/TranscodeProcessor.cpp` (overlay gọi
`toRam()`), `src/Motion/` (gọi `toRam()`), `src/Common/config.{h,cpp}`, `tests/unit/codec/test_hw_codec.cpp`,
`docs/hw-codec.md`.

## Kiểm tra sau mỗi bước

- `cmake --build build -j2`; `ctest --test-dir build-test --output-on-failure -R unit.`.
- Build thêm với FFmpeg 7.x (prefix riêng, thư mục build riêng, `CMAKE_BUILD_TYPE` tên riêng để không
  ghi đè `release/linux/Debug`), vì G2/G3 dùng filter GPU khác nhau giữa 4.4 và 7.x.
- Phần cuda chỉ chạy được trên máy có NVIDIA; ghi việc còn lại vào `docs/pending-real-device-checks.md`.
