# Đo hiệu năng GPU so với CPU (hwcodec)

So chi phí và thông lượng của từng tính năng có dùng GPU ([docs/hw-codec.md](../../../docs/hw-codec.md))
khi chạy CPU (`hwcodec.enable=0`) và khi chạy GPU (`hwcodec.enable=1`), trên cùng máy, cùng nguồn và cùng bậc tải.

## 1. Tính năng và phần việc chạy trên GPU

| Tính năng | Đường code | Bước lên GPU | Bước vẫn ở CPU | Kịch bản |
|---|---|---|---|---|
| Phát hiện chuyển động | `MediaSourceDecoder` → `FFmpegDecoder` | giải mã H.264/H.265 | tải frame về RAM + đổi YUV420P, phân tích motion | `motion-sub`, `motion-main` |
| Transcode live có overlay (watermark/mask) | luồng derived `.transcode.` | giải mã + mã hoá | vẽ overlay, sws | `transcode` |
| Trích xuất video có overlay | `FFmpegExtractor` (tiến trình `ffmpeg`) | mã hoá (`h264_vaapi`/`nvenc`/`qsv`) | giải mã, filter overlay | `extract` |
| Transcode khi chuyển tầng | `FfmpegTierMediaTransformer` (tiến trình `ffmpeg`) | L1: giải mã + scale + mã hoá; L2: mã hoá | L3: tất cả | `tier` |
| Motion MJPEG, VideoStack | dùng chung decoder của motion / `FFmpegDecoder` | giải mã | mã hoá JPEG / ghép khung | không đo riêng (phần giải mã giống `motion-*`) |
| Snapshot/thumbnail | `ffmpeg.snap` | cố ý giữ CPU (1 frame) | — | không đo |

## 2. Các tầng đo

- **L0 `raw`**: năng lực thô của chính binary `ffmpeg` đi kèm (n7.1.5, `release/linux/3rdpart/ffmpeg`), với
  tham số giống trong server: giải mã CPU `threads=auto` so với VAAPI `threads=1` (tải về RAM, đổi YUV420P);
  mã hoá libx264 `veryfast zerolatency main bf=0` so với `h264_vaapi`. Chạy 1 và 4 tiến trình song song. Cho biết
  trần fps của GPU, dùng để đặt `hwcodec.decodeBudget`/`encodeBudget`.
- **L1 `live`**: MediaServer thật, 16 camera giả (Main 1080p25 4 Mbps + Sub 360p25 512 kbps, GOP 2 s, phát bằng
  `rtsp_source.py`). Mỗi bậc tải: chờ đủ decoder/encoder → chờ ổn định 15 s → đo 40 s.
  - `idle`: 16 camera chỉ proxy, chưa giải mã. Đây là nền để trừ ra ở các bậc sau.
  - `motion-sub` / `motion-main`: bật motion trên 1, 4, 8, 16 camera, chạy trên luồng Sub (mặc định) hoặc Main.
  - `transcode`: overlay bật, 1, 2, 4, 8 viewer role viewer kéo `/media/live/<cam>/main.live.mp4`. Mỗi viewer
    trên một camera, tức là một luồng derived. Viewer chỉ `-c copy`, đếm frame nhận được.
  - `extract`: trích 60 s Main của camera 0 bằng JWT viewer (bắt buộc overlay), 1 job rồi 3 job đồng thời.
- **L2 `tier`**: gọi thẳng `FfmpegTierMediaTransformer::transform()` qua test
  `integration_tier_transcode_ffmpeg --gtest_filter=*BenchmarkFromEnv*` (bỏ qua khi không đặt
  `TIER_BENCH_SOURCE`) trên segment 60 s 1080p25 H.264 và H.265, ra H.264 720p15 1,5 Mbps và 360p10 0,5 Mbps,
  3 lần mỗi ca. Chạy đúng chuỗi L1/L2/L3 của code.

## 3. Số đo

| Số đo | Nguồn |
|---|---|
| CPU server (% một lõi) | `/proc/<pid>/stat` utime+stime trong cửa sổ đo; với `extract`/`tier`, CPU-giây của tiến trình con ffmpeg |
| CPU toàn máy | `/proc/stat` (gồm cả nguồn RTSP và viewer, chỉ để đối chiếu) |
| RSS | `/proc/<pid>/stat` |
| Tải GPU theo engine | bộ đếm Windows `\GPU Engine(*)\Utilization Percentage` qua `powershell.exe` (`gpu_sample.ps1`), cộng mọi tiến trình theo `engtype` (`3d`, `videodecode`, `videoprocessing`, `copy`) |
| Đường thực chạy | `getStatistic`: `VideoDecoder`/`HwVideoDecoder`, `VideoEncoder`/`HwVideoEncoder`, `HwCodecFallback`, `HwCodecLoad`; log `overlay re-encode on GPU backend`; `encoder_backend` của tier |
| Chất lượng phục vụ | log `decode thread start drop frame` / `encoder thread task is too more` (decoder/encoder không kịp, phải bỏ frame); fps viewer nhận được |
| Thời gian job | `extract`: từ `create` tới `ready`; `tier`: thời gian `transform()` |

## 4. Chạy

```bash
modprobe vgem                       # WSL2: tạo /dev/dri (mất sau khi khởi động lại WSL)
B=tests/performance/hwcodec_bench/hwbench.py
M=/path/media; R=/path/results; mkdir -p $R
python3 $B media $M
python3 $B raw $M --out $R/raw.json                     # ~5 phút
python3 $B tier --mode cpu --media $M --out $R/tier-cpu.json
python3 $B tier --mode gpu --media $M --out $R/tier-gpu.json
python3 $B live /path/work-cpu --mode cpu --media $M --out $R/live-cpu.json   # ~25 phút
python3 $B live /path/work-gpu --mode gpu --media $M --out $R/live-gpu.json
python3 $B report $R/*.json
```

Không chạy gì khác nặng CPU/GPU trong lúc đo (build, trình duyệt phát video): bộ đếm GPU của Windows tính
cả các tiến trình Windows khác.

Yêu cầu: `MediaServer` Debug (đọc `./api-config.json`, như harness E2E), build có `ENABLE_FFMPEG`,
`ENABLE_MOTION`; `integration_tier_transcode_ffmpeg` (build có `ENABLE_TESTS=ON`); `python3` có `cryptography`.

`p2_overlay_startup.py <work> <media> <out.json>`: viewer role viewer kéo `main.live.mp4` của camera i ở T+3·i s sau
khi server lên (camera có watermark + privacy mask từ đầu), ghi HTTP status và fps của luồng nhận được (derived
overlay 5 fps, luồng gốc 25 fps).

Kết quả đợt 2026-10-24 (WSL2, Iris Xe): `results-2026-10-24/` (`p2-*` tới `p7-*` là các lượt kiểm sau khi sửa
lỗi, xem mục 10 của báo cáo), báo cáo ở
[docs/hwcodec-benchmark-2026-10-24.md](../../../docs/hwcodec-benchmark-2026-10-24.md). Rig nhỏ:
`--cameras 4 --motion-steps 1,2,4 --transcode-steps 1,2,4`. Giới hạn phiên GPU: `--ini "hwcodec.maxDecodeSessions=2"`.

Những điều harness đã xử lý sẵn:

- Mỗi bậc chạy một MediaServer mới. Đổi motion/overlay giữa chừng không dựng lại luồng camera đang chạy.
- Trước mỗi lần khởi động, harness xoá `db/` và `record/record/<cam>/info.txt`. Cả hai được nạp trước `api-config.json`, nên nếu
  giữ lại, cấu hình của bậc trước sẽ chạy thêm khoảng 20 s ở đầu bậc sau.
- Không đo trước giây thứ `--warmup` (45). Khoảng 30 s đầu, luồng camera khởi động lại liên tục
  (`media source already existed: hls`); số lần đếm được nằm ở `restart_churn`.
- Viewer vào trước khi policy overlay của camera được nạp sẽ nhận luồng gốc, nên harness mở lại viewer (tối đa 4 lần)
  tới khi đủ encoder.
- Clip không có B-frame: `rtsp_source.py` đánh timestamp theo thứ tự giải mã. Nếu clip có B-frame, pts đi lùi và
  `FFmpegFrameRateFilter` bị dựng lại ở mỗi frame.
- Không dùng `pkill -f`/`pgrep -f` với tên script để dừng harness, vì nó khớp luôn shell đang chạy lệnh đó.

## 5. Giới hạn

- Bản Debug: libavcodec/libx264 vẫn là bản tối ưu (thư viện riêng), nhưng glue và phân tích motion chạy -O0.
  Hai chế độ cùng chịu phần cộng thêm này, nên chênh lệch tuyệt đối vẫn đúng còn tỉ lệ thì bị nén lại.
- WSL2 + Mesa d3d12: GPU đi qua lớp D3D12 của Windows (GPU-PV). Máy Linux có driver `iHD`/NVIDIA thật sẽ nhanh
  hơn và tốn CPU ít hơn ở bước chép frame. `hevc_vaapi` segfault trong driver nên probe tắt nó (mã hoá H.265 luôn
  ở CPU).
- Nguồn tổng hợp (testsrc2 + nhiễu) khó nén hơn cảnh camera thật; cùng nguồn cho cả hai chế độ.
