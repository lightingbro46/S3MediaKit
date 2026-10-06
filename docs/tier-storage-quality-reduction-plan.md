# Kế hoạch giảm chất lượng khi chuyển dữ liệu giữa các tier

## 1. Mục tiêu

- Bổ sung bước transcoding tùy chọn trước khi chuyển dữ liệu sang WARM/COLD.
- Áp dụng cho mọi trigger hiện có: lịch retention, áp lực dung lượng và yêu cầu chuyển tier từ API, khi tier đích có profile chất lượng.
- Cho phép FE bật/tắt toàn cục trong policy và cấu hình profile riêng cho từng tier.
- Video được bảo vệ/evidence vẫn được chuyển tier nhưng giữ nguyên chất lượng.
- Chỉ áp dụng cho những lần chuyển mới; không backfill dữ liệu đã nằm ở WARM/COLD.
- Dùng `ffmpeg` theo mô hình file-to-file. Tự chọn NVENC, QSV rồi software fallback; không dùng pipeline live `TranscodeProcessor`.

## 2. API, cấu hình và dữ liệu

### 2.1. Policy API

Mở rộng `PolicyAdvancedRules`:

```json
{
  "enable_tier_quality_reduction": false
}
```

Mở rộng mỗi `PolicyTierConfig`:

```json
{
  "quality_reduction": {
    "enabled": true,
    "video_codec": "H264",
    "max_width": 1280,
    "max_height": 720,
    "max_fps": 15,
    "max_video_bitrate_kbps": 1200
  }
}
```

Quy tắc validation:

- Chỉ cho phép bật ở WARM/COLD.
- Codec chỉ nhận `H264` hoặc `H265`.
- `max_width` và `max_height` phải cùng được cấu hình; giá trị `0` nghĩa là không giới hạn.
- Phải có ít nhất một giới hạn dương.
- Không upscale, không tăng FPS hoặc bitrate so với nguồn.
- Audio luôn stream-copy trong phiên bản đầu tiên.
- Không nhận encoder backend hoặc tham số `ffmpeg` thô từ FE.

Mở rộng API capability của storage:

- Trả `qualityReduction.supported`, trạng thái operational, codec hỗ trợ và backend khả dụng.
- API create/update policy từ chối bật profile nếu node không có backend encode phù hợp.
- Policy detail trả đầy đủ profile để FE dựng toggle và form cấu hình.

### 2.2. Database

Tạo migration kế tiếp `22_tier_quality_reduction.sql`:

- Thêm vào `tiering_jobs`: `operation`, `quality_profile_json`, `output_bytes`, `transcoded_segment_count`, `preserved_segment_count`.
- Tạo `tiering_job_segments` để lưu trạng thái bền vững của từng segment: nguồn, khoảng thời gian, kích thước đầu vào/đầu ra, operation, encoder, trạng thái và lỗi.
- Tạo outbox cập nhật TimeFile, gồm `segment_path`, kích thước mới, trạng thái, số lần retry và lỗi gần nhất.
- Job cũ mặc định là `COPY_MOVE`, profile rỗng và tiếp tục chạy như hiện tại.
- Mọi thay đổi trạng thái có ghi dữ liệu phải dùng `beginWriteTxn()`.

## 3. Luồng thực thi

### 3.1. Tạo job

- Snapshot profile của tier đích vào job; thay đổi policy sau đó không làm đổi hành vi của job đang chờ.
- Nếu công tắc tổng hoặc profile của tier đích tắt, tạo `COPY_MOVE`.
- Dùng logic chia range hiện có để tách theo protected-video:
  - Phần được bảo vệ tạo `COPY_MOVE`.
  - Phần còn lại tạo `TRANSCODE_MOVE`.
- External move chỉ transcode khi target pool ánh xạ được tới một tier có profile chất lượng.

### 3.2. Media transformer

Thêm `TierMediaTransformer` trong `manager/Local` với các trách nhiệm:

- Probe media, xác định segment có thật sự cần transcode và chọn encoder.
- Thứ tự backend H.264: `h264_nvenc` → `h264_qsv` → `libx264`.
- Thứ tự backend H.265: `hevc_nvenc` → `hevc_qsv` → `libx265`.
- Xác định capability bằng encode smoke-test ngắn, không chỉ dựa vào kết quả `ffmpeg -encoders`.
- Nếu hardware encoder lỗi khi chạy, thử backend tiếp theo trong cùng attempt.
- Scale giữ aspect ratio và kích thước chẵn; FPS chỉ giảm.
- Ghi output vào staging file riêng theo job/segment và giữ nguyên storage key khi chuyển sang tier đích.
- Trước khi upload/copy, validate codec, duration, resolution, FPS, bitrate và file khác rỗng.

Thêm process runner an toàn trong `manager/Common`:

- Nhận argv dạng vector, không thực thi qua shell.
- Có timeout, cancellation, giới hạn log và cleanup staging.
- Không nhận tham số dòng lệnh trực tiếp từ FE.

Thêm các operational config:

- Bật/tắt transcoding ở mức hệ thống.
- Đường dẫn `ffmpeg` và `ffprobe`.
- Staging directory và process timeout.
- Thứ tự encoder preference.
- Số encode đồng thời tối đa.

### 3.3. Threading và trạng thái

- Thêm `BlockingExecutors::storage()` dành cho disk, S3, SQLite và tiến trình transcoding của tier storage.
- `WorkThreadPool` chỉ giữ timer và công việc điều phối; không thực hiện transfer hoặc chờ process.
- Giữ giới hạn đồng thời của dispatcher hiện có và bổ sung semaphore riêng cho encode để bảo vệ CPU/GPU.
- Không giữ cache mutex trong lúc thực hiện I/O hoặc chờ `ffmpeg`.

Trạng thái segment:

```text
PENDING -> TRANSFORMING -> STORED -> COMMITTED
                       \-> FAILED
```

- Khi restart, `TRANSFORMING` quay lại `PENDING`.
- `STORED` được tái sử dụng nếu output còn tồn tại và hợp lệ.
- Chỉ xóa source sau khi toàn bộ output đã được validate và transaction metadata commit thành công.
- Nếu transcode thất bại, job thất bại/retry theo cơ chế hiện tại và source được giữ nguyên; không tự động copy bản gốc.

### 3.4. Commit metadata và TimeFile

- `commitTierTransfer()` sử dụng kích thước output thực tế thay vì kích thước nguồn.
- Trong cùng write transaction: cập nhật aggregate range, trạng thái segment/job và tạo outbox cập nhật TimeFile.
- Worker viết lại TimeFile bằng atomic swap.
- Các truy vấn range overlay kích thước trong outbox chưa hoàn tất để quota, split, restore và lần move tiếp theo luôn dùng số liệu đúng.
- Job JSON trả thêm operation, input/output bytes, dung lượng tiết kiệm, số segment transcoded/preserved và encoder summary.

## 4. Kiểm thử và nghiệm thu

### 4.1. Unit test

- JSON round-trip, default và validation của policy mới.
- Lựa chọn NVENC/QSV/software và fallback khi encoder lỗi.
- Sinh argv an toàn, không upscale, giữ aspect ratio và giới hạn FPS/bitrate.
- Quyết định copy/transcode theo master toggle, tier đích và protected-video.
- Khôi phục state machine của segment sau restart.

### 4.2. Integration test

- HOT → WARM và WARM → COLD trên local/NAS/MinIO với H.264 và H.265.
- Scheduled, pressure và external move đều áp dụng profile phù hợp.
- Range chứa xen kẽ segment protected được tách đúng.
- Transcode, upload hoặc commit thất bại vẫn giữ source và phục hồi đúng sau restart.
- Kích thước range, quota và TimeFile phản ánh output thực tế.
- Playback/restore tiếp tục sử dụng URL và storage key cũ.
- Dùng transformer giả lập cho test ổn định và thêm một bộ test `ffmpeg` thật, tự skip khi môi trường thiếu binary hoặc codec.

### 4.3. Validation và rollout

- Chạy tier-storage unit test và MinIO integration suite hiện có.
- Build bằng `cmake --build build -j2`.
- Kiểm tra migration từ database cũ và job `COPY_MOVE` đang tồn tại.
- Triển khai binary/migration lên toàn bộ node trước, xác nhận capability, sau đó mới cho FE bật profile.
- Tất cả cấu hình mới mặc định tắt để giữ tương thích ngược.

## 5. Giả định đã chốt

- Phiên bản đầu tiên không backfill và không giảm chất lượng audio.
- Profile dùng thông số tùy chỉnh theo tier, không dùng preset cố định.
- FE chọn codec nhưng không chọn NVENC/QSV/software.
- Cho phép software fallback khi NVIDIA và Intel không khả dụng.
- Job đã queue luôn dùng snapshot profile tại thời điểm tạo.
- Video protected/evidence vẫn chuyển tier nhưng giữ nguyên nội dung và chất lượng.
