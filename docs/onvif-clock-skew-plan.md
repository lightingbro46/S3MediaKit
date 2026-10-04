# Plan: camera ONVIF lệch giờ — đo lại, gửi lại, báo đúng nguyên nhân

Ngày lập: 2026-10-04. Nhánh: `feature/onvif-http-digest` (tiếp sau 7be76e26).
Trạng thái: Q1–Q4 đã chốt 2026-10-04 (mục 8); đã code, kết quả ở mục 9; chưa commit.

## 1. Bối cảnh

WS-UsernameToken ký bằng `Created` (thời điểm tạo request). Camera so `Created` với **đồng hồ của nó**; lệch quá
ngưỡng của hãng thì từ chối bằng SOAP fault (`ter:NotAuthorized`, `wsse:FailedAuthentication`,
`wsse:MessageExpired`…), thường kèm HTTP 400/401 — **giống hệt** khi sai mật khẩu.

Hiện trạng (sau 93235630, 7be76e26):

| Việc | Ở đâu | Hạn chế |
|---|---|---|
| Đọc giờ camera | `getCameraTime()`: `GetSystemDateAndTime` không cần xác thực (ONVIF Core: PRE_AUTH) | Camera không theo chuẩn, bắt xác thực cả lệnh này, thì không đọc được |
| Ký theo giờ camera | `setCredentials()`: lệch ≥ 5 s thì ký `_at(cam_time)` + hook sửa `wsu:Created` | Độ lệch lấy từ cache 300 s (`onvif.clockOffsetTtlSec`) |
| Bị từ chối | `reportError()` xoá cache độ lệch | **Lệnh đang chạy trả lỗi luôn** (905004); chỉ lệnh sau mới ký đúng |
| Phân loại | `classifyFailure()` → `Unauthorized` (905004) | Không tách lệch giờ với sai mật khẩu |
| Đặt giờ camera | `setCameraTimeManual()`, chỉ gọi từ chu kỳ bảo trì `CameraController::onManager()` (sau connect thành công, hoặc khi đang kết nối mỗi `onvif.clockSyncIntervalSec`) | Camera chỉ nhận WS-Security mà lệch giờ thì **connect không bao giờ thành công**, nên giờ của nó **không bao giờ được đặt lại** |

Camera có chào HTTP Digest (chế độ `digest&ws-username token`) đã tự thoát nhờ 93235630: request bị từ chối vì
WS-Security nhận 401 + challenge, gửi lại bằng Digest (không phụ thuộc giờ). Plan này chủ yếu cho camera **chỉ
WS-Security** hoặc camera trả SOAP fault mà không kèm challenge HTTP.

## 2. Mục tiêu

1. **Gửi lại sau khi đo giờ:** lệnh bị từ chối kiểu xác thực/timestamp thì đo lại giờ camera; nếu độ lệch khác với độ
   lệch đã dùng để ký thì ký lại bằng giờ mới và gửi lại **một lần**. Lệnh thành công thì người dùng không thấy lỗi.
2. **Tách lệch giờ với sai mật khẩu:** thêm `DeviceFailure::ClockSkew` và mã `905008 CODE_DEVICE_CLOCK_SKEW`, response
   kèm giờ camera, giờ server và độ lệch để FE hiển thị.
3. **Dự phòng đọc giờ qua header `Date`:** camera bắt xác thực `GetSystemDateAndTime` thì lấy giờ từ header HTTP `Date`
   của chính response bị từ chối.
4. **Chỉ đặt giờ camera trong chu kỳ kiểm tra kết nối** của `CameraController` (yêu cầu của user). API điều khiển chỉ
   **đọc** giờ camera (`GetSystemDateAndTime`, không ghi gì lên camera), không bao giờ gọi `SetSystemDateAndTime`.
   Phát hiện lệch giờ ở API thì chỉ **đánh dấu** để chu kỳ kế tiếp đặt giờ, không chờ `clockSyncIntervalSec`.

Ngoài phạm vi: đổi cách `setCameraTimeManual()` tính `UTCDateTime` (`build_camera_utc`, giữ nguyên); `SoapUtil`
(`/index/api/getStreamUrl`) — client đó ký bằng giờ local không có offset, nhưng đã có Digest dự phòng; ghi chú ở mục 7.

## 3. Thiết kế

### 3.1 Đọc giờ camera (`getCameraTime`)

- Giữ nguyên đường chính `GetSystemDateAndTime`.
- **Dự phòng `Date`:** plugin `httpAuthPlugin` (đã bọc `fparsehdr`) ghi thêm header `Date` của mọi response vào
  `HttpAuthChallenge::date` (`time_t`, parse RFC 1123 bằng `strptime("%a, %d %b %Y %H:%M:%S GMT")` + `timegm`; parse
  hỏng thì 0). Reset ở `OnvifHttpAuth::apply()` như các field khác.
- `getCameraTime()` thất bại (lỗi HTTP/SOAP, không phải lỗi mạng) mà response có `Date` hợp lệ thì dùng nó làm
  `cam_time` (UTC), đánh dấu `_camTimeInfo.from_http_date = true`, `TZ` giữ giá trị cũ. Độ chính xác 1 s, đủ cho WS-Security.
- `Date` chỉ là dự phòng để **ký** và **báo lỗi**; `setCameraTimeManual()` vẫn cần `TZ` từ `GetSystemDateAndTime`
  như hiện tại (không có `TZ` thì không đặt giờ, giữ hành vi cũ).

### 3.2 Gửi lại sau khi đo giờ (`callWithAuth`)

Thêm nhánh thứ hai cạnh nhánh 401 HTTP:

```cpp
int ret = call();
if (ret == 401 && _http_auth->learn(...)) { ...gửi lại bằng HTTP auth... }      // đã có
else if (ret != SOAP_OK && isWsseRejected() && resignWithFreshClock()) {
    ret = call();                                                             // mới, tối đa 1 lần
}
```

- `isWsseRejected()`: request **không** mang credential HTTP (`!_http_auth->sentCredentials()`) và lỗi là SOAP fault
  có marker xác thực/timestamp (`isAuthOrTimestampFault`), hoặc HTTP 401/400 không có challenge HTTP dùng được.
- `resignWithFreshClock()`: nhớ độ lệch đã dùng để ký (`_signed_offset`, ghi trong `setCredentials()`), gọi
  `getCameraTime()` (bỏ qua cache), tính độ lệch mới. Chỉ gửi lại khi `|mới − đã ký| ≥ 5 s` (cùng ngưỡng
  `need_update_time` hiện có): độ lệch không đổi thì ký lại cũng y như cũ, gửi lại vô ích và tốn thêm một lần sai
  mật khẩu (camera có thể khoá tài khoản). Gửi lại: `setCredentials()` (ký theo độ lệch mới) rồi `call()`.
- **Không** gọi `SetSystemDateAndTime` ở đây.
- Chi phí: chỉ phát sinh khi camera đã từ chối; camera bình thường không thêm request.

### 3.3 Phân loại `ClockSkew`

`DeviceFailure` thêm `ClockSkew`. Sau lần gửi cuối cùng, nếu vẫn bị từ chối kiểu xác thực/timestamp:

| Điều kiện | Kết quả |
|---|---|
| Đo được giờ camera và `|giờ camera − giờ server| ≥ onvif.clockSkewThresholdSec` (Q1, đề xuất 30 s) | `ClockSkew` |
| Ngược lại (lệch nhỏ, hoặc không đo được giờ) | `Unauthorized` như hiện tại |
| Fault là `wsse:MessageExpired` (camera nói rõ timestamp hết hạn), bất kể độ lệch | `ClockSkew` |

Lý do vẫn báo `ClockSkew` dù đã ký theo giờ camera mà vẫn hỏng: có camera so `Created` với UTC "thật" nhưng lại báo
`UTCDateTime` sai theo múi giờ cấu hình, nên ký theo giờ nó báo vẫn bị từ chối. Đánh đổi: camera vừa sai mật khẩu vừa
lệch giờ ≥ 30 s sẽ báo `ClockSkew` trước; chu kỳ bảo trì đặt lại giờ, lần sau sẽ ra `905004` đúng.

`OnvifControl` giữ thêm (atomic, đọc được từ thread khác):

```cpp
std::atomic<time_t> _skew_camera_time {0};  // giờ camera UTC lúc đo, 0 = không có
std::atomic<time_t> _skew_measured_at {0};  // giờ server lúc đo
ClockSkewInfo clockSkewInfo() const;        // {camera_time, server_time, offset_sec, from_http_date}
bool clockSyncWanted() const;               // có lệch giờ cần chu kỳ bảo trì xử lý
```

### 3.4 Đặt giờ chỉ trong chu kỳ kiểm tra kết nối (`CameraController::onManager`)

Đặt giờ vẫn chỉ ở `onManager()` (timer của controller, chạy qua `postDeviceTask`), thêm hai chỗ:

1. **Camera đang kết nối:** `sync_clock` hiện chỉ đúng khi quá `clockSyncIntervalSec` (3600 s). Thêm điều kiện
   `onvif_ctr->clockSyncWanted()` (một API đã phát hiện lệch) để chu kỳ **kế tiếp** (≤ 60 s) đặt giờ luôn, rồi xoá cờ.
2. **Connect thất bại vì `ClockSkew`:** hiện không bao giờ đặt giờ vì chưa connect được. Thêm: sau `connect()` thất bại
   với `lastFailure() == ClockSkew`, gọi `setCameraTimeManual()` (đã tự đo giờ, tự ký theo giờ camera, tự gửi lại khi
   401 HTTP), chu kỳ sau connect lại. `setCameraTimeManual()` cần `_proxyDevice` mà `connect()` thất bại đã
   `disconnect()`, nên thêm một biến thể dùng `soap` cục bộ + endpoint `device_service` (giống cách hàm đang tạo
   `local_soap`), không phụ thuộc trạng thái kết nối. Giới hạn: mỗi chu kỳ (60 s) tối đa một lần.

Nếu đặt giờ cũng bị từ chối (camera thật sự sai mật khẩu) thì log cảnh báo, lần connect sau vẫn báo đúng nguyên nhân.

### 3.5 Mã lỗi và response

- `WebApiErrCode.h`: `XX(CODE_DEVICE_CLOCK_SKEW, "Device clock is out of sync with the server", 403, 905008)`.
  403 cùng lý do với 905004 (không dùng 401).
- `CameraController::failureCode`: `ClockSkew` → `CODE_DEVICE_CLOCK_SKEW`; `controllerNotReady()` thêm lý do
  `"device clock is out of sync"`.
- **Dữ liệu kèm theo:** khi code = 905008, `runDeviceCommand` (`WebApiControl.cpp`) điền `val["data"]`:

```json
{ "code": 905008, "msg": "...",
  "data": { "cameraTime": "2026-10-04T08:12:03Z", "serverTime": "2026-10-04T08:24:41Z",
            "clockOffsetSec": -758, "source": "GetSystemDateAndTime" | "HttpDate" } }
```

  Đường đi: `SockException` không mang dữ liệu, nên `respond` (đã chạy trên request poller) đọc
  `clockSkewInfo()` qua `GenericRtspCameraImp` → `CameraController` → `OnvifControl` (atomic, không chặn). Cần
  thêm getter `clockSkewInfo()` ở `GenericRtspCameraImp`/`CameraController`.
- `RETURN_API_RESPONSE` hiện chỉ đặt code/msg: điền `val["data"]` trước khi gọi macro (macro giữ nguyên `val`).

### 3.6 Config (đăng ký cạnh `OnvifCtrl` trong `OnvifDeviceControl.cpp`)

| Key | Mặc định | Ý nghĩa |
|---|---|---|
| `onvif.clockSkewThresholdSec` | 30 (Q1) | Độ lệch tối thiểu để bị từ chối được báo là `ClockSkew` thay cho `Unauthorized` |

Ngưỡng 5 s để gửi lại (3.2) giữ là hằng số, trùng ngưỡng ký theo giờ camera đã có.

## 4. Các bước

| Bước | Nội dung | Xong khi |
|---|---|---|
| P0 | `onvif_mock.py`: `--wsse-check` kiểm WS-Security thật (PasswordDigest = Base64(SHA1(nonce+Created+pass)) và `|Created − giờ mock| ≤ --wsse-window-sec`, sai thì fault `ter:NotAuthorized` HTTP 400); `--clock-offset-sec N` làm giờ mock (GetSystemDateAndTime, header `Date`) lệch N giây; `--time-requires-auth` bắt xác thực `GetSystemDateAndTime`; `SetSystemDateAndTime` cập nhật offset của thiết bị | Tái hiện: mock lệch 600 s, connect thất bại mãi |
| P1 | `Date` header trong plugin + dự phòng trong `getCameraTime()` | Unit: camera giả bắt xác thực GetSystemDateAndTime vẫn đo được giờ |
| P2 | `resignWithFreshClock()` + nhánh gửi lại trong `callWithAuth()` | Unit: camera giả lệch giờ đổi giữa chừng, lệnh thành công, đúng 1 lần gửi lại; sai mật khẩu không gửi lại (độ lệch không đổi) |
| P3 | `ClockSkew`, `clockSkewInfo()`, config ngưỡng, mã 905008, `failureCode`/`controllerNotReady` | Unit phân loại (bảng 3.3) |
| P4 | `onManager()`: cờ `clockSyncWanted()`, đặt giờ khi connect thất bại vì `ClockSkew` (biến thể không cần kết nối) | Unit `unit_camera_controller` với FakeOnvifControl: đặt giờ chỉ từ timer, không từ PTZ |
| P5 | `runDeviceCommand` điền `data` cho 905008; docs `api-device-control-errors.md`, `conf/config.ini` | — |
| P6 | E2E mock (mục 5) | Bảng kết quả mục 9 |
| P7 | Camera thật chỉ WS-Security, chỉnh giờ camera lệch 10 phút | — |

## 5. Kiểm thử

Unit (`unit_onvif_http_auth`, FakeCamera thêm chế độ WS-Security có cửa sổ thời gian):
1. Lệch 600 s, cache độ lệch còn hạn nhưng sai → lệnh đầu tiên thành công nhờ gửi lại; số request = 1 thất bại + 1
   GetSystemDateAndTime + 1 thành công.
2. Sai mật khẩu, giờ đúng → không gửi lại, `Unauthorized`.
3. Sai mật khẩu, lệch 600 s nhưng camera nhận chữ ký theo giờ nó → ký lại vẫn hỏng → `ClockSkew` (đánh đổi ở 3.3).
4. `wsse:MessageExpired` lệch 0 s → `ClockSkew`.
5. GetSystemDateAndTime bắt xác thực, header `Date` lệch 600 s → đo được, ký lại thành công.
6. Không có `Date` và không đọc được giờ → `Unauthorized`, không gửi lại.

`unit_camera_controller`: PTZ thất bại `ClockSkew` → không có `setCameraTimeManual` trong lệnh PTZ; chu kỳ
`onManager` kế tiếp gọi đúng 1 lần dù chưa tới `clockSyncIntervalSec`; connect thất bại `ClockSkew` → chu kỳ gọi
đặt giờ, chu kỳ sau connect thành công.

E2E (`run_stress.sh` + `ONVIF_MOCK_ARGS="--wsse-check --clock-offset-sec 600"`, Digest tắt):
- Trước sửa: 30 camera không connect được.
- Sau sửa: chu kỳ đầu báo `905008` kèm `data`, chu kỳ sau đặt giờ, camera connect; số `SetSystemDateAndTime` chỉ đến
  từ timer (đếm theo thời điểm, không trùng lúc gọi PTZ).
- Đổi offset mock khi đang kết nối → PTZ vẫn thành công (gửi lại), chu kỳ kế tiếp đặt giờ.

## 6. Rủi ro

| Rủi ro | Xử lý |
|---|---|
| Gửi lại làm camera đếm thêm một lần sai mật khẩu (khoá tài khoản) | Chỉ gửi lại khi độ lệch đo được khác độ lệch đã ký ≥ 5 s; sai mật khẩu + giờ đúng không gửi lại |
| `ClockSkew` che sai mật khẩu khi cả hai cùng xảy ra | Chấp nhận (3.3); chu kỳ bảo trì sửa giờ rồi báo đúng 905004 |
| Header `Date` của camera sai múi giờ (gửi giờ local ghi "GMT") | Chỉ dùng khi không có GetSystemDateAndTime; nếu ký theo nó vẫn hỏng thì phân loại như 3.3, không lặp |
| Đặt giờ khi connect thất bại đụng camera không muốn bị đổi giờ | Giữ nguyên điều kiện hiện có của `setCameraTimeManual()` (lệch ≥ 10 s, có `TZ`); Q3 có hỏi thêm cờ tắt |
| Thêm request trên poller | Không: mọi đo/gửi lại chạy trên device queue như lệnh gốc; response API đọc atomic |

## 7. Không thay đổi

- API điều khiển không bao giờ gọi `SetSystemDateAndTime`.
- `setCameraTimeManual()` giữ cách tính `UTCDateTime`; chỉ thêm biến thể không cần `_proxyDevice`.
- `SoapUtil` (getStreamUrl) ký bằng giờ local; nếu cần sẽ làm riêng (đo `GetSystemDateAndTime` trước khi ký).

## 8. Câu hỏi đã chốt (2026-10-04)

Q1 = 30 s, Q2 = có, Q3 = thêm `onvif.clockSyncEnable` (mặc định 1, toàn cục), Q4 = đúng: API được **đọc** giờ
camera để phân biệt sai mật khẩu với lệch giờ, không được **đặt** giờ.

- **Q1.** Ngưỡng `onvif.clockSkewThresholdSec` mặc định 30 s? (Ít hơn dễ báo nhầm sai mật khẩu thành lệch giờ; nhiều
  hơn thì camera có cửa sổ hẹp lệch 10–30 s sẽ báo 905004.)
- **Q2.** Response 905008 kèm `data.cameraTime/serverTime/clockOffsetSec/source` như 3.5? (Đề xuất: có.)
- **Q3.** Có cần cờ theo camera hoặc toàn cục để **tắt** việc server đặt giờ camera (camera đồng bộ NTP riêng)? Hiện
  code luôn đặt khi lệch ≥ 10 s. Đề xuất: thêm `onvif.clockSyncEnable` (mặc định 1) toàn cục, chưa cần theo camera.
- **Q4.** API đọc giờ camera (`GetSystemDateAndTime`, chỉ đọc) trong lúc gửi lại ở 3.2 là chấp nhận được? Yêu cầu
  là không **đặt** giờ trong API; plan hiểu là đọc được phép.

## 9. Kết quả (2026-10-04)

Khác/bổ sung so với thiết kế:
- **Cờ theo camera** (user yêu cầu thêm): `CameraOption::enableTimeSync` (mặc định `true`), đọc từ
  `cameraAdvanceConfig.onvif.enableTimeSync`, lưu trong thống kê camera (JSON + key-value; dữ liệu cũ thiếu key = `true`).
  Chu kỳ bảo trì chỉ đặt giờ khi `onvif.clockSyncEnable=1` **và** `enableTimeSync=true`.
- **Thước đo lệch giờ** cho `ClockSkew`/`clockOffsetSec`/cờ cần đặt giờ là `cam_sys_time − giờ server`, cùng thước đo
  `setCameraTimeManual()` dùng (server giữ giờ hiển thị trên camera bằng giờ server, `build_camera_utc`). Ký
  WS-Security vẫn dùng độ lệch UTC thô. Hệ quả: camera TZ khác server mà UTC đúng vẫn bị coi là lệch (theo đúng chính
  sách đặt giờ có sẵn); E2E phải chạy server cùng TZ với mock.
- `setCameraTimeManual()` chạy được khi chưa kết nối: dùng `LocalDeviceSoap` riêng, đọc giờ qua `readCameraClock()`,
  không đụng `_m_soap` và không ghi đè `lastFailure()`.
- Header `Date` dùng cả trong `getCameraTime()` (ký lệnh thường), không chỉ khi gửi lại.
- **Sửa thêm hook timestamp**: hook chỉ ghi lại `wsu:Created/Expires` trong `wsu:Timestamp`; trước đây nó ghi đè cả
  `Created` của UsernameToken (đã ký theo giờ camera), nếu qua ranh giới giây thì chữ ký sai.
- Response `905008` có `data` cả ở nhánh "controller chưa sẵn sàng".

Kiểm thử:
- Unit: `unit_onvif_http_auth` 24 case (thêm phân loại ClockSkew, camera giả WS-Security có đồng hồ riêng: đồng hồ
  nhảy → ký lại 1 lần thành công; sai mật khẩu giờ đúng → không gửi lại, 905004; sai mật khẩu lệch 600 s → ClockSkew;
  header Date khi camera không cho đọc giờ; không đọc được giờ → Unauthorized; đặt giờ khi chưa kết nối), lặp 30 lần
  không lỗi. `unit_camera_controller` 42 case (thêm 5: connect hỏng vì lệch giờ → chu kỳ đặt giờ; lỗi khác không đặt;
  lệnh PTZ chỉ đánh dấu, chu kỳ kế tiếp đặt đúng 1 lần; cờ camera và cờ toàn cục tắt). ctest unit 225/225.
- E2E `run_stress.sh smoke` + `onvif_mock.py --wsse-check` (chỉ WS-Security), server `TZ=UTC`:
  1. Mọi camera lệch 600 s: kết nối bằng giờ camera, 0 lần bị từ chối, chu kỳ đặt giờ 30 camera; PTZ `0`.
  2. Đồng hồ nhảy 900 s khi đang kết nối: PTZ `0` (đo lại + ký lại); số lần đặt giờ của camera ngay sau PTZ không đổi,
     chu kỳ kế tiếp đặt 1 lần.
  3. Sai mật khẩu + lệch 900 s: `905008`, `data.clockOffsetSec=900`, `source=GetSystemDateAndTime`.
  4. Sai mật khẩu, giờ đúng: `905004`.
  5. Camera bắt xác thực cả GetSystemDateAndTime, lệch 1200 s: PTZ `0` (27 lần đọc giờ bị từ chối, không lệnh nào
     khác bị từ chối → giờ lấy từ header Date).
- Lần chạy đầu ở TZ máy (+0700) bước 4 ra `905008` thay vì `905004`: mock báo TZ `UTC0`, theo chính sách đặt giờ
  camera bị coi là lệch −7 giờ (đúng chính sách, sai kỳ vọng của test).

Còn lại: camera thật chỉ WS-Security (P7); speaker chưa áp dụng.
