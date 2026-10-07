# Plan: giới hạn 10 s cho `/media/mserver/device/discovery`

Trạng thái: ĐÃ CODE D1–D5 (2026-10-07), chưa commit. Build MediaServer đạt; ctest unit 228/228; E2E onvif_mock đạt
(camera thường 200 trong 0.02 s; latency 3 s/lệnh và 20 s/lệnh → 504/905006 sau 10.00 s; 6 request đồng thời → 4×504 sau
10 s + 2×503 busy, slot trả ngay sau đó; subnetScan .1–.5 có 2 IP chậm xong sau 10.0 s, đủ 3 thiết bị). Còn camera thật.

Ghi chú khi làm: gSOAP 2.8.133 tính `transfer_timeout` âm sai (nhân 1e6 thay vì chia, `stdsoap2.cpp:687`) nên
`transfer_timeout` đặt bằng giây dương (làm tròn lên); `connect/recv/send_timeout` vẫn dùng µs âm. Hạn cũng áp
cho `readCameraClock` (GetSystemDateAndTime trong `setCredentials` chạy trên `_m_soap` ngoài `callWithAuth`).
Test case "server chỉ chậm sau GetDeviceInformation" và "không hạn giữ 10 s/lệnh" chưa viết (cần mock SOAP),
để E2E onvif_mock kiểm tra.

## Hiện trạng

`discovery` chạy `SubnetScan::discovery_device` trên `BlockingExecutors::api()` (qua `runApiWork`)
và đợi probe tự kết thúc. Không có hạn tổng, chỉ có timeout lẻ từng bước:

| Nhánh | Timeout hiện tại | Xấu nhất |
|---|---|---|
| `rtsp://` (ffprobe) | 10 s viết cứng + `process->kill(2000)` | ~12 s |
| IP camera (ONVIF) | `connect/recv/send_timeout = 10` s **mỗi lệnh SOAP** (`OnvifDeviceControl.cpp:525`, `LocalDeviceSoap` :1411) | `connect()` gọi tuần tự ~8 lệnh + theo từng profile (GetVideoEncoderConfigurations/Options, GetAudioSources, GetStreamUri), có thể gọi lại khi 401/lệch giờ → vài chục giây tới vài phút với camera chậm |
| Speaker | `wait_for(30 s)` (`SubnetScan.cpp:217`) | ~30 s |

Hệ quả:
- `HttpSession::onManager` đóng socket sau `http.keepAliveSecond` (mặc định 15 s) kể từ byte cuối
  nhận/gửi; lúc chờ async không reset `_ticker` → probe > 15 s thì client mất kết nối, không có JSON.
- Probe vẫn chạy tiếp, giữ thread pool `api()` và slot `api.limit.device.discovery.maxInflight` (= 4)
  → vài camera chậm làm các request discovery sau bị 503.
- `subnetScan` gọi cùng `discovery_device` cho từng IP nên một IP chậm kéo dài cả lượt quét.
- Khoá `searcher.timeout_sec` (mặc định 10) đã đăng ký nhưng **không nơi nào đọc**.

## Mục tiêu

- Request `discovery` luôn có response trong ≤ 10 s tính từ lúc server nhận request, kể cả thời gian
  xếp hàng; hết hạn trả `CODE_DEVICE_TIMEOUT` (HTTP 504, 905006) kèm `data.address`, `data.err_msg`.
- Probe tự dừng ở ≈ hạn đó để trả thread/slot, không chỉ "bỏ mặc" phía sau.
- Mỗi IP trong `subnetScan` cũng bị giới hạn bởi cùng hạn.
- Không đổi hành vi điều khiển camera/loa đang chạy (OnvifControl của camera thật không có deadline).
- Dùng lại `searcher.timeout_sec` (mặc định 10) làm hạn, không thêm khoá mới.

## Thiết kế: 2 lớp

**Lớp 1 — deadline trong probe** (giải phóng tài nguyên, áp cho cả `subnetScan`).
`discovery_device` lấy `deadline = now + searcher.timeout_sec` (steady clock) lúc bắt đầu và chia
phần thời gian còn lại cho từng nhánh. Có thể vượt nhẹ vài trăm ms (parse, kill process).

**Lớp 2 — hạn trả lời ở API** (đảm bảo cứng ≤ 10 s, gồm cả thời gian xếp hàng).
Handler đặt `doDelayTask(timeout)` trên poller của request; ai tới trước (kết quả probe hay timer)
thì trả lời, bên kia bỏ qua. Cả `done` của `runApiWork` lẫn timer đều chạy trên poller request nên
chỉ cần một `bool answered` trong `shared_ptr`, không cần atomic.

## Các bước

### D1. ONVIF tôn trọng deadline — `manager/Control/OnvifDeviceControl.{h,cpp}`
- Thêm `void setDeadline(uint64_t steady_ms)` (0 = không hạn, mặc định) và `bool deadlineExceeded() const`.
- Helper `bool applyDeadline(struct soap *soap)`: nếu không có hạn → không làm gì, trả `true`.
  Còn hạn → đặt `connect_timeout = recv_timeout = send_timeout = transfer_timeout =
  -min(10 s, còn lại)` tính bằng µs (gSOAP 2.8.133: giá trị âm = µs; `transfer_timeout` chặn tổng
  thời gian một lệnh, tránh camera nhỏ giọt từng byte). Hết hạn → set `_deadline_exceeded = true`,
  trả `false`.
- `callWithAuth`: gọi `applyDeadline(_m_soap)` trước **mỗi** lần `call()` (lần đầu, lần lại sau 401,
  lần lại sau ký lại); hết hạn thì trả `SOAP_TCP_ERROR` không gọi. Sau mỗi `call()` lỗi mà đã quá hạn
  thì cũng đánh dấu `_deadline_exceeded`.
- `LocalDeviceSoap` (đo giờ camera trong `measureCameraClock`/`setCredentials`): áp cùng helper
  trước khi gửi; quá hạn thì `readCameraClock` coi như thất bại.
- Không đụng chỗ khác: camera đang quản lý không gọi `setDeadline` nên giữ nguyên 10 s/lệnh.

### D2. `SubnetScan::discovery_device` — `manager/Control/SubnetScan.{h,cpp}`
- Đầu hàm: `GET_CONFIG(float, timeout_sec, Searcher::kTimeoutSec)`, tính deadline; `<= 0` thì dùng 10.
- RTSP: `FFmpegProbe::probe(url, remaining_sec - kill_grace, ...)`, `kill_grace` ≈ 1 s để cả bước
  kill vẫn trong hạn (sàn tối thiểu ~1 s để còn probe được).
- ONVIF: `onvif->setDeadline(deadline)` trước `connect()`. Nếu `connect()` sai **hoặc**
  `deadlineExceeded()` (một lệnh đã bị cắt — profile/stream URI có thể thiếu) → trả
  `SockException(Err_timeout, ..., CODE_DEVICE_TIMEOUT)` với `ret.address`, `ret.err_msg =
  "Discovery timeout after N s"`; còn lại giữ nguyên `CODE_DEVICE_NOT_FOUND`.
- Speaker: `wait_for(remaining)` thay cho 30 s, hết hạn trả `CODE_DEVICE_TIMEOUT`.
  `SpeakerController::probe` chạy tiếp ở nền rồi tự bỏ (callback chỉ giữ `result` shared_ptr) — đã an toàn.
- Cập nhật comment ở `SubnetScan.h:102-110` (bỏ "~20s for ONVIF").

### D3. Hạn trả lời ở handler — `server/WebApiControl.cpp`
- Đọc `searcher.timeout_sec` lúc nhận request; tạo `auto state = make_shared<{bool answered}>()`.
- `apiRequestPoller(sender)->doDelayTask(timeout_ms, ...)`: nếu `!answered` → `answered = true`,
  trả `CODE_DEVICE_TIMEOUT` (`data.address = address`, `data.err_msg`). Return 0.
- `done` của `runApiWork`: nếu `answered` → bỏ qua (log Debug), không gọi `invoker` lần 2; ngược lại
  `answered = true`, huỷ timer, trả lời như cũ.
- Lambda timer chỉ bắt `state`, `invoker`, `session` (weak) — không bắt `this`; session đã đóng thì
  thôi. Slot `maxInflight` vẫn được giữ tới khi probe xong (đúng: probe còn chiếm thread), nhưng
  nhờ D1/D2 probe kết thúc ≈ cùng lúc.
- Có hàm/macro trả lỗi dùng chung cho cả 2 đường (tránh lặp `RETURN_API_RESPONSE`).

### D4. Test
- Unit `tests/unit/manager/test_subnet_scan.cpp`:
  - ONVIF: server TCP cục bộ accept rồi im lặng (hoặc trả rất chậm); `searcher.timeout_sec = 2` →
    `discovery_device` trả `CODE_DEVICE_TIMEOUT` trong ≤ ~2.5 s. Thêm case server chỉ chậm ở lệnh
    sau `GetDeviceInformation` để kiểm tra `deadlineExceeded()` khi `connect()` vẫn "thành công".
  - RTSP: fake `FFmpegProbe::probe` sẵn có → kiểm tra timeout truyền vào ≤ hạn − grace.
  - Không hạn (OnvifControl dùng ngoài discovery) giữ 10 s/lệnh: kiểm tra `setDeadline` không gọi thì
    timeout soap không đổi.
- E2E: `tests/integration/stress_vms/onvif_mock.py` có tham số latency — chạy mock với latency lớn,
  `curl` discovery: nhận 504/905006 trong ~10 s, socket không bị đóng trước; camera mock bình thường
  vẫn trả đủ profile; `subnetScan` dải có 1 IP chậm vẫn xong.
- `ctest --test-dir build-test -R unit.` toàn bộ.

### D5. Docs
- `docs/threading-blocking-work.md` (bảng config dòng ~381): ghi `searcher.timeout_sec` = hạn tổng
  discovery/IP của subnetScan.
- Tài liệu API discovery/subnetScan: thêm lỗi 905006 `CODE_DEVICE_TIMEOUT`.

## Rủi ro / lưu ý
- Camera ONVIF thật chậm (NVR nhiều kênh, nhiều profile) có thể không kịp 10 s → giờ báo timeout thay
  vì đợi lâu rồi thành công. Nếu gặp thì tăng `searcher.timeout_sec`; giữ < `http.keepAliveSecond`
  (15 s), vượt mức đó phải tăng cả keepAlive.
- `transfer_timeout` âm (µs) — xác nhận trong `stdsoap2.c` bản 2.8.133 trước khi dựa vào.
- Nhánh speaker: probe nền vẫn có thể chạy hết các brand sau khi đã trả timeout (chỉ tốn HTTP poller,
  không giữ thread blocking).
