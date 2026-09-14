# Hướng dẫn tích hợp luồng Restore Job cho Web, Desktop và Mobile

Tài liệu này mô tả contract chung để ba phân hệ Web, Desktop và Mobile khôi phục dữ liệu ghi hình từ tầng lưu trữ COLD bằng hai API:

- `POST /media/mserver/storage/restoreJob/create`
- `GET/POST /media/mserver/storage/restoreJob/detail`

Frontend xác định khoảng thời gian cần restore từ API timeline. Backend dùng window đó để kiểm tra và resolve chính xác các segment COLD; API detail chỉ dùng để theo dõi job đã tạo hoặc được tái sử dụng.

## 1. Điều kiện tích hợp

- MediaServer phải được build với `ENABLE_TIER_STORAGE=ON`.
- Request cần JWT hợp lệ trong header `Authorization: Bearer <JWT>`.
- User cần đồng thời permission Playback `1003`, Read MediaServer `150301` và quyền truy cập camera.
- `start_time` và `end_time` là Unix timestamp theo giây, với `0 < start_time < end_time`.
- `target_tier` hiện chỉ hỗ trợ `HOT`.
- Các endpoint dùng query/form parameters, không dùng JSON body.

Response dùng envelope chung:

```json
{
  "code": 0,
  "msg": "",
  "data": {}
}
```

Chỉ coi request thành công khi HTTP thành công và `code == 0`.

## 2. Luồng tổng thể

```text
Timeline trả khoảng cần restore
        |
        v
POST restoreJob/create
        |
        +-- READY -------> gọi API playback hiện hữu
        |
        +-- RESTORING ---> lưu job_id và poll restoreJob/detail
                                |
                                +-- RESTORING -> cập nhật donut, tiếp tục poll
                                +-- READY ----> dừng poll, gọi playback
                                +-- FAILED ---> dừng poll, hiển thị thử lại
                                +-- NOT_READY -> dừng poll, cho phép gọi create lại
                                +-- CANCELLED -> dừng poll, cho phép gọi create lại
                                +-- EXPIRED ---> dừng poll, gọi create lại khi người dùng xem
```

Client không cần biết đường dẫn cache restore. Khi job `READY`, client gọi API xem lại như luồng hiện tại; `Mp4Reader` sẽ resolve và đọc file trong folder restore của job.

## 3. Tạo hoặc tái sử dụng restore job

### 3.1 Request

```http
POST /media/mserver/storage/restoreJob/create
Authorization: Bearer <JWT>
Content-Type: application/x-www-form-urlencoded

camera_id=cam-001&stream_id=main&start_time=1789092000&end_time=1789092600&target_tier=HOT&reason=PLAYBACK
```

| Field | Bắt buộc | Mô tả |
|---|---:|---|
| `camera_id` | Có | ID camera lấy từ timeline/playback context |
| `stream_id` | Không | Stream cần restore; bỏ trống để restore các stream phù hợp trong window |
| `start_time` | Có | Đầu khoảng restore, epoch giây |
| `end_time` | Có | Cuối khoảng restore, epoch giây |
| `target_tier` | Không | Mặc định `HOT`; hiện không nhận giá trị khác |
| `reason` | Không | Metadata mô tả, mặc định `MANUAL`; không tham gia điều kiện tái sử dụng job |

### 3.2 Response khi tạo job mới

```json
{
  "code": 0,
  "msg": "",
  "data": {
    "job_id": "rj-12345678",
    "type": "RESTORE",
    "camera_id": "cam-001",
    "source_tier": "COLD",
    "target_tier": "HOT",
    "status": "PENDING",
    "start_time": 1789092000,
    "end_time": 1789092600,
    "total_bytes": 104857600,
    "processed_bytes": 0,
    "progress_percent": 0,
    "job_created": true,
    "playback_status": "RESTORING",
    "playback_ready": false,
    "total_segment_count": 10,
    "restored_segment_count": 0,
    "remaining_segment_count": 10,
    "failed_segment_count": 0,
    "segment_progress_percent": 0,
    "expires_at": 0,
    "created_at": 1789092601,
    "updated_at": 1789092601
  }
}
```

### 3.3 Quy tắc tái sử dụng

Backend tìm job có cùng camera, stream, time window, target tier và cùng tập segment/source pool:

- Job `PENDING` hoặc `RUNNING`: trả lại job hiện có với `job_created=false`.
- Job `DONE` và toàn bộ cache còn đầy đủ: trả lại job với `playback_status=READY`, đồng thời cập nhật `updated_at` để bắt đầu lại TTL.
- Job `DONE` nhưng thiếu cache: không tái sử dụng; tạo job mới.
- Job `FAILED`, `CANCELLED` hoặc `EXPIRED`: không tái sử dụng; tạo job mới.
- Request khác thông số có thể tạo job riêng và tải lại cùng segment.

Frontend phải luôn sử dụng `data.job_id` từ response, không tự giả định create sẽ sinh ID mới. Nếu create trả `READY`, frontend có thể phát ngay mà không cần poll detail.

> Poll API detail không gia hạn TTL. Muốn tái sử dụng và gia hạn một job `DONE`, client phải gọi lại create với đúng thông số restore ban đầu.

## 4. Theo dõi trạng thái restore

### 4.1 Request

```http
GET /media/mserver/storage/restoreJob/detail?job_id=rj-12345678
Authorization: Bearer <JWT>
```

Có thể dùng POST với form parameter `job_id`, nhưng cả ba client nên thống nhất dùng GET để truy vấn trạng thái.

### 4.2 Response

Response detail có cùng snapshot như create, ngoại trừ không có `job_created`:

```json
{
  "code": 0,
  "msg": "",
  "data": {
    "job_id": "rj-12345678",
    "type": "RESTORE",
    "camera_id": "cam-001",
    "source_tier": "COLD",
    "target_tier": "HOT",
    "status": "RUNNING",
    "start_time": 1789092000,
    "end_time": 1789092600,
    "total_bytes": 104857600,
    "processed_bytes": 41943040,
    "progress_percent": 40,
    "playback_status": "RESTORING",
    "playback_ready": false,
    "total_segment_count": 10,
    "restored_segment_count": 4,
    "remaining_segment_count": 6,
    "failed_segment_count": 0,
    "segment_progress_percent": 40,
    "expires_at": 0,
    "created_at": 1789092601,
    "updated_at": 1789092610
  }
}
```

API không trả `estimated_restore_seconds`. Client không tự tạo countdown hoặc suy diễn thời gian hoàn tất từ số segment.

## 5. Trạng thái dùng cho giao diện

Frontend dùng `playback_status` làm trạng thái nghiệp vụ chính và dùng `playback_ready` làm điều kiện cuối để bật playback.

| `playback_status` | Ý nghĩa | Hành động client |
|---|---|---|
| `RESTORING` | Job đang `PENDING` hoặc `RUNNING` | Hiển thị progress và tiếp tục poll |
| `READY` | Job `DONE`, toàn bộ cache tồn tại và TTL còn hiệu lực | Dừng poll, bật nút xem và gọi playback |
| `NOT_READY` | Parent đã `DONE` nhưng cache/child không còn đầy đủ | Dừng poll, hiển thị nút “Khôi phục lại” để gọi create |
| `FAILED` | Parent hoặc ít nhất một child thất bại | Dừng poll, hiển thị lỗi chung và nút thử lại |
| `CANCELLED` | Job đã bị hủy | Dừng poll, cho phép gọi create lại |
| `EXPIRED` | Job hoặc cache đã hết TTL | Dừng poll; khi người dùng xem lại thì gọi create |

Không bật playback chỉ dựa trên `status == DONE`; điều kiện an toàn là:

```text
playback_status == READY && playback_ready == true
```

## 6. Quy ước hiển thị progress

### 6.1 Donut progress

Hiển thị tiến độ bằng icon donut determinate:

- Giá trị: `segment_progress_percent`, clamp trong khoảng `0..100`.
- Chữ giữa donut: `{segment_progress_percent}%`.
- Không dùng `progress_percent` cho donut vì field đó tính theo byte, trong khi UX cần phản ánh số segment đã restore.
- Khi `total_segment_count <= 0`, hiển thị donut dạng indeterminate và không thực hiện phép chia ở client.
- Donut phải có accessibility label, ví dụ: `Đã khôi phục 4 trên 10 đoạn dữ liệu`.

Trong trạng thái `RESTORING`, ngay dưới donut hiển thị đúng mẫu:

```text
Đang khôi phục dữ liệu ... {restored_segment_count}/{total_segment_count}
```

Ví dụ:

```text
Đang khôi phục dữ liệu ... 4/10
```

Wireframe tối thiểu dùng chung:

```text
┌──────────────────────────────────┐
│              ◔ 40%               │
│                                  │
│  Đang khôi phục dữ liệu ... 4/10 │
└──────────────────────────────────┘
```

Web/Desktop có thể đặt panel cạnh player; Mobile nên đặt panel trong vùng nội dung phía trên hoặc thay thế player cho đến khi `READY`. Thứ tự donut trước, chú thích ngay dưới phải được giữ nhất quán giữa ba nền tảng.

Nên khai báo chuỗi bằng localization key thay vì ghép cứng trong từng ứng dụng:

```text
restore.progress.caption = Đang khôi phục dữ liệu ... {restored}/{total}
```

### 6.2 Gợi ý theo trạng thái

| Trạng thái | Donut/icon | Dòng chú thích | CTA |
|---|---|---|---|
| `RESTORING` | Donut theo `segment_progress_percent`, màu primary | `Đang khôi phục dữ liệu ... {restored}/{total}` | Có thể cho phép ẩn panel; không gọi create lặp |
| `READY` | Donut 100% hoặc icon thành công | `Dữ liệu đã sẵn sàng` | `Xem lại` |
| `NOT_READY` | Icon cảnh báo | `Dữ liệu khôi phục không còn đầy đủ` | `Khôi phục lại` |
| `FAILED` | Icon lỗi | `Không thể khôi phục dữ liệu` | `Thử lại` |
| `CANCELLED` | Icon trung tính | `Đã hủy khôi phục dữ liệu` | `Khôi phục lại` |
| `EXPIRED` | Icon đồng hồ/hết hạn | `Dữ liệu tạm đã hết hạn` | `Khôi phục lại` |

Màu sắc phải dùng design token của từng nền tảng; không dùng màu làm tín hiệu duy nhất. Luôn kết hợp icon, text và accessibility semantics.

## 7. View model dùng chung cho ba phân hệ

Web, Desktop và Mobile nên normalize API response về cùng một model:

```text
RestoreUiState
  jobId: string
  playbackStatus: RESTORING | READY | NOT_READY | FAILED | CANCELLED | EXPIRED
  playbackReady: boolean
  progressPercent: number
  restoredSegments: number
  totalSegments: number
  remainingSegments: number
  failedSegments: number
  expiresAt: number
  caption: string
  canPlay: boolean
  canRetry: boolean
```

Mapping tối thiểu:

```text
progressPercent = clamp(data.segment_progress_percent, 0, 100)
caption = format("Đang khôi phục dữ liệu ... {restored}/{total}",
                 data.restored_segment_count,
                 data.total_segment_count)
canPlay = data.playback_status == "READY" && data.playback_ready == true
canRetry = data.playback_status in ["NOT_READY", "FAILED", "CANCELLED", "EXPIRED"]
```

Không lưu riêng progress do client tự tính qua nhiều lần poll. Mỗi response mới phải thay thế snapshot cũ để tránh progress sai khi backend recovery job sau restart.

## 8. Chiến lược polling

Khuyến nghị chung:

1. Render ngay snapshot từ response create.
2. Nếu chưa `READY` và đang `RESTORING`, gọi detail sau khoảng 2 giây.
3. Khi ứng dụng/tab ở background, giảm xuống khoảng 5–10 giây hoặc tạm dừng theo lifecycle nền tảng.
4. Thêm jitter nhỏ để nhiều client không poll đồng thời.
5. Chỉ duy trì một polling task cho mỗi `job_id`.
6. Dừng polling khi trạng thái khác `RESTORING`, khi người dùng rời màn hình hoặc khi request bị hủy.
7. Với lỗi mạng tạm thời, backoff có giới hạn; không gọi lại create chỉ vì một lần detail timeout.
8. Khi app quay lại foreground, gọi detail ngay bằng `job_id` đã lưu rồi tiếp tục theo trạng thái mới.

Không đặt timeout nghiệp vụ dựa trên ước lượng thời gian restore vì API không cung cấp estimate. Có thể hiển thị hành động đóng/ẩn panel trong khi job tiếp tục chạy ở backend.

## 9. Lưu ý theo nền tảng

### Web

- Dùng một polling controller keyed theo `job_id` để tránh nhiều component tạo timer trùng.
- Hủy HTTP request/timer khi component unmount.
- Dùng Page Visibility API để giảm hoặc tạm dừng polling khi tab ẩn; gọi detail ngay khi tab active lại.
- Donut cần `role="progressbar"`, `aria-valuemin="0"`, `aria-valuemax="100"` và `aria-valuenow` tương ứng.

### Desktop

- Polling thuộc view model/service, không gắn trực tiếp với vòng đời một dialog ngắn hạn.
- Khi cửa sổ được mở lại, lấy snapshot mới bằng detail thay vì dùng trạng thái cache cũ.
- Hủy timer và request khi logout, đổi server hoặc ứng dụng thoát.
- Nếu app chạy nền, có thể giảm tần suất poll nhưng không tạo lại job.

### Mobile

- Không phụ thuộc vào background polling liên tục vì hệ điều hành có thể suspend ứng dụng.
- Lưu `job_id` cùng camera/window; khi app foreground lại, gọi detail để phục hồi UI.
- Khi mất mạng, giữ snapshot gần nhất kèm trạng thái offline; không chuyển job sang `FAILED` ở client.
- Donut và CTA phải có vùng chạm, contrast và screen-reader label theo guideline của nền tảng.

## 10. Xử lý lỗi API

| API code | HTTP | Cách xử lý gợi ý |
|---:|---:|---|
| `901001` | 401 | Yêu cầu đăng nhập lại/refresh token theo cơ chế hiện hữu |
| `901004` | 401 | User thiếu quyền Playback |
| `901006` | 401 | User thiếu quyền Read MediaServer |
| `902001` | 400 | Thiếu hoặc sai tham số |
| `902009` | 404 | Time window không hợp lệ |
| `905001` | 404 | Camera không tồn tại |
| `914018` | 404 | Job không tồn tại; cho phép gọi create lại nếu vẫn cần xem |
| `914019` | 500 | Không tạo được job hoặc không có segment phù hợp để enqueue |

Với lỗi mạng hoặc HTTP 5xx khi poll detail, giữ nguyên progress gần nhất và retry có backoff. Với business status `FAILED`, dừng poll và chờ thao tác chủ động của người dùng.

## 11. Pseudocode tích hợp

```text
restoreAndPlay(request):
    snapshot = POST restoreJob/create(request)
    render(snapshot)

    if snapshot.playback_status == READY && snapshot.playback_ready:
        openPlayback(request.camera_id, request.stream_id,
                     request.start_time, request.end_time)
        return

    while snapshot.playback_status == RESTORING:
        waitAccordingToAppLifecycle()
        snapshot = GET restoreJob/detail(snapshot.job_id)
        render(snapshot)

    if snapshot.playback_status == READY && snapshot.playback_ready:
        openPlayback(...)
    else:
        renderTerminalAction(snapshot.playback_status)
```

Khi người dùng chọn “Thử lại” hoặc “Khôi phục lại”, gọi lại create bằng camera, stream và time window ban đầu. Không gọi create trong mỗi vòng polling.

## 12. Checklist nghiệm thu chung

- [ ] Chỉ gọi create cho khoảng COLD đã xác định từ timeline.
- [ ] Lưu và sử dụng `job_id` backend trả về, kể cả `job_created=false`.
- [ ] Xử lý trường hợp create trả `READY` ngay.
- [ ] Donut dùng `segment_progress_percent`.
- [ ] Chú thích RESTORING đúng mẫu `Đang khôi phục dữ liệu ... {restored}/{total}`.
- [ ] Chỉ bật playback khi `playback_status=READY` và `playback_ready=true`.
- [ ] Poll detail một lần cho mỗi chu kỳ và mỗi `job_id`.
- [ ] Dừng poll ở mọi trạng thái terminal.
- [ ] Không tạo countdown vì API không có trường estimate.
- [ ] Resume đúng sau refresh trang, mở lại desktop view hoặc mobile foreground.
- [ ] CTA retry gọi create bằng đúng request ban đầu.
- [ ] Kiểm thử accessibility, localization và trường hợp `total_segment_count <= 0`.

## 13. Web Console Player: restore theo chunk

Player Console dùng trực tiếp metadata `tier` và `restore_required` trong từng
period của `GET /media/esc/recordedTimePeriod?periodType=2&detail=1`. Không cần
gọi thêm API storage timeline để tô dữ liệu COLD.

Các period `restore_required=true` liền nhau được chuẩn hóa thành một range.
Khi người dùng chọn một timestamp trong range, chunk đầu tiên bắt đầu đúng tại
timestamp đó; các chunk tiếp theo nối tiếp nhau và dài tối đa 300 giây:

1. Gọi `restoreJob/create` cho chunk bắt đầu tại timestamp và chỉ mở playback khi
   `playback_status=READY && playback_ready=true`.
2. Khi media timestamp đạt 80% chunk hiện tại, create/poll chunk kế tiếp ở nền.
3. Nếu chunk kế tiếp READY trước biên, giữ URL hiện tại và tiếp tục phát.
4. Nếu chưa READY tại biên, pause video, hiển thị restore overlay và tiếp tục
   poll detail.
5. Khi chunk bị chặn chuyển READY, mở URL replay mới với timestamp bằng đầu
   chunk đó. Không dùng URL cũ vì reader trước đã bị gián đoạn.

Mỗi playback restore session chỉ giữ tối đa current chunk và một chunk
look-ahead. Seek, đổi camera, stream, transport, ngày hoặc chuyển sang Live phải
invalidate session cũ; response bất đồng bộ cũ không được phép đổi URL hay UI.
Auto quality ưu tiên phát liên tục trên URL cũ khi prefetch kịp READY; tùy dữ
liệu hiện có, reader có thể tạm fallback sang low quality cho tới lần mở URL
tiếp theo.
