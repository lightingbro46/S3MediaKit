# Job trích xuất video sự kiện

MediaServer nhận yêu cầu, lưu job vào SQLite của MediaServer trước khi xử lý, trích xuất dữ liệu ghi hình bằng FFmpeg, tải file lên URL được cấp và gửi kết quả cuối về VMS. Job chưa hoàn tất và callback chưa gửi được sẽ được khôi phục sau khi tiến trình khởi động lại.

## Tạo job

```http
POST /media/esc/extractArchived/job
X-Secret-Key: <manager.apiSecret>
Content-Type: application/json
```

```json
{
  "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
  "cameraId": "cam-001",
  "streamId": "main",
  "startTime": 1784253600,
  "endTime": 1784253615,
  "uploadUrl": "https://minio.internal/vms-clips/events/2026/07/15/b0ca90e6.mp4?X-Amz-Signature=...",
  "format": "mp4"
}
```

Endpoint này dành cho giao tiếp service-to-service. MediaServer chỉ nhận job khi header `X-Secret-Key` tồn tại và khớp chính xác với cấu hình `manager.apiSecret`; cấu hình secret rỗng luôn bị từ chối. Endpoint không sử dụng JWT hoặc quyền người dùng/camera.

`streamId` không bắt buộc; khi bỏ trống, MediaServer dùng luồng ghi hình phù hợp theo cơ chế hiện có. `format` nhận `mp4`, `mkv` hoặc `avi`. `uploadUrl` phải dùng HTTPS và có thể bị giới hạn bởi `extract_job.allowed_upload_hosts`.

Phản hồi thành công dùng HTTP `202 Accepted`:

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
    "status": "PENDING"
  }
}
```

`fileId` là khóa idempotency. Gửi lại đúng toàn bộ nội dung trả về job đã có; dùng cùng `fileId` với nội dung khác trả HTTP `409 Conflict`. URL có chữ ký chỉ được lưu trong lúc job còn cần upload và không được ghi vào log.

## Liệt kê job

```http
GET /media/esc/extractArchived/job/list?camera_id=cam-001&start_time_from=1784250000&start_time_to=1784260000&page=0&size=20

```

Các bộ lọc đều không bắt buộc. `camera_id` được so khớp chính xác; `start_time_from` và `start_time_to` là epoch giây, được áp dụng inclusive lên thời điểm bắt đầu của job. Kết quả được sắp xếp theo `start_time` mới nhất trước. `page` bắt đầu từ 0, `size` mặc định là 20 và tối đa 100.

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "items": [
      {
        "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
        "cameraId": "cam-001",
        "streamId": "main",
        "startTime": 1784253600,
        "endTime": 1784253615,
        "format": "mp4",
        "status": "SUCCESS",
        "progressPercent": 100.0,
        "callbackStatus": "DELIVERED",
        "completedAt": 1784253723,
        "createdAt": 1784253601,
        "updatedAt": 1784253724,
        "file": {
          "sizeBytes": 5242880,
          "durationSeconds": 15,
          "contentType": "video/mp4"
        }
      }
    ],
    "page": 0,
    "size": 20,
    "statistics": {
      "total": 12,
      "success": 9,
      "failed": 2
    }
  }
}
```

Thống kê được tính trên toàn bộ tập dữ liệu sau khi áp dụng bộ lọc, không chỉ trên trang hiện tại. Vì các job đang xử lý không thuộc `SUCCESS` hoặc `FAILED`, `success + failed` có thể nhỏ hơn `total`.

## Upload file

MediaServer thực hiện HTTP `PUT` nội dung file trực tiếp tới `uploadUrl`, đặt `Content-Type` tương ứng định dạng. Mọi mã HTTP 2xx là thành công. Lỗi mạng, 408, 429 và 5xx được retry theo cấu hình; lỗi 4xx khác kết thúc job với trạng thái `FAILED`.

## Callback kết quả

Địa chỉ callback là `hook.api_url` kết hợp `hook.on_video_extraction_result` (mặc định `/api/media-server/video-extractions/result`). Callback dùng `POST`, `Content-Type: application/json` và header `Idempotency-Key` bằng `fileId`.

Thành công:

```json
{
  "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
  "status": "SUCCESS",
  "file": {
    "sizeBytes": 5242880,
    "durationSeconds": 15,
    "contentType": "video/mp4"
  },
  "completedAt": 1784253723
}
```

Thất bại:

```json
{
  "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
  "status": "FAILED",
  "error": {
    "code": "RECORDING_NOT_FOUND",
    "message": "No recording covers the requested interval"
  },
  "completedAt": 1784253723
}
```

Callback có ngữ nghĩa at-least-once: VMS phải xử lý idempotent theo `fileId`. MediaServer chỉ đánh dấu `DELIVERED` khi callback trả 2xx; lỗi có thể retry được sẽ được lưu lại trong database và gửi lại sau khi khởi động lại.

## Trạng thái bền vững

Luồng chính là `PENDING -> EXTRACTING -> UPLOADING -> SUCCESS|FAILED`. Callback có trạng thái riêng `NONE -> PENDING -> SENDING -> DELIVERED|DEAD`. Khi khởi động, `EXTRACTING` được đưa về `PENDING`, `SENDING` được đưa về `PENDING`, còn `UPLOADING` tiếp tục từ file tạm đã lưu.
