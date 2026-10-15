# Plan: báo "đang restore" cho replay khi MinIO đọc không kịp

> **Trạng thái:** HOÃN — chỉ làm khi trên production tốc độ đọc của MinIO (object storage)
> không đủ để restore xong một window replay trong `storage.replay_restore_wait_sec`.
> Khi MinIO đủ nhanh (restore window 300 s xong trong < 10 s), người xem không gặp các
> trường hợp dưới đây và plan này không cần làm.

## 1. Khi nào cần làm

Dấu hiệu trên production (cần thấy lặp lại, không phải lẻ tẻ):

- Log `Replay of camera=... goes on without the recordings still being restored, job=...`
  ([server/Manager.cpp](../server/Manager.cpp), `restoreReplayWindow`).
- Log `Replay restore window camera=... waited <ms>` thường xuyên chạm `replay_restore_wait_sec`.
- Cảnh báo throughput pool (`PoolThroughputTracker`) trên pool object storage.
- FE/VMS báo: mở xem lại bản ghi cũ bị 404 rồi mở lại thì được, hoặc xem lại tự dừng giữa chừng
  dù timeline còn bản ghi.

Trước khi code nên thử cấu hình: tăng `storage.replay_restore_wait_sec` (phải nhỏ hơn
`general.maxStreamWaitMS`), giảm `storage.replay_restore_window_sec`, hoặc nâng băng thông MinIO.
Chỉ khi các cách đó không đủ mới làm plan này.

## 2. Hành vi hiện tại (đã xác định qua code, 2026-10-15)

Không có mã lỗi riêng cho "restore không kịp".

### 2.1 Restore không kịp lúc mở replay

| Giao thức | API bị dừng | Mã trả |
|---|---|---|
| HTTP-FLV / HTTP-TS / HTTP-fMP4 | request stream | 404 |
| HLS | playlist gốc `/media/record/<cam>/.../hls.m3u8` (không phải segment) | 404 |
| HLS master | `/media/record/.../hls.master.m3u8` | 404 |
| RTSP / RTMP | DESCRIBE / play | not-found của giao thức |

Request treo tới hết `replay_restore_wait_sec` (mặc định 10 s) rồi mới nhận 404. Đường đi:
`findAsync`/`findAsyncByApp` → `openReplayAsync` → `open()` trên pool "replay open" → listener
`kBroadcastMediaSeeked2`/`kBroadcastGetReplayTimeline` gọi `restoreReplayWindow` → lô bị cắt trước
segment COLD → rỗng → `cb(nullptr)` → `sendNotFound`. Con trỏ null không mang lý do.

Mở lại cùng URL dùng lại job restore đang chạy (`requestRestoreWindow`), nên retry sau vài giây
thường thành công.

### 2.2 Restore không kịp khi đang phát (lô kế tiếp)

Reader tra lô kế 30 s × speed trước khi hết lô; lô có segment COLD thì chờ restore tối đa
`replay_restore_wait_sec`, hết chờ thì lô rỗng → `_timeline_exhausted` → phát hết phần đã có rồi
reader dừng, muxer huỷ.

- HTTP-FLV / TS / fMP4: header 200 đã gửi, kết nối đóng (EOF), giống hết bản ghi.
- HLS: `HlsMakerImp` ghi playlist cuối có `#EXT-X-ENDLIST`, giữ source và file thêm
  `hls.deleteDelaySec` (10 s). Player reload trong 10 s nhận playlist 200 + ENDLIST, segment 200,
  phát nốt rồi báo "ended" — **không có lỗi**. Reload sau 10 s: playlist và segment 404.

Người xem không phân biệt được "chưa restore xong" với "hết bản ghi".

## 3. Phần A — 503 + `Retry-After` lúc mở replay

Chỉ đổi trường hợp 2.1. Trường hợp 2.2 giữ nguyên (xem phần B).

**A1. Listener đánh dấu** — `restoreReplayWindow` ([server/Manager.cpp](../server/Manager.cpp)):
hết `replay_restore_wait_sec` mà job còn `RESTORING` thì gọi `MediaSource::markReplayRestoring()`
(ghi cờ `thread_local`; listener chạy đồng bộ trên đúng thread của `open()`). Không đổi
`Seek2Invoker`/`ReplayTimelineInvoker` và macro `*Args`.

**A2. `openReplayAsync` đọc cờ** ([src/Common/MediaSource.cpp](../src/Common/MediaSource.cpp)):
task trên pool reset cờ trước `open()`, đọc sau. `ReplayOpenRegistry::finish(key, reader, restoring)`,
`Waiter` thành `void(bool opened, bool restoring)` để request join cùng lần mở cũng nhận lý do.

**A3. Báo người gọi, không đổi callback cũ**: `findAsync`/`findAsyncByApp` thêm tham số cuối
`const std::function<void()> &on_restoring = nullptr`. Thất bại do restore và có `on_restoring` thì
gọi nó thay cho `cb(nullptr)`; không truyền thì như cũ (RTMP, SRT, WebRTC, FFmpegSource,
HttpFileManager, test không phải sửa).

**A4. HttpSession trả 503**: helper `sendReplayRestoring(close_flag)` — `503`,
`Retry-After: <replay_restore_wait_sec>`, body `503 Replay Restoring`. Truyền `on_restoring` ở 4 chỗ:
- `checkLiveStream` (FLV/TS/fMP4 per-stream, `findAsync`)
- `checkLiveStreamHls` nhánh redirect playlist replay (`findAsyncByApp`)
- `checkLiveStreamByApp` (replay gộp)
- `checkLiveStreamHlsByApp` (HLS master)

**A5 (tuỳ chọn). RTSP** trả `503 Service Unavailable` trong `RtspSession` (chỗ gọi `findAsync`).
RTMP giữ `NetStream.Play.StreamNotFound`, WebRTC giữ nguyên.

**A6. Test + docs**
- Unit (`tests/unit/record/test_merged_replay_open.cpp`, theo mẫu test `findAsync` có sẵn):
  listener giả gọi `markReplayRestoring()` và trả timeline rỗng → `on_restoring` được gọi, `cb`
  không; request join cùng lần mở cũng nhận `on_restoring`; không có cờ → `cb(nullptr)`.
- E2E MinIO: `replay_restore_wait_sec=0`, mở replay COLD → 503 + `Retry-After` (FLV và HLS
  playlist); mở lại → 200.
- `docs/api-storage-tiering.md`: thêm mã 503, báo FE/VMS.

**Ước lượng:** ~65 dòng production (~75 nếu có A5), ~70 dòng test, ~10 dòng docs.

Vẫn là 404: `maxStreamWaitMS` hết trước `wait_sec`; job `FAILED`; `auto_restore_on_record_access=0`;
build tắt tier storage.

**Phía FE:** retry khi nhận 503 theo `Retry-After`. hls.js mặc định coi lỗi tải manifest là lỗi
nặng — cần cấu hình retry cho manifest.

## 4. Phần B — dừng giữa chừng (chưa thiết kế chi tiết)

Không giải được bằng mã HTTP vì response đã 200 (FLV) hoặc HLS trả ENDLIST hợp lệ. Hướng có thể:

- **B1. Không cắt lô khi restore đang chạy**: reader đứng chờ (như `kBatchPending` của per-stream)
  tới khi job xong hoặc FAILED, player buffer thay vì kết thúc. Cần giới hạn thời gian chờ tổng và
  xử lý HLS (không có segment mới → player stall, không ENDLIST).
- **B2. HLS không ghi ENDLIST khi dừng vì restore**: player reload thấy playlist không đổi rồi tự
  báo lỗi/stall; phức tạp hơn và phụ thuộc player.

Chọn hướng khi có số liệu production: restore window 300 s thực tế mất bao lâu.

## 5. Cần chốt trước khi code

1. Có làm A5 (RTSP) không.
2. `Retry-After` theo `replay_restore_wait_sec` hay hằng số.
3. Có cần phần B không, sau khi đo restore thực tế trên production.
