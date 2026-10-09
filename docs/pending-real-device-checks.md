# Việc còn phải chạy thật

Các plan dưới đây đã code xong và đã merge `develop`; file plan đã xoá ngày 2026-10-05 (xem lại
trong git: `git log --diff-filter=D -- docs/<tên>-plan.md`, rồi `git show <commit>^:docs/<tên>-plan.md`).
File này chỉ giữ phần chưa kiểm được vì thiếu thiết bị hoặc môi trường thật. Xong mục nào thì xoá dòng đó.

## Camera và loa thật

| Plan đã xoá | Việc còn lại |
|---|---|
| `psia-device-control-plan.md` (P6) | Camera ONVIF thật (hồi quy); camera bật PSIA ở chế độ `PSIA`; camera tắt ONVIF, chỉ còn PSIA, ở `AUTO`; restart vẫn giữ giao thức đã khoá; đổi `ONVIF` ↔ `PSIA` qua API; R3: URL luồng camera tự cấu hình có khớp URL PSIA sinh ra không. Harness: `E2E_CAMERA_*`, `E2E_CONTROL_PROTOCOL` của `tests/integration/e2e_real` |
| `isapi-speaker-control-plan.md` (E, I0, S3, S4) | Spike I0 trên thiết bị Hikvision thật; PTZ relative ISAPI; loa ISAPI/ONVIF chạy qua `SpeakerController` trong MediaServer (loa và file âm thanh đến từ ESC); plugin Bosch qua `runVendorOp` |
| `speaker-device-errors-clock-plan.md` | Loa Bosch thật: đặt giờ, lỗi API hãng 401; loa Hikvision ISAPI thật |
| `onvif-clock-skew-plan.md` (P7) | Camera thật chỉ bật WS-Security, lệch giờ. Đặt giờ cho loa chưa áp dụng cơ chế này |
| `onvif-http-digest-auth-plan.md` | Camera thật đặt chế độ `digest` (rủi ro camera từ chối WS-Security đi kèm); `/index/api/getStreamUrl` trên camera thật |
| `video-config-rule-fix-plan.md` | Checklist kịch bản `videorule` ([tests/integration/stress_vms/README.md](../tests/integration/stress_vms/README.md)) với ít nhất một camera ONVIF thật: xem stream đổi độ phân giải/bitrate thật và cách camera phản hồi `SetVideoEncoderConfiguration` |
| `mjpeg-ingest-transcode-plan.md` | Camera MJPEG thật, GPU encode, nhiều camera MJPEG cùng lúc; `sourceVcodec` trong thống kê camera end-to-end qua VMS |
| GPU budget + nhiều GPU (`hwcodec.decodeBudget/encodeBudget`, `cuda:0;1`, chọn render node vaapi) | Đo sức chứa NVDEC/NVENC và iGPU Intel để đặt budget mặc định cho máy đích; máy NVIDIA + Intel: vaapi chọn node `i915`/`xe` (log `GPU backend vaapi: using ...`), decode tràn từ cuda sang vaapi khi đặt budget; máy 2 card NVIDIA: chia tải theo `HwCodecLoad`, giới hạn 8 phiên NVENC GeForce đếm chung hai card; trích xuất overlay không lấn encoder live; build + `test_hw_codec` với FFmpeg 7.x, qsv trên iGPU driver `xe` |
| Transcode chuyển tầng trên GPU (`storage.tier_transcode_hwaccel`, [hw-codec.md](hw-codec.md) mục "Transcode khi chuyển tầng") | Máy NVIDIA: L1 cuda (`-hwaccel cuda` + `scale_cuda` + `h264_nvenc`/`hevc_nvenc`) ra file đạt `validate()` với `-preset p4 -rc vbr -maxrate`; nguồn 10-bit rơi về L2; đo CPU một segment 1080p → 720p ở L1/L2/L3; đã có 8 phiên NVENC live thì tier sang CPU mà trần NVENC học được (`HwCodecLoad`) không bị hạ; 2 card: job vào card ít tải, L1 không trộn card. qsv chưa chạy lần nào. Đã đạt trên WSL ngày 2026-10-09: E2E MinIO COLD H.264 bằng vaapi d3d12 (`h264_vaapi` L1, restore và phát lại 0 lỗi DTS) |

## E2E và môi trường

| Plan đã xoá | Việc còn lại |
|---|---|
| `e2e-real-run-fix-plan.md` | Chạy lại E2E ([e2e-test-plan.md](e2e-test-plan.md)) cho các vấn đề đã sửa mà chưa chạy lại: OVL-01..04 (ảnh evidence), RPL-13/14/22, APF-06 (xem tay chất lượng đoạn Sub), BKM-01/03a/03b, EXT-01/03, RPL-19, BKM-02, khởi động lại server có bản ghi rồi replay ngay (phải `503` rồi `200`, không `403`); thử `tfdt` sau khi chuyển Main↔Sub trên player web |
| `blocking-executors-phase2-plan.md` | E2E camera giả phần overlay OVL-01..09, HLS có overlay, thumbnail có overlay, sau khi xoá cache `www/overlay/` để đi qua nhánh ffmpeg resize; `startRecord` gọi tay |
| `disk-io-monitor-and-thread-fix-plan.md` (A bước 5, B4) | Trên db3: binary Release chạy `ccu_test.sh --suite quick`; xem `io_util_pct`; docker map `/dev/sdX` đúng tên trong `/proc/diskstats`; mock API nhận `DiskIo*`/`DiskReadOnly`; remount `ro` một mount record. Báo team API các `eventCode` mới (kể cả `*Recovered`) trước khi lên production |
| `token-auto-renew-fix-plan.md` | WebRTC chưa chạy thật (mọi build dir tắt `ENABLE_WEBRTC`, build cần thêm `ENABLE_RTPPROXY=ON`); checklist ở [user-author-manager-flow.md](user-author-manager-flow.md) mục 4. Lần phát đầu (không phải gia hạn) vẫn cache REJECT 600 s khi hook lỗi mạng/timeout |
| `mjpeg-ingest-transcode-plan.md` | WebRTC phát luồng MJPEG đã transcode |

`record-retention-forecast-plan.md` không còn việc gì: đã chạy thật cả tier ON lẫn OFF.
