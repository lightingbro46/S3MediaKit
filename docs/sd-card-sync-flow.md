# Luồng đồng bộ dữ liệu thẻ nhớ SD

`MediaGapFillManager` là implementation duy nhất của luồng lấp dữ liệu media bị thiếu. Tên,
tham số, phân quyền và response JSON của các API lịch sử được giữ tương thích với client cũ.

## Luồng hoạt động

1. `CameraStatisticImp` ghi một device-level status gap khi camera offline và đóng gap khi online.
2. Sau khi ONVIF kết nối thành công, `CameraController` đăng ký provider SD bằng
   `MediaGapFillManager::onManager()`.
3. Sự kiện online được phát sau khi gap đã đóng và gọi `onDeviceStateChanged()` để discovery.
4. Provider tìm recording/segment trên SD trong khoảng `[disconnect_time, reconnect_time)`.
5. Các khoảng được lọc theo `sdCardSyncMinSegmentGapSec` và record schedule.
6. Kết quả discovery được đánh dấu terminal trong `media_gap_fill_session`; gap không có media
   không bị gọi ONVIF lại ở các management cycle sau.
7. Mỗi khoảng hợp lệ trở thành một `media_gap_fill_task` có checkpoint và retry riêng.
8. Scheduler giới hạn tổng connection, chỉ chạy một session trên mỗi device nhưng cho phép các
   stream thuộc cùng session chạy song song.
9. RTSP replay được ghi vào stream `<stream_id>.relay`; metadata lưu lại stream gốc và timestamp lịch sử.

```text
device status gap
  -> ONVIF discovery
  -> media_gap_fill_task
  -> RTSP replay (<stream_id>.relay)
  -> MP4 recorder
  -> kBroadcastRecordMP4
  -> timefile/tier storage với stream ID gốc
```

## Persistence và resume

Bảng `media_gap_fill_session` lưu trạng thái discovery (`Pending`, `Discovering`, `Ready`,
`NoMedia`, `Failed`, `Completed`) và trạng thái điều khiển `Active`/`Paused` của gap.

Bảng `media_gap_fill_task` lưu:

- `session_id = sess-<disconnect_time>` để giữ contract client;
- `gap_id` nội bộ;
- device, stream và `segment_index` ổn định;
- replay URI và timestamp đầu nguồn để resume mà không discovery lại; credentials chỉ tồn tại
  trong provider runtime và không được lưu xuống DB;
- khoảng disconnect/reconnect và khoảng segment;
- `processed_up_to`, `retry_count`, `next_retry_time`, trạng thái và lỗi cuối;
- thời gian bắt đầu/kết thúc xử lý.

Task `Success` không được enqueue lại. Task `Failed` chỉ chạy lại qua API reset. Task
`Pending` hoặc `Processing` được dựng lại từ checkpoint sau restart.

Manager lấy live progress từ replay provider mỗi 5 giây và force-flush trước stop, retry hoặc
shutdown. Playback chỉ được đánh dấu hoàn tất khi raw progress lớn hơn `0.99`; khi hoàn tất,
`processed_up_to` được chốt chính xác bằng `end_time`. Callback của lần chạy cũ được loại bằng
generation token nên không thể ghi đè lần chạy mới.

Retry lỗi runtime được đưa về `Pending` với lịch chạy tiếp theo lưu trong `next_retry_time`.
Backoff bắt đầu từ 2 giây, tăng dần và giới hạn 60 giây; scheduler dùng timer theo deadline gần
nhất nên không tạo vòng lặp retry tức thời và vẫn resume đúng lịch sau restart.

Migration 14 tạo trực tiếp `media_gap_fill_session` và `media_gap_fill_task`. Migration cũ
`12_camera_sd_card_sync.sql` cùng các bảng trung gian `sd_sync_*` đã được loại bỏ vì luồng cũ
chưa từng được triển khai production.

Các status gap đã đóng quá thời gian retention được xóa theo batch. Task, session và status gap
được xóa trong cùng transaction để tránh dữ liệu con mồ côi hoặc trạng thái cleanup dở dang.

## State và JSON tương thích

Các state client-visible giữ nguyên: `Pending`, `Processing`, `Success`, `Failed`.

Response session giữ cấu trúc:

```text
session_id
disconnect_time
reconnect_time
processing_start_time
processing_end_time
session_state
progress_percent
stream_handle_infos[]
  stream_id
  handle_state
  segments[]
    segment_index
    start_time
    end_time
    state
    processed_up_to
```

## API client

Các endpoint không đổi:

| Endpoint | Chức năng |
|---|---|
| `/media/mserver/device/historical/getSDInfo` | Thông tin SD và session còn lại |
| `/media/mserver/device/historical/getAllSessionsInfo` | Danh sách/chi tiết mọi session |
| `/media/mserver/device/historical/getSessionInfo` | Chi tiết một session |
| `/media/mserver/device/historical/resetFailedSession` | Retry các segment failed của session |
| `/media/mserver/device/historical/resetFailedSegment` | Retry một segment failed |
| `/media/mserver/device/historical/stopSession` | Dừng session đang xử lý và đưa về Pending |
| `/media/mserver/device/historical/stopSegment` | Dừng segment đang xử lý và đưa về Pending |

Token, permission, ownership check, request argument và API error code vẫn dùng implementation
trong `server/WebApiSdCardSync.cpp`.

## Cấu hình

| Camera option | Vai trò |
|---|---|
| `sdCardSyncEnabled` | Bật đồng bộ SD |
| `sdCardSyncAutoSyncEnabled` | Tự discovery khi camera sẵn sàng |
| `sdCardSyncMinSegmentGapSec` | Bỏ qua outage quá ngắn |
| `sdCardSyncRetryCount` | Số retry tối đa của task |

`MediaGapFillManager::setMaxConnections()` cấu hình giới hạn connection toàn hệ thống; mặc định là 4.
