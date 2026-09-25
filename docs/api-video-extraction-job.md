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

Endpoint này dành cho giao tiếp service-to-service và không sử dụng JWT hoặc quyền người dùng/camera. Khi `manager.enableAuthorize` bật, request phải có header `X-Secret-Key`, và giá trị phải khớp chính xác với `manager.apiSecret`. Lưu ý: nếu `manager.apiSecret` rỗng, MediaServer chỉ yêu cầu header tồn tại mà không so sánh giá trị. Vì vậy môi trường triển khai phải cấu hình secret khác rỗng. Thiếu hoặc sai secret → HTTP 401, `code=901001`.

| Field | Kiểu | Bắt buộc | Ràng buộc |
|---|---|---|---|
| `fileId` | string | Có | Không rỗng; là khóa idempotency |
| `cameraId` | string | Có | Tối đa 256 ký tự; camera phải tồn tại trên node |
| `startTime`, `endTime` | số nguyên | Có | `startTime > 0`, `endTime > startTime`, độ dài không vượt `extract_job.max_duration_sec` (mặc định 3600) |
| `uploadUrl` | string | Có | HTTPS, không chứa userinfo, tối đa 8192 ký tự; nếu `extract_job.allowed_upload_hosts` khác rỗng thì host phải nằm trong danh sách (phân tách bằng dấu phẩy) |
| `format` | string | Có | `mp4`, `mkv` hoặc `avi` |
| `streamId` | string | Không | Tối đa 256 ký tự; bỏ trống thì MediaServer chọn stream có dữ liệu khả dụng dài nhất |

Sai kiểu dữ liệu (ví dụ `startTime` là chuỗi) trả `902001`. Khoảng thời gian không hợp lệ trả `902009`, định dạng không hỗ trợ trả `902003`, còn camera không tồn tại trả `905001`.

Phản hồi thành công dùng HTTP `202 Accepted`:

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "fileId": "b0ca90e6-d938-4f25-9b78-1eda8b33feb7",
    "accepted": true,
    "status": "QUEUED"
  }
}
```

Response tạo job chỉ có `fileId`, `accepted` và `status` (`QUEUED`, `RUNNING`, `SUCCESS` hoặc `FAILED`). `stage` và `progressPercent` chỉ có trong API liệt kê job.

`fileId` là khóa idempotency. Gửi lại đúng toàn bộ nội dung sẽ nhận `202` kèm trạng thái hiện tại của job đã có. Dùng cùng `fileId` với nội dung khác sẽ nhận HTTP `409 Conflict` (`909004`). Lỗi lưu job trả `909005`. URL có chữ ký chỉ được lưu trong lúc job còn cần upload và không được ghi vào log.

Restore là một stage nội bộ của tiến trình trích xuất. API liệt kê job giữ `status=RUNNING`,
trả `stage=RESTORING` và ánh xạ tiến độ restore vào khoảng 0-30%. FFmpeg dùng khoảng
30-90%, upload dùng 90-100%. ID của restore job không được đưa vào response. Nếu
`streamId` rỗng, MediaServer chọn trước stream có dữ liệu khả dụng dài nhất rồi chỉ
restore và trích xuất stream đó. Việc chọn stream, chọn nguồn đọc, restore, giữ cache
restore và chạy FFmpeg đều do cùng `FFmpegExtractor` quản lý; manager của service job
chỉ lưu trạng thái, upload và callback.

## Liệt kê job

```http
GET /media/esc/extractArchived/job/list?camera_id=cam-001&start_time_from=1784250000&start_time_to=1784260000&page=0&size=20
Authorization: Bearer <jwt>
```

Khác với API tạo job, API này xác thực bằng JWT người dùng, không dùng `X-Secret-Key`.

Các bộ lọc đều không bắt buộc. `camera_id` được so khớp chính xác; `start_time_from` và `start_time_to` là epoch giây, được áp dụng inclusive lên thời điểm bắt đầu của job. Nếu `start_time_from > start_time_to` hoặc có giá trị âm, API trả `902009`. Kết quả được sắp xếp theo `start_time` mới nhất trước. `page` bắt đầu từ 0 (giá trị âm được coi là 0). `size` mặc định là 20; giá trị `<= 0` hoặc `> 100` được đưa về 20.

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
        "stage": "COMPLETED",
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

MediaServer thực hiện HTTP `PUT` nội dung file trực tiếp tới `uploadUrl`, đặt `Content-Type` tương ứng định dạng. Mọi mã HTTP 2xx là thành công. Lỗi mạng, 408, 429 và 5xx được retry theo cấu hình: `extract_job.upload_max_attempts` (mặc định 3), `extract_job.retry_delay_sec` (5) và `extract_job.upload_timeout_sec` (300). Lỗi 401/403 kết thúc job với mã `UPLOAD_URL_EXPIRED`. Các lỗi 4xx khác kết thúc job với mã `UPLOAD_FAILED`. Presigned URL cần còn hạn đủ lâu để bao cả thời gian job chờ trong hàng đợi (`extract_job.max_concurrent`, mặc định 1 job/node), thời gian restore và thời gian trích xuất.

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

| `error.code` | Ý nghĩa |
|---|---|
| `RECORDING_NOT_FOUND` | Khoảng thời gian không có dữ liệu ghi hình |
| `EXTRACT_FAILED` | FFmpeg hoặc restore thất bại |
| `UPLOAD_URL_EXPIRED` | Storage trả 401/403 khi upload |
| `UPLOAD_FAILED` | Upload thất bại sau khi đã retry, hoặc file tạm bị mất |
| `INTERNAL_ERROR` | Lỗi nội bộ khi lưu metadata file |

Callback có ngữ nghĩa at-least-once: VMS phải xử lý idempotent theo `fileId`. MediaServer chỉ đánh dấu `DELIVERED` khi callback trả 2xx. Lỗi có thể retry sẽ được lưu lại trong database và gửi lại, kể cả sau khi khởi động lại, tối đa `extract_job.callback_max_attempts` lần (mặc định 10). Sau đó callback chuyển sang `DEAD`, và VMS cần đối soát bằng API liệt kê job.

## Đọc dữ liệu từ cold storage

Với `storage.object_read_mode=HYBRID`, persistent extraction có thể đọc trực tiếp object qua URL tạm khi pool online, TTL đủ dài và request không vượt giới hạn segment/byte. Khi tạo playlist cho FFmpeg, path của block COLD được thay bằng presigned URL; URL không được trả qua API hoặc lưu trong database. Nếu resolver xác định nguồn trực tiếp cần restore trước khi chạy FFmpeg, job chuyển sang cơ chế restore rồi thực hiện lại.

Segment được tải lên khi đang bật mã hoá bản ghi nằm trên bucket dưới key `<key>.mp4.enc`, FFmpeg không giải mã được. Khi có pool bật mã hoá, trước khi cấp URL resolver gửi HEAD cho key `.enc` rồi tới key trơn: segment mã hoá, hoặc segment không xác nhận được bằng HEAD, luôn đi qua restore. Khi không pool nào bật mã hoá thì resolver không HEAD, nên object `.enc` còn lại từ lúc trước khi tắt mã hoá vẫn bị cấp URL và FFmpeg sẽ báo lỗi đọc.

## Trạng thái bền vững

Luồng chính là `PENDING -> RESTORING|EXTRACTING -> UPLOADING -> SUCCESS|FAILED`. Callback có trạng thái riêng `NONE -> PENDING -> SENDING -> DELIVERED|DEAD`. Khi khởi động, job bị gián đoạn ở `RESTORING` hoặc `EXTRACTING` được đưa về `PENDING` và tự lập kế hoạch nguồn đọc lại theo cấu hình `storage.object_read_mode`; `SENDING` được đưa về `PENDING`, còn `UPLOADING` tiếp tục từ file tạm đã lưu.
