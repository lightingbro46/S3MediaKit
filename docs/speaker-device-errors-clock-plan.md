# Plan: mã lỗi thiết bị và xử lý lệch giờ cho loa

Ngày lập: 2026-10-04, nhánh `feature/psi-controller` (đã merge develop tại 56b21d0c).

Camera đã có các mã lỗi 905004–905008 ([api-device-control-errors.md](api-device-control-errors.md)) và cơ chế
đồng bộ giờ ([onvif-clock-skew-plan.md](onvif-clock-skew-plan.md)). Loa (`SpeakerController`) dùng chung lớp điều
khiển `DeviceControl` (ONVIF/ISAPI/PSIA) nên đã có sẵn `lastFailure()`, `clockReading()`,
`takeClockSyncWanted()`. Tuy vậy, phía loa chưa dùng tới các hàm này.

## 1. Hiện trạng loa

| Vấn đề | Ở đâu | Hậu quả |
|---|---|---|
| Loa **không bao giờ đặt giờ thiết bị**: không gọi `setCameraTimeManual()` khi connect, không có chu kỳ đặt giờ | `SpeakerController::onManager`, `publishConnected` | Loa ONVIF bị lệch giờ thì không qua được WS-Security, nên **không bao giờ kết nối được**. Đến khi người dùng tự chỉnh giờ trên loa thì log vẫn chỉ ghi "sai mật khẩu" |
| Lỗi "chưa sẵn sàng" luôn trả `905002` | `GenericIPSpeakerImp::setAudioVolume`, `playAudioFile` ("Speaker controller is not ready") | FE không phân biệt được lỗi sai mật khẩu, lỗi mạng hay lệch giờ |
| `SpeakerController::vendorDevice` trả `"API controller is not ready"` qua `OnDeviceResult(bool, string)` | `SpeakerController.cpp` | Lỗi này bị gói thành `CODE_SET_AUDIO_VOLUME_FAILED` hoặc `CODE_PLAY_AUDIO_FAILED`, nên nguyên nhân bị mất |
| Lỗi từ API hãng (Bosch) chỉ là chuỗi, ví dụ `"Login HTTP 401"` | `ext-plugin/Bosch/BoschIpSpeaker.cpp` | Không ra được mã 905004/905005 |
| `/device/speaker/playAudioFile` luôn trả 200; kết quả từng loa chỉ ghi log | `WebApiControl.cpp` | Mã lỗi mới không tới được FE qua API này |
| Kết quả dò AUTO thất bại không giữ lại nguyên nhân | `SpeakerController::onProbeDone` | Loa AUTO chưa có giao thức thì không biết lý do |

Phía camera đã làm, phía loa sẽ làm theo cùng mẫu:
- `controllerNotReady()` và `_probe_failure`;
- `publishConnected(..., sync_clock)`;
- `takeClockSyncWanted`, `_last_clock_sync`;
- `probeControlProtocols(..., sync_clock_on_skew)`;
- `CameraOption::enableTimeSync`.

## 2. Câu hỏi đã chốt

User chấp nhận cả ba đề xuất ngày 2026-10-04: Q1 (a), Q2 không đổi `playAudioFile`, Q3 làm SP4.

- **Q1. Bật đồng bộ giờ cho loa mặc định?** Trước giờ loa không bao giờ bị đặt giờ. Có hai lựa chọn:
  - (a) Giống camera: `enableTimeSync` mặc định `true`. Server đặt giờ khi connect và theo `onvif.clockSyncIntervalSec`.
  - (b) Mặc định `true` nhưng **chỉ đặt giờ khi phát hiện lệch** (connect bị từ chối vì ClockSkew, hoặc lệnh báo
    `takeClockSyncWanted`), không đặt định kỳ.

  **Đề xuất (a)** để camera và loa hành xử giống nhau, và vì vẫn tắt được bằng `onvif.clockSyncEnable=0` (toàn server)
  hoặc `speakerAdvanceConfig.onvif.enableTimeSync=false` (từng loa).
- **Q2. `playAudioFile` có trả kết quả từng loa không?** API này nhận nhiều `deviceIds` và hiện trả 200 ngay.
  **Đề xuất: không đổi hợp đồng API trong plan này.** Mã mới sẽ đi qua `setAudioVolume`, `/device/reconnect` và
  webhook trạng thái. Đổi `playAudioFile` để trả mảng kết quả là việc riêng, cần FE đồng ý.
- **Q3. Phân loại lỗi của API Bosch (SP4)?** Lỗi login HTTP 401/403 → 905004, lỗi TCP → 905005. **Đề xuất làm**, vì
  diff nhỏ và chỉ nằm trong plugin.

## 3. Các bước

### SP1. Mã lỗi khi loa chưa sẵn sàng

- `SpeakerController` thêm:
  - `DeviceFailure _probe_failure`, được ghi trong `onProbeDone` (lấy `result.failure` của `probeControlProtocols`);
  - `toolkit::SockException notReadyError() const`.

  `notReadyError()` dùng chung logic với `CameraController::controllerNotReady()`. Để không chép code, chuyển phần
  ánh xạ `DeviceFailure` → (lý do, `ApiErrCode`) thành hàm tự do trong `manager/Camera/CameraController.cpp`, khai báo
  ở một header chung, ví dụ `manager/Common/DeviceFailureCode.h`. Đây là header nhỏ: `failureCode()` cùng
  `deviceNotReadyError(DeviceFailure)`. Header nằm ở manager vì `ApiErrCode` ở `server/WebApiErrCode.h`, và
  `CameraController.cpp` cũng đã include file đó.
- `GenericIPSpeakerImp::setAudioVolume` / `playAudioFile`: nếu controller chưa `ready` thì trả `notReadyError()`
  thay vì `CODE_DEVICE_OFFLINE`.
- `vendorDevice()` để nguyên: lời gọi từ `GenericIPSpeakerImp` đã được chặn ở bước trên. Nhánh `!_vendor_ctr` vẫn
  là lỗi riêng của thao tác.
- Unit test (fixture `SpeakerProtocolTest`):
  - `FakeProtocolControl` thêm `lastFailure` đặt được từ `ProtocolWorld`;
  - loa AUTO dò hỏng với `Unauthorized` thì `notReadyError()` trả 905004;
  - loa khoá ONVIF connect hỏng `Unreachable` thì trả 905005.

### SP2. Đồng bộ giờ cho loa (theo Q1)

- `SpeakerOption::enableTimeSync = true`:
  - đọc từ `speakerAdvanceConfig.onvif.enableTimeSync` trong `server/Manager.cpp`, giống camera ở dòng ~717;
  - thêm vào `operator==`;
  - `SpeakerStatistic` lưu/đọc JSON, dữ liệu cũ thiếu key thì coi là `true`;
  - xuất ra `makeSpeakerOptionJson`.
- `SpeakerController`:
  - thêm `_enableTimeSync` và `_last_clock_sync`;
  - `onManager()` tính `time_sync = onvif.clockSyncEnable && _enableTimeSync`, `sync_wanted =
    device_ctr->takeClockSyncWanted()` và `sync_clock` giống camera;
  - nhánh online (`probeConnection` thành công) thì đặt giờ khi `sync_clock`;
  - `publishConnected(device_ctr, ctx)` thêm `sync_clock` và gọi `setCameraTimeManual()` trước khi đọc capabilities;
  - connect hỏng với `lastFailure() == ClockSkew` thì đặt giờ ngay (khi `time_sync`);
  - `startProtocolProbe` truyền `time_sync` vào `probeControlProtocols(..., sync_clock_on_skew)`.
- Đổi `enableTimeSync` không cần kết nối lại: cập nhật tại chỗ như camera (nhánh "connection unchanged" của
  `setupController`).
- Loa có plugin hãng (Bosch) đặt giờ qua ONVIF bằng tài khoản `speaker.onvifUsername` mà plugin tạo. **Rủi ro:**
  `SetSystemDateAndTime` cần quyền Administrator trong ONVIF. Nếu tài khoản đó không đủ quyền, lệnh đặt giờ thất bại
  và chỉ ghi `WarnL`; không được làm hỏng connect. Phải kiểm trên thiết bị thật.
- Unit test:
  - `FakeProtocolControl.setCameraTimeManual` ghi sự kiện `"settime X"`;
  - các case giống 5 case camera: ClockSkew khi connect thì đặt giờ; lỗi khác thì không; cờ từ lệnh thì chu kỳ sau
    mới đặt; tắt theo loa; tắt toàn server;
  - thêm 1 case dò AUTO bị ClockSkew thì đặt giờ và **không** chuyển sang ISAPI/PSIA.

### SP3. Trả 905008 kèm giờ thiết bị cho loa

- `GenericIPSpeakerImp::clockReader()` → `SpeakerController::clockReader()`, giống camera.
- `setAudioVolume` trong `WebApiControl.cpp`: khi `ex.getCustomCode() == CODE_DEVICE_CLOCK_SKEW` thì gắn
  `data = makeClockSkewJson(reader())`, giống `runDeviceCommand`.
- `/device/reconnect` (camera và loa): `ReconnectResult` thêm `failure`. Response thêm `data.code` theo
  `failureCode(failure, 0)`, và kèm giờ khi là 905008. FE có sẵn `error` (`AUTH_FAILED`…), field mới chỉ bổ sung.

### SP4. Lỗi của API Bosch (theo Q3)

- `BoschIpSpeaker`:
  - login trả HTTP 401/403 → `cb(false, ...)`, đồng thời ghi lại `DeviceFailure::Unauthorized`;
  - `SockException` khi kết nối → `Unreachable`, khi quá thời gian → `Timeout`.

  Interface `IDevice` thêm `virtual DeviceFailure lastFailure() const { return DeviceFailure::None; }`. `ext-plugin`
  không phụ thuộc `manager`, nên khai báo enum `DeviceFailure` đặt ở `manager/Common/DeviceControlTypes.h` phải include
  được từ plugin. Cần kiểm tra include path của target `ext-plugin`. Nếu không include được thì dùng `int` và chuyển
  đổi ở `SpeakerController`.
- `runVendorOp`: lỗi `connect` của plugin thì đổi mã `CODE_SET_AUDIO_VOLUME_FAILED`/`CODE_PLAY_AUDIO_FAILED` thành
  `failureCode(device->lastFailure(), ...)`. Để làm được, `OnDeviceResult` phải mang mã, mà kiểu này đang là
  `(bool, string)`. **Cách diff nhỏ nhất:** `GenericIPSpeakerImp` đọc `controller->lastVendorFailure()` (giá trị
  atomic `SpeakerController` ghi trong `runVendorOp`) khi nhận `ok=false`, không đổi chữ ký callback.
- Unit test: chỉ test hàm phân loại (thuần dữ liệu). Không dựng HTTP Bosch giả.

### SP5. Docs và config

- [api-device-control-errors.md](api-device-control-errors.md):
  - bảng "API áp dụng" thêm `/device/setAudioVolume` và `/device/reconnect` (`data.code`);
  - ghi rõ `playAudioFile` không trả các mã này (Q2).
- [camera-control-protocol.md](camera-control-protocol.md) mục Loa: thêm đồng bộ giờ và `enableTimeSync` của loa.
- `conf/config.ini` `[onvif] clockSyncEnable`: sửa chú thích thành "camera và loa".

## 4. Kiểm thử thật

- `onvif_mock.py --wsse-check --clock-offset-sec 900` đóng vai loa ONVIF (backchannel dùng
  `rtsp_backchannel_mock.py`):
  - trước khi sửa: loa không bao giờ kết nối;
  - sau khi sửa: chu kỳ đầu đặt giờ, chu kỳ sau kết nối được;
  - `setAudioVolume` khi loa chưa sẵn sàng trả 905008 kèm `clockOffsetSec`.
- `psia_mock.py --dialect isapi` với sai mật khẩu: `/device/reconnect` trả `data.code=905004`.
- Hồi quy: `ctest -R unit.` toàn bộ; loa ONVIF đúng giờ vẫn kết nối như cũ, và không có thêm request
  `SetSystemDateAndTime` nào ngoài lúc connect và đúng chu kỳ.
- Thiết bị thật: loa Bosch (quyền đặt giờ của tài khoản ONVIF plugin tạo, SP4), loa Hikvision ISAPI.

## 5. Ngoài phạm vi

- Lỗi của backchannel (RTSP 401 với loa ONVIF, ISAPI two-way audio 401/403) vẫn chạy bất đồng bộ qua
  `onFailed` và chỉ ghi log. Muốn đưa tới FE thì cần `playAudioFile` trả kết quả (Q2).
- Đổi hợp đồng `playAudioFile`.

## 6. Kết quả

(ghi theo từng bước khi làm)
