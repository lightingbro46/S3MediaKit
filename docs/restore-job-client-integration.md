# Hướng dẫn tích hợp luồng Restore Job cho Web, Desktop và Mobile

Tài liệu này mô tả contract chung để ba phân hệ Web, Desktop và Mobile khôi phục dữ liệu ghi hình từ tầng lưu trữ COLD bằng hai API:

- `POST /media/mserver/storage/restoreJob/create`
- `POST /media/mserver/storage/restoreJob/detail`

Frontend xác định khoảng thời gian cần restore từ API timeline. Backend dùng window đó để kiểm tra và resolve chính xác các segment COLD; API detail chỉ dùng để theo dõi job đã tạo hoặc được tái sử dụng.

## 1. Điều kiện tích hợp

- MediaServer phải được build với `ENABLE_TIER_STORAGE=ON`.
- Request cần JWT hợp lệ trong header `Authorization: Bearer <JWT>`.
- User cần đồng thời permission Playback `1003`, Read MediaServer `150301` và quyền truy cập camera.
- `start_time` và `end_time` là Unix timestamp theo giây, với `0 < start_time < end_time`.
- `target_tier` hiện chỉ hỗ trợ `HOT`.
- FE gửi tham số bằng JSON body với `Content-Type: application/json`.
- `GET restoreJob/detail?job_id=...` vẫn có thể được giữ cho client cũ, nhưng
  tích hợp mới nên thống nhất dùng `POST` với JSON body.

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
Content-Type: application/json

{
  "camera_id": "cam-001",
  "stream_id": "main",
  "start_time": 1789092000,
  "end_time": 1789092600,
  "target_tier": "HOT",
  "reason": "PLAYBACK"
}
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
POST /media/mserver/storage/restoreJob/detail
Authorization: Bearer <JWT>
Content-Type: application/json

{
  "job_id": "rj-12345678"
}
```

Client cũ có thể tiếp tục dùng
`GET /media/mserver/storage/restoreJob/detail?job_id=...`; client mới nên dùng
POST JSON để thống nhất transport với API create.

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
2. Nếu chưa `READY` và đang `RESTORING`, gọi detail sau 5 giây.
3. Khi ứng dụng/tab ở background, giữ tối thiểu 5 giây, tăng lên khoảng 10 giây
   hoặc tạm dừng theo lifecycle nền tảng.
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
| `-1` | 500 | Không tạo được job hoặc không có segment phù hợp để enqueue (trước 2026-10-05 là `914019`) |

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
        snapshot = POST restoreJob/detail({"job_id": snapshot.job_id})
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

Phần này lấy state machine trong `www/console/player/pc-restore.js` làm tham
chiếu. Contract dành cho FE mới được chuẩn hóa dùng JSON body và poll mỗi 5 giây,
không phụ thuộc vào DOM hoặc player library của Console.

### 13.1. Nguồn timeline và routing

Player dùng metadata trong từng period của:

```http
GET /media/esc/recordedTimePeriod?periodType=2&detail=1
```

Một period được xem là cần restore khi `restoreRequired === true` hoặc
`restore_required === true`. Khoảng của period được tính theo nửa kín:

```text
[startTime, startTime + duration)
```

Các period hợp lệ được nhóm theo cặp `mediaServerId + tier`, sắp xếp theo thời
gian và gộp khi giao nhau hoặc liền nhau. Không gộp range thuộc MediaServer hoặc
tier khác nhau. FE dùng timeline tổng hợp nhiều node phải gửi restore request
đến đúng MediaServer sở hữu range. Console hiện dùng `serverUrl` đang active và
giả định timeline cũng thuộc server đó.

Console nội bộ truyền `secret` trên query string. FE sản phẩm không nên sao chép
cách này; hãy dùng `Authorization: Bearer <JWT>` và permission như phần 1.

### 13.2. Chia restore window

Khi người dùng chọn timestamp `T`:

- Nếu `T` không thuộc range `restore_required`, hủy restore session hiện tại và
  gọi luồng seek/playback thông thường.
- Nếu `T` thuộc range COLD, pause player và tạo restore session mới.
- Làm tròn `T` xuống epoch giây. Chunk đầu bắt đầu đúng tại `T`, không bắt đầu
  từ đầu range COLD.
- Mỗi chunk dài tối đa 300 giây; chunk cuối được cắt tại cuối range.
- Dùng interval `[chunk.startTime, chunk.endTime)` để tránh trùng biên.

Ví dụ range COLD `[10:14:01, 10:30:00)` và người dùng chọn `10:16:00`:

```text
chunk 0: [10:16:00, 10:21:00)
chunk 1: [10:21:00, 10:26:00)
chunk 2: [10:26:00, 10:30:00)
```

Không restore phần `[10:14:01, 10:16:00)` mà người dùng không yêu cầu xem.
Backend vẫn lấy segment giao với window, vì một MP4 có thể bắt đầu trước
`chunk.startTime` nhưng còn dữ liệu tại thời điểm đó.

### 13.3. Model trạng thái phía client

Mỗi chunk nên có state độc lập:

```text
RestoreChunk
  startTime: number
  endTime: number
  jobId: string
  snapshot: RestoreJobSnapshot | null
  status: IDLE | CREATING | RESTORING | READY |
          NOT_READY | FAILED | CANCELLED | EXPIRED
  requestInFlight: boolean
  prefetchStarted: boolean
  pollTimer: Timer | null
  error: string
```

Restore session giữ toàn bộ danh sách chunk để tính biên, nhưng tại một thời
điểm chỉ chủ động restore/poll chunk hiện tại và tối đa một chunk look-ahead.
Các cờ `requestInFlight` và `prefetchStarted` ngăn create/poll trùng.

Mỗi session cần một `generation` hoặc cancellation token. Callback bất đồng bộ
chỉ được cập nhật UI/player khi token vẫn thuộc session hiện tại. Invalidate
session phải tăng token và hủy toàn bộ timer; không cần cancel restore job ở
backend vì job có thể tiếp tục và được tái sử dụng.

### 13.4. Tạo job và bắt đầu playback

Với chunk cần phát, gọi một lần:

```http
POST /media/mserver/storage/restoreJob/create
Content-Type: application/json

{
  "camera_id": "<camera>",
  "stream_id": "<active-stream>",
  "start_time": <chunk-start>,
  "end_time": <chunk-end>,
  "target_tier": "HOT",
  "reason": "PLAYBACK"
}
```

Nên truyền rõ `stream_id` đang phát. Nếu để trống, backend có thể restore tất cả
stream phù hợp của camera trong window.

Luồng xử lý response create:

1. Luôn lấy `job_id` từ response, kể cả `job_created=false` vì backend có thể tái
   sử dụng job đang chạy hoặc cache READY.
2. Nếu snapshot đã `READY` và `playback_ready=true`, mở playback ngay.
3. Nếu `RESTORING`, lưu snapshot và poll detail.
4. Nếu là trạng thái terminal khác, dừng poll và hiển thị CTA phù hợp.
5. Không gọi create lần nữa trong vòng polling.

Chunk đầu READY thì gọi playback bằng timestamp người dùng chọn. Chunk sau READY
sau khi player đã bị chặn tại biên thì mở lại playback bằng `chunk.startTime`.

### 13.5. Polling, prefetch và chuyển chunk

FE mới áp dụng các giá trị sau:

- Poll `restoreJob/detail` mỗi 5 giây bằng POST JSON body
  `{"job_id":"<job-id>"}`.
- Cộng jitter ngẫu nhiên `0..249 ms` vào mỗi lần schedule.
- Khi document bị ẩn, có thể tăng lên 10 giây hoặc tạm dừng theo lifecycle.
- Lỗi mạng hoặc HTTP 5xx: giữ snapshot gần nhất và retry sau 5 giây cộng jitter;
  không tạo job mới.
- HTTP/business error không retry: code `914018` được ánh xạ `NOT_READY`; lỗi
  khác được ánh xạ `FAILED`.
- Dừng timer ngay khi snapshot không còn `RESTORING`.

Khi playback đạt 80% thời lượng chunk hiện tại, gọi create cho chunk kế tiếp ở
nền. Prefetch không hiển thị overlay nếu playback vẫn tiếp tục bình thường.

Tại `current.endTime`:

- Không còn chunk tiếp theo: kết thúc restore session và tiếp tục behavior kết
  thúc range của player.
- Chunk kế tiếp READY: chuyển `currentChunkIndex`, giữ URL/player hiện tại và
  tiếp tục phát.
- Chunk kế tiếp chưa READY: pause player, hiển thị overlay và tiếp tục poll.
  Khi READY, mở lại playback tại đầu chunk kế tiếp.

Nếu media reader tự dừng trong vòng 2 giây trước cuối chunk, xử lý giống như đã
đến biên: chuyển sang chunk READY hoặc block chờ chunk kế tiếp.

### 13.6. Overlay và thao tác người dùng

Overlay chỉ bắt buộc hiển thị khi chunk hiện tại đang block playback. Nội dung
dùng snapshot như phần 6:

```text
progress = clamp(segment_progress_percent, 0, 100)
caption  = Đang khôi phục dữ liệu ... restored_segment_count/total_segment_count
```

Nếu `total_segment_count <= 0`, dùng progress indeterminate. Chỉ ẩn overlay và
cho phát khi đồng thời thỏa:

```text
playback_status == READY && playback_ready == true
```

Nút “Thử lại/Khôi phục lại” reset state của đúng chunk về `IDLE`, xóa `jobId`
cũ và gọi create lại bằng window của chunk đó. Nút dừng phải invalidate session,
dừng player và hủy timer. Seek mới, đổi camera/stream/transport/ngày, chuyển Live,
logout hoặc đổi server cũng phải invalidate session.

### 13.7. Pseudocode tham chiếu

```text
requestReplay(T):
    range = findRestoreRange(T)
    invalidateCurrentSession()
    if range == null:
        seekNormally(T)
        return

    session = newSession(splitFrom(floor(T), range.end, 300 seconds))
    pausePlayer()
    restoreChunk(session.current, blocking=true)

restoreChunk(chunk, blocking):
    if chunk READY:
        if blocking: resumeAt(chunk is first ? selectedTime : chunk.startTime)
        return
    if request already running or status is RESTORING/CREATING/terminal:
        return

    snapshot = POST restoreJob/create(chunk.window)
    chunk.jobId = snapshot.job_id
    applySnapshot(snapshot)

applySnapshot(snapshot):
    if callback does not belong to current generation: return
    replace old snapshot
    if READY && playback_ready:
        stopPolling()
        if current playback is blocked: resumeAtExpectedTimestamp()
    else if playback_status == RESTORING:
        scheduleDetailPoll(5 seconds + jitter)
    else:
        stopPolling()
        renderTerminalAction()

onPlaybackTime(wallTime):
    if wallTime >= current.start + 80% * current.duration:
        prefetchNextOnce()
    if wallTime >= current.end:
        if no next: finishSession()
        else if next READY: advanceWithoutReopeningPlayer()
        else: pauseAndBlockFor(next)
```

### 13.8. Checklist cho FE mới

- [ ] Chỉ tạo restore job khi timeline đánh dấu `restore_required=true`.
- [ ] Không gộp range khác `mediaServerId` hoặc tier.
- [ ] Chunk đầu bắt đầu tại timestamp người dùng chọn; chunk tối đa 300 giây.
- [ ] Truyền explicit active `stream_id` nếu chỉ phát một stream.
- [ ] Lưu `job_id` kể cả khi `job_created=false`.
- [ ] Chỉ phát khi `READY && playback_ready`.
- [ ] Create và detail dùng `Content-Type: application/json` với JSON body.
- [ ] Poll detail mỗi 5 giây; có jitter và backoff khi lỗi tạm thời.
- [ ] Prefetch một chunk tại mốc 80%, không create trùng.
- [ ] Có generation/cancellation token chống response cũ đổi player.
- [ ] Backoff lỗi mạng/5xx; không gọi create trong vòng poll detail.
- [ ] Invalidate timer/session khi thay đổi playback context.
- [ ] Không đưa `secret` lên query string trong FE sản phẩm.
