# Giải mã / mã hoá bằng GPU (hwcodec)

MediaServer giải mã video cho motion, transcode, video stack, snapshot trong tiến trình và mã hoá
lại cho transcode; trích xuất video có overlay mã hoá lại bằng tiến trình ffmpeg. Từ bản này các bước đó dùng GPU khi máy có, trên Linux, Windows, macOS và Android;
không có GPU, GPU lỗi hoặc hết phiên thì chạy CPU như trước, không cần cấu hình gì.

| Nền tảng | Backend (thứ tự mặc định) | Giải mã GPU | Mã hoá GPU |
|---|---|---|---|
| Linux | `cuda,vaapi,qsv` | NVDEC (cuda), VAAPI; Jetson: `h264_nvmpi`/`hevc_nvmpi` | nvenc, `*_vaapi`, `*_qsv` |
| Windows | `cuda,qsv,amf,d3d11va,dxva2` | NVDEC (cuda), D3D11VA, DXVA2 (mọi hãng) | nvenc, `*_qsv`, `*_amf` |
| macOS | `videotoolbox` | VideoToolbox | `*_videotoolbox` |
| Android | `mediacodec` | `h264_mediacodec`/`hevc_mediacodec` | `*_mediacodec` (FFmpeg ≥ 6.0) |

Mới kiểm thật trên Linux (CPU và VAAPI qua Mesa d3d12 trên WSL). Code cho Windows/macOS/Android chưa
được biên dịch hay chạy thử trên các nền tảng đó.

## 1. Cách hoạt động

- **Probe lúc khởi động** — `server/main.cpp` đẩy `HwCodecManager::probe()` sang
  `BlockingExecutors::ffmpeg()` (tạo context CUDA mất 100–500 ms, không chạy trên poller). Trong lúc
  probe chưa xong, mọi decoder/encoder mở trên CPU (không chờ). Phần thử chạy trong **tiến trình con**
  (xem mục "Probe bằng tiến trình con" bên dưới). Kết quả ghi một dòng log mỗi backend:
  `GPU backend vaapi: decode h264=1 h265=1, encode h264=1 h265=0`.
- **Mỗi backend một device context dùng chung** cho mọi decoder/encoder (không tạo context GPU mỗi
  phiên như `h264_cuvid`/`h264_qsv` cũ).
- **Giải mã** — `FFmpegDecoder` dùng decoder native `h264`/`hevc` + `hw_device_ctx` (hwaccel cuda,
  vaapi, videotoolbox, d3d11va, dxva2), chỉ cho video H264/H265. Frame GPU được tải về RAM và đổi sang
  YUV420P, nên motion, overlay, JPEG, sws, encoder nhận đúng định dạng như khi giải mã CPU. QSV không có
  hwaccel cho decoder native: Intel giải mã qua `vaapi` (Linux) hoặc `d3d11va` (Windows).
- **Decoder wrapper** — `h264_nvmpi`/`hevc_nvmpi` (bản FFmpeg cho Jetson) và `h264_mediacodec`/
  `hevc_mediacodec` (Android) gọi thẳng API của nền tảng, không qua hwaccel. Chúng được thử trước decoder
  native khi `hwcodec.enable=1` và `hwcodec.decode=1`, và luôn có đường quay về decoder native:
  - mở lỗi → mở decoder native;
  - lỗi `hwcodec.failThreshold` gói liên tiếp (không tính `AVERROR_INVALIDDATA`), hoặc 100 gói không ra
    frame nào → dựng lại decoder native, bỏ gói tới keyframe kế tiếp;
  - frame ra không phải YUV420P (mediacodec cho NV12) → đổi sang YUV420P.

  Motion/transcode (`MediaSourceDecoder`) và VideoStack nay dùng danh sách mặc định này. Trước đây chúng
  ép `{"h264","hevc"}` nên không bao giờ dùng phần cứng.
- **Mã hoá** — `FFmpegEncoder` mở encoder GPU của backend (`*_nvenc`, `*_vaapi`, `*_qsv`,
  `*_videotoolbox`, `*_amf`, `*_mediacodec`); mở lỗi thì trả phiên, dùng libx264/libx265 ngay. Giữ nguyên bất biến của transcode: SPS/PPS Annex-B in-band trên mỗi
  IDR (không global header), không B-frame, `requestKeyFrame()` cho ra IDR.
- **Danh sách decoder mặc định** nay duyệt xuôi: `*_nvmpi` → `*_mediacodec` → native (hwaccel nếu có
  GPU). So với bản cũ:

  | Bản cũ | Bản mới |
  |---|---|
  | `h264_nvmpi`, `hevc_nvmpi` | Giữ, kèm fallback lúc chạy |
  | `h264_cuvid`, `hevc_cuvid` | Native + hwaccel `cuda` (cùng NVDEC, device dùng chung) |
  | `h264_qsv`, `hevc_qsv` | Native + hwaccel `vaapi`/`d3d11va` (cùng phần cứng Intel) |
  | `h264_videotoolbox`, `hevc_videotoolbox` | FFmpeg không có decoder tên này (chỉ là encoder); macOS dùng hwaccel `videotoolbox` |
  | `libopenh264` | Bỏ: đứng sau `h264` native, mà native luôn có, nên chưa bao giờ được chọn |
  | — | Mới: `*_mediacodec` (Android), hwaccel `d3d11va`/`dxva2` (Windows) |

  `getCodecByName` cũng được sửa: trước đây danh sách `{"h264","hevc"}` trả decoder `h264` cho cả track
  H265. Trước đây danh sách bị duyệt ngược nên wrapper qsv/cuvid được thử trước
  decoder native. Đó là nguyên nhân luồng HEVC từng không giải mã được: `hevc_qsv` có sẵn trong FFmpeg của
  Ubuntu, `avcodec_open2` vẫn thành công vì session MFX chỉ được tạo ở gói đầu tiên, rồi mọi gói đều lỗi
  `Error initializing an internal MFX session: unsupported`. Decoder cũ chỉ fallback khi mở lỗi, nên không
  ra frame nào.

### Probe bằng tiến trình con

Driver GPU có thể segfault ngay trong `avcodec_send_frame` dù `avcodec_open2` thành công. Ví dụ
`hevc_vaapi` trên Mesa d3d12 (xem mục WSL2). Segfault không phải mã lỗi trả về nên fallback trong tiến
trình không cứu được. Vì vậy probe chạy phần thử thật trong tiến trình con (`runHwTestsInChildren`):

1. Với mỗi backend, tiến trình cha `fork()`. Tiến trình con trả signal handler về mặc định, tắt core
   dump (`prctl(PR_SET_DUMPABLE, 0)`; `core_pattern` dạng pipe như apport/systemd-coredump/WSL bỏ qua
   `RLIMIT_CORE`), đẩy stdout/stderr vào `/dev/null`, tắt log FFmpeg. Nó không dùng logger, config hay
   poller, vì lock của các luồng khác ở thời điểm fork không bao giờ được nhả trong tiến trình con.
2. Tiến trình con tạo device, rồi lần lượt `h264.decode`, `h264.encode`, `hevc.decode`, `hevc.encode`:
   - decode: libx264/libx265 mã hoá 5 frame 256×144, decoder native giải mã bằng GPU, cần ít nhất một
     frame tải về RAM được;
   - encode: mở encoder GPU, mã hoá 5 frame, gói đầu phải có SPS in-band.
   Mỗi phần thử xong là ghi kết quả qua pipe ngay.
3. Tiến trình cha đọc pipe, timeout 20 s (quá hạn thì `SIGKILL`), rồi `waitpid`. Phần không kịp ghi kết
   quả là phần làm tiến trình con chết. Phần đó bị tắt với lý do (`crashed with signal 11`,
   `hung, killed after 20000 ms`), các phần còn lại chạy tiếp trong một tiến trình con mới.
4. Chỉ backend có ít nhất một phần đạt mới được tạo device thật trong tiến trình cha.

Log ví dụ: `GPU backend vaapi: hevc.encode disabled, test process crashed with signal 11 (Segmentation fault)`.
Chỉ Linux probe bằng tiến trình con. Các nền tảng khác chỉ mở thử encoder ngay trong tiến trình, nên
không bắt được driver bị crash lúc mã hoá:
- Windows không có `fork`;
- macOS: CoreFoundation/VideoToolbox chủ động abort trong tiến trình con fork mà không `exec`;
- Android: tiến trình app (ART, JNI cho MediaCodec) không được fork.

Trên Linux, khi fork lỗi cũng quay về cách thử trong tiến trình này.
Probe mất thêm khoảng 1–3 s cho mỗi backend có GPU; backend không có GPU thì lỗi ngay ở bước tạo device.

### Fallback

| Khi nào | Xử lý |
|---|---|
| Probe chưa xong / không backend nào chạy | CPU, không log lỗi |
| Decoder GPU mở lỗi | mở lại cùng decoder trên CPU, `HwCodecFallback` +1 |
| GPU không nhận stream (profile, độ phân giải, bit depth) | FFmpeg chọn định dạng phần mềm, decoder trả phiên GPU, log `cannot decode this stream` |
| Decoder GPU lỗi `hwcodec.failThreshold` lần liên tiếp lúc chạy (không tính `AVERROR_INVALIDDATA`) | dựng lại context CPU ngay trên luồng giải mã, bỏ gói tới keyframe kế tiếp |
| Encoder GPU mở lỗi | libx264/libx265 ngay |
| Encoder GPU lỗi lúc chạy | bỏ frame đó, frame sau mở libx264/libx265 (frame đầu là IDR có SPS/PPS) |

### Circuit breaker và ngân sách phiên

- Mỗi phiên lỗi được tính một lần cho backend. Sau `hwcodec.failThreshold` phiên lỗi liên tiếp,
  backend bị bỏ qua `hwcodec.cooldownSec` giây (log `GPU backend cuda disabled for 300s`). Hết thời gian
  cho đúng một phiên thử; phiên thử chạy được thì bật lại (`works again, re-enabled`), lỗi thì khoá tiếp.
- `hwcodec.maxDecodeSessions` / `hwcodec.maxEncodeSessions` giới hạn số phiên GPU của **mỗi** device;
  vượt thì phiên mới thử backend kế tiếp rồi về CPU.
- `hwcodec.decodeBudget` / `hwcodec.encodeBudget` giới hạn theo **tải**, riêng cho từng backend
  (`cuda:720,vaapi:450`), áp cho mỗi device. Đơn vị là fps quy về 1080p: `rộng × cao × fps / (1920 × 1080)`,
  một luồng 1080p30 tốn 30, một luồng 360p15 tốn khoảng 1,7. Luồng không vừa phần còn lại thì sang backend
  kế tiếp, rồi CPU. Decoder mở trước khi có SPS được tính 1080p25, rồi sửa theo khung giải mã đầu tiên (và
  khi đổi độ phân giải). Hạ budget lúc đang chạy thì phiên cũ giữ nguyên, chỉ phiên mới bị chặn.
  Đặt budget theo thông lượng **H.264** của engine (NVDEC HEVC nhanh gần gấp đôi, đặt theo HEVC thì camera
  H.264 sẽ quá tải). Đo trên máy thật: chạy song song N lệnh `ffmpeg -hwaccel cuda|vaapi -i <rtsp 1080p30>
  -f null -`, tăng N tới khi fps < 30, theo dõi `nvidia-smi dmon -s u` (cột dec/enc) và `intel_gpu_top`;
  budget ≈ N × 30. NVDEC/NVENC Ampere/Ada khoảng 750–900 fps 1080p H.264 mỗi engine; iGPU Intel phải đo.
- Encoder mở lỗi trong khi các encoder khác của backend vẫn chạy được coi là chạm giới hạn phiên của
  thiết bị (card NVIDIA dòng consumer chỉ cho vài phiên NVENC, tính chung **mọi card** của hệ thống), không
  tính vào breaker: số encoder GPU của backend (cộng mọi device) bị chặn ở mức đang chạy trong `cooldownSec`
  giây.

### Nhiều GPU

- **Nhiều card một backend:** `hwcodec.device=cuda:0;1` (ngăn device trong một backend bằng `;`). Mỗi card
  được probe riêng, có breaker và budget riêng; phiên mới vào card có tỉ lệ tải/budget thấp nhất (không đặt
  budget thì card tải thấp nhất). Chỉ số card theo thứ tự của CUDA, khác thứ tự `nvidia-smi`: đặt
  `CUDA_DEVICE_ORDER=PCI_BUS_ID` trong môi trường chạy MediaServer để hai bên khớp. Luôn viết tiền tố
  `cuda:`; mục không có tiền tố áp cho mọi backend.
- **NVIDIA + iGPU Intel:** khi `hwcodec.device` không có mục `vaapi`, MediaServer tự chọn render node theo
  driver trong `/sys/class/drm/renderD*/device/driver`: ưu tiên `i915`/`xe`, rồi `amdgpu`, rồi driver khác,
  **không bao giờ** node `nvidia` (log `GPU backend vaapi: using /dev/dri/renderD129 (i915)`). Máy chỉ có node
  NVIDIA thì bỏ vaapi (cuda đã phủ card đó, kể cả khi có `nvidia-vaapi-driver`). FFmpeg tự chọn thì lấy node
  đầu tiên mở được, mà số node theo thứ tự nạp driver nên có thể là card NVIDIA.
- Để iGPU nhận phần tràn decode từ cuda, đặt `hwcodec.decodeBudget` cho cuda; không có budget thì NVDEC
  không bao giờ báo đầy và mọi decoder đều vào cuda.
- `vaapi` và `qsv` trên Linux là **cùng một iGPU**: budget của hai backend tính trùng. Đặt `encodeBudget`
  của qsv nhỏ hoặc bỏ qsv khỏi `hwcodec.backends`. Với qsv, `hwcodec.device` là tên implementation
  (`hw`, `auto`), không phải đường dẫn `/dev/dri`; FFmpeg tự tìm iGPU Intel cho qsv.

## 2. Cấu hình

Mặc định đã an toàn (production sinh config từ default trong code):

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `hwcodec.enable` | `1` | `0` = luôn CPU, không tạo device GPU (đọc lúc probe và mỗi lần mở phiên) |
| `hwcodec.backends` | theo nền tảng (bảng đầu trang) | Thứ tự thử; đổi nóng được thứ tự/bỏ backend, thêm backend mới cần khởi động lại |
| `hwcodec.device` | rỗng | `cuda:1,vaapi:/dev/dri/renderD129`; nhiều card: `cuda:0;1`; mục không có `backend:` áp cho mọi backend; rỗng = device mặc định của FFmpeg, riêng vaapi trên Linux tự chọn node Intel/AMD (mục "Nhiều GPU") |
| `hwcodec.decode` | `1` | Dùng GPU để giải mã |
| `hwcodec.encode` | `1` | Dùng GPU để mã hoá transcode |
| `hwcodec.maxDecodeSessions` | `0` | Số phiên giải mã GPU tối đa, `0` = không giới hạn |
| `hwcodec.maxEncodeSessions` | `0` | Số phiên mã hoá GPU tối đa, `0` = không giới hạn (tự học giới hạn khi mở lỗi) |
| `hwcodec.decodeBudget` | rỗng | Tải giải mã tối đa mỗi device, `cuda:720,vaapi:450` (fps quy 1080p); rỗng/thiếu backend/`<= 0` = không giới hạn |
| `hwcodec.encodeBudget` | rỗng | Như trên cho mã hoá |
| `hwcodec.offlineCost` | `60` | Tải tính cho một việc mã hoá/giải mã chạy nhanh hơn thời gian thực (trích xuất overlay, transcode khi chuyển tầng); chỉ có tác dụng khi có budget |
| `hwcodec.offlineHeadroom` | `0.8` | Việc offline chỉ được nhận khi tải sau đó không vượt `budget × hệ số` này, chừa phần còn lại cho luồng live |
| `hwcodec.failThreshold` | `3` | Lỗi liên tiếp để một phiên về CPU, và số phiên lỗi để khoá backend |
| `hwcodec.cooldownSec` | `300` | Thời gian khoá backend / giữ giới hạn phiên đã học |
| `general.check_nvidia_dev` | `1` | Giữ nghĩa cũ: `0` = không bao giờ dùng `cuda` |

## 3. Theo dõi

`/index/api/getStatistic` và `/media/api/getStatistic` có thêm:

| Trường | Ý nghĩa |
|---|---|
| `VideoDecoder`, `VideoEncoder` | Số decoder/encoder video đang chạy (CPU + GPU) |
| `HwVideoDecoder`, `HwVideoEncoder` | Số phiên đang chạy trên GPU |
| `HwCodecFallback` | Số decoder/encoder đã bắt đầu trên GPU rồi về CPU kể từ khi khởi động |
| `HwCodecBackends` | Backend probe thành công, ví dụ `cuda`; rỗng = toàn bộ chạy CPU |
| `HwCodecLoad` | Tải từng device, ví dụ `cuda:0 dec=120/720 enc=30,vaapi:/dev/dri/renderD129 dec=12`; số sau `/` là budget khi có đặt |

Mỗi phiên ghi một dòng log: `Open the decoder successfully:h264 on GPU cuda`,
`FFmpegEncoder opened: h264_nvenc (GPU cuda) 1280x720 @15fps ...`, hoặc lý do về CPU.

## 4. Triển khai

FFmpeg 4.4 của Ubuntu 22.04 (libavcodec 58) đã có sẵn nvenc/nvdec, vaapi và qsv; driver phải có ở
máy chạy:

- **NVIDIA (Docker)** — NVIDIA Container Toolkit, chạy với `--gpus all` và
  `NVIDIA_DRIVER_CAPABILITIES=compute,video,utility` (thiếu `video` thì không có `libnvcuvid`/
  `libnvidia-encode`, probe báo `cuda unavailable` hoặc `h264_nvenc open failed`).
- **Intel/AMD (VAAPI)** — mount `/dev/dri` (`--device /dev/dri`), cài driver VA (`intel-media-va-driver`
  hoặc `i965-va-driver`, `mesa-va-drivers` cho AMD), user chạy MediaServer thuộc group `render`
  (hoặc `video`). Kiểm tra bằng `vainfo`.
- **Intel QSV (chỉ mã hoá)** — như VAAPI + `libmfx1` (FFmpeg 4.4) hoặc `libvpl2` (FFmpeg 7.x).

docker-compose ví dụ cho NVIDIA:

```yaml
services:
  mediaserver:
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: all
              capabilities: [gpu, video, compute, utility]
    environment:
      - NVIDIA_DRIVER_CAPABILITIES=compute,video,utility
```

Intel/AMD:

```yaml
services:
  mediaserver:
    devices:
      - /dev/dri:/dev/dri
    group_add:
      - render
```

### Kiểm tra GPU và driver

Các lệnh dưới đây viết cho Ubuntu 22.04 (bản chạy production). Với Docker, kiểm tra ở **máy host**
trước, sau đó chạy lại các lệnh ở bước 5 **bên trong container**.

**1. Máy có GPU gì**

```bash
lspci -nn | grep -Ei 'vga|3d|display'
# NVIDIA ... [10de:xxxx]  -> làm theo mục NVIDIA
# Intel  ... [8086:xxxx]  -> VAAPI (giải mã + mã hoá), QSV (mã hoá)
# AMD    ... [1002:xxxx]  -> VAAPI
```

Trên máy ảo thường không thấy GPU thật, nên MediaServer chạy CPU. WSL2 xem mục dưới.

**WSL2 (máy dev Windows)**

WSL2 không đưa GPU thật vào Linux: `lspci` chỉ thấy `3D controller: Microsoft Corporation Device 008e`,
GPU nằm sau `/dev/dxg`. Driver Intel/NVIDIA vẫn là driver của Windows. VAAPI chạy được qua driver
`d3d12` của Mesa (gói `mesa-va-drivers` ≥ 23, có file `/usr/lib/x86_64-linux-gnu/dri/d3d12_drv_video.so`),
nhưng cần node `/dev/dri`. Node này do module `vgem` tạo; nếu kernel WSL build vgem dạng module
(`zcat /proc/config.gz | grep VGEM` ra `=m`) thì phải nạp tay:

```bash
sudo modprobe vgem                       # tạo /dev/dri/card0 + renderD128, mất sau khi khởi động lại WSL
LIBVA_DRIVER_NAME=d3d12 vainfo --display drm --device /dev/dri/renderD128
# Driver version: Mesa Gallium driver ... for D3D12 (Intel(R) Iris(R) Xe Graphics)
```

Chạy MediaServer hoặc unit test với `LIBVA_DRIVER_NAME=d3d12`. Chỉ có backend `vaapi`; `qsv` và driver
`iHD` cần GPU Intel thật nên không chạy trên WSL. Kết quả đo trên máy dev Iris Xe (Mesa 23.2.1, FFmpeg 4.4):

| Việc | Kết quả |
|---|---|
| Giải mã H264/HEVC (hwaccel vaapi) | Chạy được |
| Mã hoá `h264_vaapi` | Chạy được, có IDR + SPS/PPS |
| Mã hoá `hevc_vaapi` | **Segfault** trong `vaEndPicture` của `d3d12_drv_video.so` ở frame đầu; mở encoder thì không lỗi |

Probe bằng tiến trình con phát hiện lỗi này và chỉ tắt `hevc.encode` (transcode H265 dùng libx265);
giải mã H264/H265 và mã hoá H264 vẫn chạy GPU. Chạy thử đường GPU thật bằng
`LIBVA_DRIVER_NAME=d3d12 unit_codec --gtest_filter='*RealProbe*'`.

**2. NVIDIA**

Kiểm tra:

```bash
nvidia-smi                                   # thấy tên card + phiên bản driver là driver đã chạy
ls -l /dev/nvidia*                           # /dev/nvidia0, /dev/nvidiactl, /dev/nvidia-uvm
ldconfig -p | grep -E 'libcuda\.so|libnvcuvid|libnvidia-encode'   # đủ 3 thư viện: cuda, NVDEC, NVENC
nvidia-smi -q | grep -iA3 'encoder stats'    # card có NVENC (một số card datacenter không có)
```

Cài driver nếu chưa có:

```bash
sudo ubuntu-drivers list                     # các driver phù hợp với card
sudo ubuntu-drivers install                  # hoặc chọn bản cụ thể: sudo apt install nvidia-driver-550
# Máy server không có màn hình: bản headless kèm thư viện encode/decode
# sudo apt install nvidia-headless-550-server libnvidia-encode-550-server libnvidia-decode-550-server
sudo reboot
```

Driver quá cũ so với FFmpeg sẽ làm `h264_nvenc` mở lỗi ("Driver does not support the required nvenc
API version"): cập nhật driver lên bản mới hơn.

Chạy trong Docker cần thêm NVIDIA Container Toolkit trên host:

```bash
curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
  | sudo gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
  | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
  | sudo tee /etc/apt/sources.list.d/nvidia-container-toolkit.list
sudo apt update && sudo apt install -y nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker

# Thử: phải in được bảng nvidia-smi
docker run --rm --gpus all -e NVIDIA_DRIVER_CAPABILITIES=compute,video,utility ubuntu:22.04 nvidia-smi
```

Trong container không cài driver NVIDIA. Toolkit gắn thư viện của host vào container, và chỉ gắn
`libnvcuvid`/`libnvidia-encode` khi `NVIDIA_DRIVER_CAPABILITIES` có `video`.

**3. Intel / AMD (VAAPI)**

Kiểm tra:

```bash
ls -l /dev/dri                               # cần có renderD128 (card thứ hai là renderD129)
sudo apt install -y vainfo
vainfo --display drm --device /dev/dri/renderD128
#   VAProfileH264Main  : VAEntrypointVLD        -> giải mã H264 được
#   VAProfileHEVCMain  : VAEntrypointVLD        -> giải mã H265 được
#   VAProfileH264Main  : VAEntrypointEncSlice   -> mã hoá H264 được (EncSliceLP cũng được)
id                                           # user chạy MediaServer phải thuộc group render (hoặc video)
```

`vainfo` báo lỗi hoặc thiếu dòng `EncSlice` thì cài driver:

```bash
# Intel Broadwell (Gen8) trở lên. Bản non-free (repo multiverse) mới có đủ mã hoá
sudo apt install -y intel-media-va-driver-non-free
# Intel đời cũ (Haswell trở về trước)
sudo apt install -y i965-va-driver-shaders
# AMD
sudo apt install -y mesa-va-drivers

sudo usermod -aG render "$USER"              # đăng nhập lại để có hiệu lực
```

Máy có cả hai driver mà `vainfo` chọn sai thì ép tên driver bằng `LIBVA_DRIVER_NAME=iHD` (Intel mới),
`i965` (Intel cũ) hoặc `radeonsi` (AMD). Docker: cài cùng gói driver VA **trong image**, mount
`/dev/dri` và `group_add: [render]` như ví dụ ở trên. Nếu GID của group `render` trong container khác
host, dùng số GID của host (`getent group render`).

**4. Intel QSV (chỉ mã hoá)**

QSV dùng chung driver VAAPI ở bước 3, thêm runtime Media SDK:

```bash
ldconfig -p | grep -E 'libmfx|libvpl'
sudo apt install -y libmfx1                  # FFmpeg 4.4 (Ubuntu 22.04)
# Intel Gen12+ (Tiger Lake, Arc...) với FFmpeg 7.x: sudo apt install -y libvpl2 libmfx-gen1.2
```

**5. FFmpeg dùng được GPU chưa**

MediaServer link cùng thư viện FFmpeg hệ thống với lệnh `ffmpeg`. Các lệnh dưới chạy được thì
MediaServer cũng dùng được GPU đó.

```bash
ffmpeg -hide_banner -hwaccels                               # có cuda / vaapi / qsv
ffmpeg -hide_banner -encoders | grep -E 'nvenc|vaapi|qsv'   # có h264_nvenc, h264_vaapi, ...

# Mã hoá thử 3 giây
ffmpeg -hide_banner -f lavfi -i testsrc2=size=1280x720:rate=25 -t 3 -c:v h264_nvenc -f null -
ffmpeg -hide_banner -vaapi_device /dev/dri/renderD128 -f lavfi -i testsrc2=size=1280x720:rate=25 -t 3 \
       -vf format=nv12,hwupload -c:v h264_vaapi -f null -
ffmpeg -hide_banner -f lavfi -i testsrc2=size=1280x720:rate=25 -t 3 -vf format=nv12 -c:v h264_qsv -f null -

# Giải mã thử một camera hoặc file
ffmpeg -hide_banner -hwaccel cuda -i rtsp://<camera> -t 5 -f null -
ffmpeg -hide_banner -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i rtsp://<camera> -t 5 -f null -
```

**6. Xác nhận trên MediaServer**

```bash
grep -E 'GPU backend|No GPU decode/encode' <log-dir>/*.log | tail
curl -s 'http://127.0.0.1/index/api/getStatistic?secret=<secret>' | grep -E 'HwCodec|HwVideo'
```

| Log probe | Nguyên nhân thường gặp |
|---|---|
| `GPU backend cuda unavailable: ...` | Chưa có driver, không có `/dev/nvidia*`, hoặc container chạy thiếu `--gpus` |
| `h264_nvenc open failed: ...` | Thiếu `libnvidia-encode` (`NVIDIA_DRIVER_CAPABILITIES` thiếu `video`), driver quá cũ, card không có NVENC |
| `GPU backend vaapi unavailable: Generic error in an external library` | Không có `/dev/dri`, user không thuộc group `render`, hoặc chưa cài driver VA |
| `h264_vaapi open failed: ...` | Driver VA không có mã hoá (thường là `intel-media-va-driver` bản free): cài bản `-non-free` |
| `GPU backend qsv unavailable: ...` | Thiếu `libmfx1`/`libvpl2` hoặc driver VAAPI Intel |
| `No GPU decode/encode backend in use` | Không backend nào dùng được: MediaServer chạy CPU như trước |

Sửa xong driver thì **khởi động lại MediaServer**, vì probe chỉ chạy một lần lúc khởi động.

### Trích xuất video có overlay

Trích xuất của camera có watermark/privacy mask (`ffmpeg.extract_overlay`) phải giải mã, vẽ overlay rồi
mã hoá lại. Bước mã hoá dùng GPU: khi template còn nguyên phần `-c:v libx264 -preset veryfast -pix_fmt
yuv420p` mặc định, `FFmpegExtractor` xin một phiên mã hoá H264 **offline** của `HwCodecManager` (tính vào
`hwcodec.maxEncodeSessions`; khi có `hwcodec.encodeBudget` thì tính `hwcodec.offlineCost` và chỉ nhận khi
còn dưới `budget × hwcodec.offlineHeadroom`; giữ đến khi tiến trình ffmpeg xong) và thay phần đó theo backend:

| Backend | Tham số mã hoá |
|---|---|
| cuda | `-c:v h264_nvenc -preset fast -rc vbr -cq 23 -b:v 0 -pix_fmt yuv420p` (+ `-gpu N` khi `hwcodec.device` là số) |
| vaapi | `-init_hw_device vaapi=gpu[:device] -filter_hw_device gpu`, cuối filter `format=nv12,hwupload`, `-c:v h264_vaapi -qp 23` |
| qsv | `-c:v h264_qsv -preset veryfast -global_quality 23 -pix_fmt nv12` |
| amf | `-c:v h264_amf -rc cqp -qp_i 23 -qp_p 23 -pix_fmt nv12` |
| videotoolbox | `-c:v h264_videotoolbox -b:v 6M -pix_fmt nv12` |

Filter overlay và bước giải mã vẫn chạy CPU. Probe kiểm libavcodec liên kết vào MediaServer, còn đây là
binary `ffmpeg.bin`, có thể không có encoder đó. Nếu tiến trình ffmpeg chạy GPU thoát lỗi (trừ khi bị huỷ
hoặc quá thời gian), job chạy lại ngay bằng lệnh libx264 (log `GPU encoder ... failed ..., retrying with
libx264`, tăng `HwCodecFallback`). Backend đó không dùng cho trích xuất trong `hwcodec.cooldownSec`;
breaker của pipeline trong tiến trình không bị ảnh hưởng. Template `extract_overlay` đã sửa phần encoder
thì luôn chạy đúng như template.

### Transcode khi chuyển tầng

Giảm chất lượng khi chuyển tầng (`storage.tier_transcode_enabled`, xem
[tier-storage-quality-reduction-architecture.md](tier-storage-quality-reduction-architecture.md)) chạy
binary `storage.tier_transcode_ffmpeg_bin` cho từng segment. Khi `storage.tier_transcode_hwaccel=1`
(mặc định), mỗi segment xin một phiên mã hoá **offline** của `HwCodecManager`
(`acquireEncodeOffline`, cùng `hwcodec.offlineCost`/`offlineHeadroom` với trích xuất overlay); nguồn
H.264/H.265 trên cuda/vaapi xin thêm một phiên giải mã offline (`acquireDecodeOffline`). Thử lần lượt:

| Mức | Khi nào | Lệnh |
|---|---|---|
| L1 | có cả phiên giải mã lẫn mã hoá, cùng backend và **cùng card** | cuda: `-hwaccel cuda -hwaccel_output_format cuda [-hwaccel_device N]`, filter `fps=F,scale_cuda=W:H`; vaapi: `-hwaccel vaapi -hwaccel_output_format vaapi -hwaccel_device <node>`, filter `fps=F,scale_vaapi=w=W:h=H:format=nv12` |
| L2 | có phiên mã hoá | filter CPU như cũ; cuda `h264_nvenc -preset p4 -rc vbr -b:v X -maxrate X -bufsize 2X -pix_fmt yuv420p [-gpu N]`; vaapi `-init_hw_device vaapi=gpu:<node> -filter_hw_device gpu`, cuối filter `format=nv12,hwupload`, `-rc_mode VBR -b:v X -maxrate X`; qsv `-preset veryfast -b:v X -maxrate X -pix_fmt nv12` |
| L3 | còn lại | các encoder CPU đã qua probe trong `storage.tier_transcode_h264_encoders`/`h265_encoders` (mặc định `libx264`/`libx265`) |

L1 lỗi thì chạy lại ngay ở L2 cùng phiên, và L1 của backend đó nghỉ `hwcodec.cooldownSec`. L2 lỗi thì
sang CPU, backend đó nghỉ `hwcodec.cooldownSec` cho tier, `HwCodecFallback` tăng. Breaker và giới hạn
phiên NVENC học được của pipeline trong tiến trình không bị ảnh hưởng. `scale_cuda` của FFmpeg 4.4
không đổi định dạng điểm ảnh, nên nguồn 10-bit với `h264_nvenc` luôn rơi về L2.

### ffmpeg ngoài tiến trình khác

`ffmpeg.cmd` / `ffmpeg.snap` / `ffmpeg.snap_overlay` (thumbnail bản ghi) chạy tiến trình ffmpeg riêng,
không qua `HwCodecManager`.

**Thumbnail cố ý để chạy CPU.** Mỗi lần trích thumbnail là một tiến trình ffmpeg mới chỉ giải mã đến
frame cần lấy và xuất đúng 1 frame JPEG. Dùng GPU thì mỗi lần phải tạo device context mới (CUDA
100–500 ms, VAAPI/QSV vài chục ms) và mở decoder GPU. Chi phí đó lớn hơn phần giải mã tiết kiệm được.
Frame còn phải tải từ GPU về RAM để mã hoá MJPEG và vẽ overlay, và mỗi lần lại chiếm một phiên
NVDEC/VRAM. Vì vậy `snap`/`snap_overlay` mặc định không có `-hwaccel`.
Thời gian của thumbnail chủ yếu nằm ở việc tìm và đọc segment (có thể phải restore từ tier), không ở
giải mã. Ngược lại, trích xuất có overlay mã hoá lại cả đoạn video, nên mới dùng GPU (mục trên).

Muốn thử GPU cho thumbnail thì vẫn có thể sửa template, ví dụ:

```ini
[ffmpeg]
snap=%s -hwaccel auto -ss %s -i %s -y -f mjpeg -frames:v 1 -an %s
```

## 5. Giới hạn và rủi ro

- Đã kiểm thật: Linux CPU, và VAAPI qua Mesa d3d12 trên WSL2 (Iris Xe), bằng
  `LIBVA_DRIVER_NAME=d3d12 unit_codec --gtest_filter='*RealProbe*'`. Chưa kiểm: NVIDIA và Intel `iHD` trên
  Linux thật. Code cho Windows (d3d11va/dxva2/amf), macOS (videotoolbox) và Android (mediacodec) chưa được
  biên dịch hay chạy trên các nền tảng đó.
- Option encoder cho videotoolbox/amf/mediacodec viết theo tài liệu FFmpeg, chưa kiểm SPS/PPS in-band và
  IDR khi có `pict_type` I trên máy thật. `*_mediacodec` encoder cần FFmpeg ≥ 6.0. MediaCodec cần JavaVM
  (`av_jni_set_java_vm`) do SDK Android đặt trước.
- Decoder wrapper (nvmpi, mediacodec) không nằm trong ngân sách phiên hay breaker của `HwCodecManager`.
  Mỗi decoder tự quay về native khi lỗi.
- Card NVIDIA consumer giới hạn số phiên NVENC (thường 5–8); mỗi luồng NVDEC giữ một pool surface trong
  VRAM nên VRAM giới hạn số luồng giải mã — đặt `hwcodec.maxDecodeSessions` khi chạy hàng trăm camera.
- Preset nvenc dùng `llhp` (có ở cả FFmpeg 4.4 và 7.x).
- Khi GPU không nhận một stream, decoder về CPU nhưng giữ `threads=1`.
- Frame vẫn đi qua RAM (tải về rồi đổi sang YUV420P). Scale trên GPU trước khi tải về cho motion
  (`scale_cuda`/`scale_vaapi`) và zero-copy giải mã → mã hoá khi không có overlay chưa làm.
- `api/` (C ABI) không chạy probe nên decoder tạo qua `mk_decoder_*` không dùng hwaccel. Decoder wrapper
  (nvmpi, mediacodec) thì vẫn được thử, vì chỉ phụ thuộc config.
