# Plan: điều khiển camera bằng PSIA bên cạnh ONVIF (mỗi camera chỉ dùng một giao thức)

Ngày lập: 2026-10-03. Nhánh: `feature/psi-controller` (tách từ `develop`).
Trạng thái: Q1–Q4 đã chốt 2026-10-03 (mục 8); đang code, tiến độ ở mục 9.

PSIA = Physical Security Interoperability Alliance, chuẩn IPMD/Common Metadata: REST qua HTTP, body XML, đường dẫn
`/PSIA/...`, xác thực HTTP Digest (Basic làm dự phòng). Code đã chừa chỗ ở `manager/Camera/CameraController.cpp:145`
(`// todo: support PSI controller`).

## 1. Mục tiêu

1. Camera điều khiển được bằng **ONVIF hoặc PSIA**: kết nối/kiểm tra sống, thông tin thiết bị, danh sách profile +
   URL luồng, đọc/ghi encoder (profile sync, video config rule), PTZ (continuous/relative/absolute/stop, preset,
   home, status), focus/iris, relay output, đồng bộ giờ.
2. **Mỗi camera tại một thời điểm chỉ có đúng một controller, một giao thức.** Không có chuyện PTZ đi PSIA trong khi
   profile sync đi ONVIF, cũng không có hai controller cùng đẩy cấu hình encoder vào một camera.
3. Chọn theo từng camera: `AUTO` (mặc định), `ONVIF`, `PSIA`. `AUTO` thử ONVIF trước, sau đó PSIA. Dò được giao
   thức nào thì **khoá** vào giao thức đó và chỉ dò lại theo quy tắc ở mục 4.2.
4. Camera ONVIF hiện có **không đổi hành vi**: thiếu field thì là `AUTO`, `AUTO` thử ONVIF trước, nên chạy y như hôm nay.

Ngoài phạm vi đợt này (đã chốt Q4):
- Phát lại thẻ nhớ / lấp khoảng hở từ SD (`MediaGapFillManager`, ONVIF Profile G). Camera chạy PSIA báo
  `supportsSdCardPlayback=false`.
- Audio backchannel, speaker (`SpeakerController` giữ nguyên ONVIF/plugin hãng).
- Sự kiện/metadata PSIA (`/PSIA/Metadata/stream`), motion phía camera.
- Dò PSIA trong `SubnetScan`.
- ISAPI của Hikvision (`/ISAPI/...`). Bảng đường dẫn trong `PsiaControl` để riêng một chỗ, thêm ISAPI về sau chỉ
  là thêm một bảng.

## 2. Vì sao chọn hướng này

- `CameraController` là điểm duy nhất gọi vào thiết bị camera. Mọi thao tác đi qua `_onvif_ctr` và chạy tuần tự trên
  `_device_queue` (`SerialTaskQueue` trên `BlockingExecutors::device()`). Đổi **kiểu** con trỏ đó sang một
  interface trung lập là đủ để ràng buộc "chỉ một giao thức" đúng về mặt cấu trúc: một controller, một con trỏ,
  một hàng đợi.
- Các struct `OnvifMediaProfile`, `OnvifPTZProfile`, `OnvifImageProfile`, `OnvifRelayOutputProfile`,
  `VideoEncoderConfig` đã là mô hình chung. `CameraStatistic`, JSON API trong `Manager.cpp` và video config rule đều
  đọc chúng. Cho PSIA điền vào **cùng các struct** thì không phải sửa logic profile sync, rule hay API. Tên `Onvif*`
  giữ nguyên, vì đổi tên tạo diff lớn mà không được gì.
- Không đặt PSIA trong `ext-plugin/` (`IDevice`/`DeviceFactory`). Interface đó chỉ có connect/createOnvifUser, phục
  vụ speaker theo hãng, quá mỏng để điều khiển camera.
- HTTP client: gSOAP đã link vào `ManagerKit` (`onvifcore`) và có sẵn `soap_GET/soap_PUT/soap_POST/soap_DELETE`
  (`stdsoap2.h:3195-3199`) cùng plugin Digest `httpda` (đã nằm trong `3rdpart/onvif/core/CMakeLists.txt`). Client
  này blocking, có timeout, chạy được trên pool `device()` như ONVIF. Không phải thêm thư viện.

## 3. Hiện trạng liên quan

| Thành phần | Vị trí | Ghi chú |
|---|---|---|
| Tạo controller | `CameraController::setupController` (`CameraController.cpp:89-163`) | Luôn `createOnvifControl(...)`; tạo lại khi `ControllerOption` đổi |
| Hook test | `CameraController::createOnvifControl` (virtual); fake ở `tests/unit/manager/test_camera_controller.cpp:159` | Đổi thành `createDeviceControl(protocol, …)` |
| Chu kỳ bảo trì | `CameraController::onManager` (timer 10 s, reconnect ≥ 60 s) | `probeConnection` / `connect` / `refreshMediaProfiles` / `setCameraTimeManual` |
| Hàm thiết bị đang dùng | `CameraController.cpp`, `MediaGapFillManager.cpp` | `connect, probeConnection, isConnected, isControlled, getDeviceInfo, getMediaProfilesInfo, refreshMediaProfiles, get/setVideoEncoderConfigByToken, getPTZProfile, enablePTZ, PTZ_{ContinuousMove,RelativeMove,AbsoluteMove,Stop,GotoPreset,GotoHomePosition,GetStatus,AuxiliaryCommand}, Imaging_Focus{Move,Stop}, Iris, enableFocus/Iris/RelayOutput, Relay_SetOutputState, getImageProfile, getRelayOutputProfiles, getAudio{Output,Input}Profile, getSDCardInfo, getRecordingInformation, findVideoSegments, getSoapErrMsg, setCameraTimeManual` |
| Kiểu gSOAP lọt ra ngoài | `PTZ_GetStatus` trả `tt__MoveStatus` | Đổi sang enum trung lập |
| Capabilities | `DeviceCapabilities` (`manager/Common/DeviceOption.h:124`) | Chỉ có `isOnvifDevice` |
| Nơi đọc `isOnvifDevice` | `server/Manager.cpp:1502,1547,1548,1572,2067`; `CameraStatistic.cpp:1196` (thêm preset người dùng) | |
| Lưu option camera | `fromJson(CameraOption&)` (`Manager.cpp:648`), `makeCameraOptionJson` (`Manager.cpp:1786`); `ResourceAdapter<CameraStatistic>` ghi kv vào `VmsResource` (`CameraStatistic.cpp:1383`, đọc lại ở `:1504`) | kv được đồng bộ cluster. **Không cần migration** |
| Lưu capabilities | `makeDeviceCapabilitiesJson`/`getDeviceCapabilities` (`CameraStatistic.cpp:361,539`) | Thêm field JSON |
| Nạp lại lúc khởi động | `GenericRtspCameraImp::setupController` (`GenericRtspCameraImp.cpp:76-101`) đưa preset + `stream_settings` từ statistic vào controller | Đưa thêm giao thức đã khoá |
| Khớp profile ↔ luồng | `CameraStatisticImp::getStreamVideoEncoderConfigMap` (`:986`), tìm token theo `full_url` (`:1288`) | So **nguyên chuỗi URL**: URL PSIA phải khớp URL luồng đã cấu hình (R3) |
| Chỉ ONVIF | `MediaGapFillManager` (SD), `SpeakerController` (backchannel), `SubnetScan` | Giữ `OnvifControl::Ptr` cụ thể |

## 4. Thiết kế

### 4.1 Interface `CameraDeviceControl`

File mới `manager/Control/CameraDeviceControl.h`, kế thừa `DeviceControl`:

```cpp
enum class ControlProtocol { NONE, ONVIF, PSIA };
enum class PtzMoveStatus { IDLE, MOVING, UNKNOWN };
// Vì sao connect()/probeConnection() hỏng; quyết định có dò sang giao thức khác hay không.
enum class ConnectError { NONE, NOT_THIS_PROTOCOL, AUTH_FAILED, UNREACHABLE, OTHER };

const char *controlProtocolToString(ControlProtocol p);       // "", "ONVIF", "PSIA"
ControlProtocol controlProtocolFromString(const std::string &s);

class CameraDeviceControl : public DeviceControl {
public:
    using Ptr = std::shared_ptr<CameraDeviceControl>;

    virtual ControlProtocol protocol() const = 0;
    virtual ConnectError lastConnectError() const = 0;
    virtual std::string lastError() const = 0;                 // thay getSoapErrMsg() ở CameraController

    virtual bool probeConnection() = 0;
    virtual bool isConnected() const = 0;
    virtual bool isControlled() const = 0;
    virtual bool setCameraTimeManual() = 0;

    virtual OnvifDeviceInfo getDeviceInfo() = 0;
    virtual OnvifMediaProfileMap getMediaProfilesInfo() = 0;
    virtual bool refreshMediaProfiles() = 0;
    virtual bool getVideoEncoderConfigByToken(const std::string &token, VideoEncoderConfig &config) = 0;
    virtual bool setVideoEncoderConfigByToken(const std::string &token, const VideoEncoderConfig &config) = 0;

    virtual OnvifPTZProfile getPTZProfile() = 0;
    virtual bool enablePTZ() = 0;
    virtual bool PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout = 0) = 0;
    virtual bool PTZ_RelativeMove(...) = 0;      /* đủ các overload đang dùng */
    virtual bool PTZ_AbsoluteMove(...) = 0;
    virtual bool PTZ_Stop(bool panTilt, bool zoom) = 0;
    virtual bool PTZ_GotoPreset(...) = 0;
    virtual bool PTZ_GotoHomePosition(...) = 0;
    virtual bool PTZ_AuxiliaryCommand(...) = 0;
    virtual PtzMoveStatus PTZ_GetStatus(float &pan, float &tilt, float &zoom) = 0;

    virtual OnvifImageProfile getImageProfile() = 0;
    virtual bool Imaging_FocusMove(...) = 0;     /* + FocusStop, Iris như OnvifControl */
    virtual std::vector<OnvifRelayOutputProfile> getRelayOutputProfiles() = 0;
    virtual bool Relay_SetOutputState(...) = 0;

    // Chỉ ONVIF có; PSIA dùng mặc định "không hỗ trợ".
    virtual SDCardInformation getSDCardInfo() { return SDCardInformation(); }
    virtual OnvifAudioOutputProfile getAudioOutputProfile() { return OnvifAudioOutputProfile(); }
    virtual OnvifAudioInputProfile getAudioInputProfile() { return OnvifAudioInputProfile(); }
};
```

Chữ ký từng hàm chép đúng từ `OnvifControl` hiện có. Bước P1 chốt danh sách bằng cách cho compiler báo những gì
`CameraController` còn gọi; không thêm API mới.

`OnvifControl`:
- Đổi base `DeviceControl` → `CameraDeviceControl`, thêm `override`. Những hàm đang non-virtual mà interface cần
  (`getDeviceInfo`, `isControlled`, `getImageProfile`, …) chuyển thành override, thân hàm giữ nguyên.
- `lastError()` trả `getSoapErrMsg()`.
- `PTZ_GetStatus(float&, float&, float&)` mới trả `PtzMoveStatus`. Bản trả `tt__MoveStatus` đổi tên thành
  `PTZ_GetMoveStatus` để `SubnetScan`/`SpeakerController` dùng tiếp nếu cần.
- `lastConnectError()`: phân loại theo kết quả `connect()`:
  - `SOAP_TCP_ERROR`/timeout → `UNREACHABLE`
  - SOAP fault `NotAuthorized` hoặc HTTP 401 → `AUTH_FAILED`
  - HTTP 404/405, body không phải SOAP, hoặc lỗi `SOAP_NO_TAG`/`SOAP_TAG_MISMATCH` ở `GetDeviceInformation`
    → `NOT_THIS_PROTOCOL`
  - còn lại → `OTHER`

### 4.2 Chọn giao thức: bảo đảm chỉ một trong hai

**Option.** `CameraOption` (kế thừa từ `DeviceOption`, nên `ControllerOption::from` lấy được) thêm:

```cpp
// Giao thức điều khiển thiết bị: "AUTO" | "ONVIF" | "PSIA". AUTO thử ONVIF rồi PSIA và khoá vào cái dò được.
std::string controlProtocol = "AUTO";
```

- `DeviceOption` + `ControllerOption` (+ `operator==`): đổi giao thức thì tạo lại controller qua nhánh "connection
  changed" có sẵn.
- JSON vào: `cameraAdvanceConfig.control.protocol` trong `fromJson` (`Manager.cpp:648`). Giá trị lạ hoặc thiếu
  → `AUTO`.
- JSON ra: `makeCameraOptionJson` (`Manager.cpp:1786`).
- kv `controlProtocol` trong `ResourceAdapter<CameraStatistic>` (ghi `:1383`, đọc `:1504`) để đồng bộ cluster.
- `CameraOption::operator==` thêm field.

**State trong `CameraController`** (chỉ đọc/ghi trên `_poller`):

```cpp
CameraDeviceControl::Ptr _device_ctr;              // thay _onvif_ctr; luôn tối đa một
ControlProtocol _locked_protocol = NONE;           // AUTO đã khoá vào đâu (đồng thời là giá trị đã lưu)
int _not_this_protocol_count = 0;                  // số lần connect hỏng kiểu NOT_THIS_PROTOCOL liên tiếp
```

**Quy tắc.**

| `controlProtocol` | Hành vi |
|---|---|
| `ONVIF` / `PSIA` | `setupController` tạo đúng controller đó. Không bao giờ thử giao thức kia; connect lỗi mãi thì báo offline kèm lỗi. |
| `AUTO`, chưa khoá | `setupController` **không** tạo controller. `onManager` gửi một task dò lên `_device_queue`, thử theo `camera.control_auto_order` (mặc định `onvif,psia`). Mỗi lượt: tạo controller → `connect()`. Thành công thì dừng. Lỗi `NOT_THIS_PROTOCOL` thì huỷ controller đó và thử giao thức kế tiếp. Lỗi `UNREACHABLE`/`AUTH_FAILED`/`OTHER` thì **dừng dò**, báo offline, 60 s sau thử lại từ đầu: camera mất mạng hay sai mật khẩu không được coi là "không có ONVIF". Controller thắng được đưa về `_poller` qua `weak_ptr` rồi mới gán vào `_device_ctr` và `_locked_protocol`. |
| `AUTO`, đã khoá | Chạy như chế độ cố định với `_locked_protocol`. Mở khoá và dò lại chỉ khi (a) `ControllerOption` đổi (cơ chế tạo lại có sẵn), hoặc (b) `connect()` hỏng `NOT_THIS_PROTOCOL` liên tiếp `camera.control_reprobe_failures` lần (mặc định 5, khoảng 5 phút). Mọi kiểu lỗi khác reset bộ đếm về 0. |

**Giữ khoá qua restart.**
- Giao thức đang dùng được ghi vào `DeviceCapabilities::controlProtocol`, lưu trong JSON capabilities của
  `CameraStatistic`.
- `GenericRtspCameraImp::setupController` gọi `_controller->setLastControlProtocol(...)` cùng chỗ đang nạp preset
  và `stream_settings`. `AUTO` có giá trị đã lưu thì vào thẳng trạng thái "đã khoá", không phải dò lại.

**Bảo đảm loại trừ.**
1. `_device_ctr` chỉ bị gán hoặc reset trên `_poller`.
2. Việc dò nằm trọn trong **một** task của `_device_queue`. Controller thua bị huỷ ngay trong task, trước khi thử
   cái kế tiếp. Vì thế không lúc nào có hai controller cùng nói chuyện với camera.
3. Lệnh người dùng (PTZ/preset/imaging/relay/profile) chụp `_device_ctr` lúc gửi, như `onvif_ctr` hiện nay. Lúc
   đang dò thì `_device_ctr` rỗng, lệnh trả `CODE_DEVICE_OFFLINE` như khi controller chưa sẵn sàng.
4. Chuyển giao thức (dò lại, hoặc option đổi) theo thứ tự:
   - `MediaGapFillManager::onManager(..., nullptr, ..., false)`
   - `_ready=false`, `onControllerReady(false, "Control protocol changed", nullptr)`
   - reset `_device_ctr`
   - rồi mới dò hoặc kết nối giao thức mới.
5. `MediaGapFillManager` và các nhánh audio chỉ nhận
   `std::dynamic_pointer_cast<OnvifControl>(_device_ctr)`. PSIA thì truyền `nullptr`.

**Khi giao thức đổi so với giá trị đã lưu (R2).** Token profile của PSIA (id kênh, ví dụ `101`) khác token ONVIF.
Khi controller mới connect với `protocol()` khác `_locked_protocol` cũ, xử lý trên `_poller`:
- Báo lỗi các rule đang chờ trong `_ruleConfigMap` bằng `reportRuleFailureIfPending(token,
  CODE_DEVICE_PROTOCOL_CHANGED, ...)`, rồi xoá `_ruleConfigMap` và `_profileConfigMap`. Baseline mới được seed lại
  từ camera ở lần profile sync đầu.
- Xoá preset người dùng không còn hợp lệ: preset ONVIF lưu toạ độ abs theo không gian ONVIF.
- Log `WarnL` một dòng có đủ camera, giao thức cũ và giao thức mới.
- Mã lỗi mới `CODE_DEVICE_PROTOCOL_CHANGED` thêm vào `server/WebApiErrCode.h` cạnh nhóm `9050xx`.

### 4.3 Capabilities và API

`DeviceCapabilities` thêm `std::string controlProtocol;` (`""`, `"ONVIF"`, `"PSIA"`). Field này vào `operator==`,
`makeDeviceCapabilitiesJson` và `getDeviceCapabilities`. `isOnvifDevice` giữ nghĩa "đang điều khiển bằng ONVIF".
Mỗi lần connect thành công, `CameraController` điền cả hai field.

`server/Manager.cpp` và `CameraStatistic.cpp`:
- `:1502`, `:1572`: nhánh điền thông tin thiết bị + `onvifProfiles` đổi điều kiện từ `isOnvifDevice` sang
  `!controlProtocol.empty()`. Tên field JSON giữ nguyên để client không phải đổi.
- `:1547` `enableOnvifProfileConfig` và `:2067` (video config rule): true khi có controller.
- `:1548` `supportsSdCardPlayback`: giữ chỉ ONVIF.
- `CameraStatistic.cpp:1196` (thêm preset người dùng): đổi sang "có controller".
- Thêm vào JSON capabilities/thông tin camera:
  - `controlProtocol`: giao thức đang dùng, rỗng khi chưa kết nối
  - `controlProtocolSetting`: `AUTO|ONVIF|PSIA`
- Docs: thêm mục `controlProtocol` vào tài liệu API camera (`docs/README.md` trỏ tới) và ghi rõ: camera PSIA vẫn
  trả profile trong field `onvifProfiles`.

### 4.4 `PsiaControl`

File mới `manager/Control/PsiaDeviceControl.{h,cpp}`, implement `CameraDeviceControl`. Blocking, chỉ chạy trên
`_device_queue` giống `OnvifControl`. Có `std::mutex` bảo vệ state đọc từ `_poller`, như `OnvifControl`.

**HTTP (`PsiaHttp`, nội bộ trong .cpp).**
- Một `struct soap*` mỗi controller, tạo trong `connect()`, huỷ trong `disconnect()` và destructor.
- Timeout `connect_timeout`/`recv_timeout`/`send_timeout` = `psia.request_timeout_ms`.
- Request: `soap_GET` / `soap_PUT(..., "application/xml")` / `soap_DELETE` rồi `soap_http_get_body`, rồi
  `soap_end_recv`. Mã HTTP lấy từ `soap->status`/`soap->error`.
- Digest: `soap_register_plugin(soap, http_da)`. Gặp 401 thì `http_da_save(soap, &info, soap->authrealm, user,
  pass)` và gửi lại một lần. Các request sau dùng `http_da_restore`. Camera chỉ có Basic thì dùng
  `soap->userid/passwd`.
- XML: pugixml (`src/Onvif/pugixml.hpp`). Đọc theo tên local, bỏ namespace, vì các hãng khai namespace khác nhau.
  Ghi: đọc tài liệu gốc, sửa node, `PUT` lại cả tài liệu, để giữ các field hãng thêm vào.
- Trả lời PSIA `ResponseStatus` có `statusCode` khác 1 (OK) hoặc 7 (Reboot Required) thì coi là lỗi.
  `lastError() = "HTTP <code> <statusString> (<subStatusCode>)"`.
- Log không bao giờ in user/pass hay header `Authorization` (theo quy tắc repo).

**Phân loại lỗi connect.**
- Timeout/RST → `UNREACHABLE`
- 401 sau khi đã thử Digest, hoặc 403 → `AUTH_FAILED`
- 404/405, body không phải XML hoặc không có `DeviceInfo` → `NOT_THIS_PROTOCOL`
- còn lại → `OTHER`

**Ánh xạ.**

| Chức năng | PSIA | Điền vào |
|---|---|---|
| connect | `GET /PSIA/System/deviceInfo` → `GET /PSIA/Streaming/channels` → capabilities từng kênh → PTZ/IO (bỏ qua nếu 404) | toàn bộ cache |
| probeConnection | `GET /PSIA/System/deviceInfo`, so `serialNumber`/`macAddress` với lần connect | như `OnvifControl` |
| Thông tin thiết bị | `DeviceInfo`: `manufacturer` (thiếu thì lấy `deviceName`/`"PSIA"`), `model`, `firmwareVersion`, `serialNumber`, `macAddress`, `deviceID` → `hardwareId` | `OnvifDeviceInfo` |
| Profile | mỗi `StreamingChannel` có video là một profile, `token = id` | `OnvifMediaProfile` |
| Encoder | `Video/videoCodecType` (H.264/H.265/MJPEG → `H264`/`H265`/`JPEG`), `videoResolutionWidth/Height`, `constantBitRate`/`vbrUpperCap` (kbps), `maxFrameRate` (/100) | `vcodec,width,height,bitrate,fps` |
| URL luồng | `rtsp://<ip>:<rtspPortNo>/PSIA/Streaming/channels/<id>`. `rtspPortNo` lấy từ `Transport/rtspPortNo`, mặc định 554. Nếu `Transport/ControlProtocolList` có URL tuyệt đối thì dùng URL đó | `url` |
| Tuỳ chọn encoder | `GET /PSIA/Streaming/channels/<id>/capabilities`, đọc thuộc tính `opt=`/`min=`/`max=` | `vEncoderOptionMap`, `videoEncEditable` |
| Ghi encoder | `GET /PSIA/Streaming/channels/<id>`, sửa node, `PUT` lại | `setVideoEncoderConfigByToken` |
| Đọc lại profile | `GET /PSIA/Streaming/channels` | `refreshMediaProfiles` |
| PTZ capabilities | `GET /PSIA/PTZ/channels/<ch>/capabilities`. Kênh PTZ là `id/100` của stream (101 → 1); không có thì lấy kênh PTZ đầu tiên của `GET /PSIA/PTZ/channels` | cờ `isAbs/Rel/ConsMoveEnable`, biên |
| Continuous | `PUT /PSIA/PTZ/channels/<ch>/continuous` với `pan/tilt/zoom` ∈ [-100,100] (quy đổi ×100 từ [-1,1]). Stop = continuous 0,0,0 | `PTZ_ContinuousMove`, `PTZ_Stop` |
| Relative/Absolute | `PUT .../relative` (`positionX/Y`, `relativeZoom`), `PUT .../absolute` (`elevation`, `azimuth`, `absoluteZoom`). Quy đổi biên đọc từ capabilities về [-1,1] | `PTZ_RelativeMove`, `PTZ_AbsoluteMove` |
| Trạng thái | `GET .../status` (`AbsoluteHigh` + `moveStatus` nếu có; thiếu thì `UNKNOWN`) | `PTZ_GetStatus` |
| Preset | `GET/PUT/DELETE .../presets[/<pid>]`, `PUT .../presets/<pid>/goto` | `presetMap`, `isPresetEnable`, `PTZ_GotoPreset` |
| Home | `PUT .../homeposition/goto` | `isHomePresetEnable`, `PTZ_GotoHomePosition` |
| Focus/Iris | `PUT /PSIA/PTZ/channels/<ch>/focus`, `/iris` (`focus`/`iris` ∈ [-100,100], 0 = dừng) | `OnvifImageProfile`, `Imaging_Focus*`, Iris |
| Relay | `GET /PSIA/IO/outputs`, `PUT /PSIA/IO/outputs/<id>/trigger` (`outputState` high/low) | `OnvifRelayOutputProfile`, `Relay_SetOutputState` |
| Auxiliary | không có lệnh chuẩn → trả false, `lastError` "not supported" | `PTZ_AuxiliaryCommand` |
| Giờ | `GET /PSIA/System/time`, sửa `timeMode=manual`, `localTime`, `timeZone`; `PUT` lại | `setCameraTimeManual` |

Phần nào trả 404 hoặc thiếu capabilities thì cờ tương ứng để false (không hỗ trợ). Không coi là lỗi connect.

### 4.5 Config (`onceToken` + `GET_CONFIG`, đăng ký trong `src/Common/config.cpp` cạnh `OnvifCtrl`)

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `camera.control_auto_order` | `onvif,psia` | Thứ tự thử khi `AUTO` |
| `camera.control_reprobe_failures` | `5` | Số lần connect hỏng `NOT_THIS_PROTOCOL` liên tiếp trước khi dò lại |
| `psia.request_timeout_ms` | `5000` | Timeout mỗi request PSIA |

## 5. Các bước

Mỗi bước một commit (tiếng Việt). Build `cmake --build build -j2`. Test trong `build-test` (`-R unit.`). Thêm file
`.cpp` vào `manager/` thì phải reconfigure, vì `manager` dùng `file(GLOB)`.

| Bước | Nội dung | File chính | Kiểm tra |
|---|---|---|---|
| P0 | Spike: chương trình nhỏ trong scratchpad gọi `GET /PSIA/System/deviceInfo` bằng `soap_GET` + `httpda` vào `psia_mock.py` (Digest) và, nếu có, một camera thật. Xác nhận cách đọc body/status của gSOAP raw HTTP. | scratchpad | Ghi kết quả vào mục 9; vướng thì đổi sang client socket + Digest tự viết (MD5 của toolkit), sửa mục 4.4 |
| P1 | `CameraDeviceControl.h`, enum + string helper. `OnvifControl` implement (gồm `lastConnectError`, `PTZ_GetStatus` mới). `CameraController`: `_onvif_ctr` → `_device_ctr`, `getSoapErrMsg` → `lastError`, `tt__MoveStatus` → `PtzMoveStatus`, `createOnvifControl` → `createDeviceControl(ControlProtocol, addr, user, pass)`, `MediaGapFillManager`/audio nhận qua `dynamic_pointer_cast`. Fake trong test đổi base. **Không đổi hành vi.** | `manager/Control/*`, `manager/Camera/CameraController.*`, `tests/unit/manager/test_camera_controller.cpp` | Build + toàn bộ `unit.` xanh |
| P2 | `controlProtocol` ở `DeviceOption`/`ControllerOption`/`CameraOption`, `fromJson`/`makeCameraOptionJson`, kv `ResourceAdapter`. `DeviceCapabilities::controlProtocol` + JSON. Config `camera.*`. Logic chọn/khoá/dò lại/chuyển giao thức (4.2), `setLastControlProtocol`, `CODE_DEVICE_PROTOCOL_CHANGED`. | `manager/Common/DeviceOption.h`, `GenericRtspCamera.h`, `GenericRtspCameraImp.cpp`, `CameraController.*`, `CameraStatistic.cpp`, `server/Manager.cpp`, `server/WebApiErrCode.h`, `src/Common/config.*` | Unit test ở mục 5.1 |
| P3 | `PsiaControl`: HTTP + Digest, connect/probe, deviceInfo, channels, URL, capabilities encoder, get/set encoder, refresh, giờ, phân loại lỗi | `manager/Control/PsiaDeviceControl.*` | Unit test parse XML mẫu (mục 5.1). `psia_mock.py` + test tích hợp nhỏ |
| P4 | `PsiaControl`: PTZ (cont/rel/abs/stop/status), preset, home, focus/iris, relay | như P3 | Unit test quy đổi biên + XML PTZ. Gọi API PTZ trên mock |
| P5 | `Manager.cpp`/`CameraStatistic.cpp` đổi điều kiện `isOnvifDevice` (4.3), thêm `controlProtocol`/`controlProtocolSetting`, docs API, `conf/config.ini` | `server/Manager.cpp`, `manager/Camera/CameraStatistic.cpp`, `docs/` | Gọi API trên mock: camera PSIA có profiles, PTZ, video config rule chạy |
| P6 | E2E: camera ONVIF thật (hồi quy); camera bật PSIA thật ở chế độ `PSIA`; camera tắt ONVIF chỉ còn PSIA ở `AUTO`; restart giữ khoá; đổi `ONVIF` ↔ `PSIA` qua API. | `tests/integration/stress_vms/psia_mock.py` | Ghi kết quả mục 9 |

### 5.1 Unit test

`tests/unit/manager/test_camera_controller.cpp`, dùng 2 fake controller (ONVIF, PSIA) đếm số lần gọi và số bản thể
còn sống:
- Chế độ `ONVIF` cố định: connect lỗi `NOT_THIS_PROTOCOL` mãi cũng **không** tạo fake PSIA.
- Chế độ `PSIA` cố định: tương tự theo chiều ngược lại.
- `AUTO`: ONVIF trả `NOT_THIS_PROTOCOL` → tạo PSIA, khoá `PSIA`, fake ONVIF đã huỷ (`weak_ptr` hết hạn) trước khi
  PSIA connect.
- `AUTO`: ONVIF trả `UNREACHABLE` hoặc `AUTH_FAILED` → **không** thử PSIA.
- Đã khoá: hỏng `NOT_THIS_PROTOCOL` 4 lần không dò lại, lần 5 thì dò. Có lỗi khác xen giữa thì reset bộ đếm.
- `setLastControlProtocol(PSIA)` + `AUTO` → kết nối PSIA ngay, không tạo ONVIF.
- Đổi option `ONVIF` → `PSIA` → `onControllerReady(false)` trước, controller cũ reset, `_ruleConfigMap` báo
  `CODE_DEVICE_PROTOCOL_CHANGED`.
- Lệnh PTZ lúc đang dò → `CODE_DEVICE_OFFLINE`.

`tests/unit/manager/test_psia_control.cpp` (mới, `ENABLE_MANAGER`). Test chỉ phần parse/quy đổi, gọi qua hàm
`static` trong namespace `psia_detail` khai báo ở `PsiaDeviceControl.h`, không tách file riêng:
- Parse `DeviceInfo`, `StreamingChannelList` (Hikvision-style + chuẩn PSIA, có/không namespace), sinh URL (port
  mặc định, port tuỳ chỉnh).
- Parse `ResponseStatus` lỗi → `lastError`.
- Quy đổi tốc độ/toạ độ PTZ và biên từ capabilities.
- Sửa encoder trên XML gốc giữ nguyên node lạ.

Đăng ký test theo `tests/unit/CMakeLists.txt`. Có thêm `add_test unit.<x>_all` nếu target link ext-codec (bẫy
discovery rỗng).

## 6. Rủi ro

- **R1: PSIA lệch chuẩn theo hãng.** Hikvision firmware mới có thể bỏ `/PSIA` và chỉ còn `/ISAPI`.
  - Đọc mềm từng phần, chỉ bật cờ khi đọc được capabilities.
  - Camera như vậy ở `AUTO` vẫn chạy ONVIF bình thường. ISAPI để đợt sau (mục 1).
- **R2: token profile khác giữa hai giao thức.** Xử lý ở cuối mục 4.2: bỏ config/rule/preset cũ, báo lỗi rule.
  Không áp cấu hình cũ sang giao thức mới.
- **R3: URL luồng phải khớp nguyên chuỗi.**
  - `CameraStatistic` tìm profile theo `full_url`. Nếu luồng cấu hình bằng URL ONVIF (ví dụ
    `/Streaming/Channels/101`) mà PSIA sinh `/PSIA/Streaming/channels/101`, sẽ không khớp. Hậu quả: video config
    rule/encoder map không tìm được profile.
  - P6 phải kiểm trên camera thật.
  - Nếu lệch thì thêm bước khớp mềm trong `PsiaControl`: thử URL do camera khai báo trước, rồi đến URL sinh ra.
  - Không sửa cách so URL trong `CameraStatistic`.
- **R4: dò `AUTO` tốn thời gian trên pool `device()`.**
  - Camera không ONVIF mất một lượt connect ONVIF trước khi thử PSIA.
  - Chỉ xảy ra lần đầu hoặc khi dò lại; khoá được lưu qua restart.
  - Lỗi `NOT_THIS_PROTOCOL` thường về nhanh (404/không phải SOAP), không phải chờ hết timeout.
- **R5: hai controller cùng tồn tại khi dò.** Tránh bằng 4.2 mục 2 (huỷ controller thua trong cùng task); có unit
  test kiểm.
- **R6: phân loại lỗi ONVIF sai** có thể làm camera ONVIF chậm trả lời bị coi là "không ONVIF".
  - Chỉ HTTP 404/405 hoặc body không phải SOAP mới được tính `NOT_THIS_PROTOCOL`.
  - Timeout luôn là `UNREACHABLE`.
  - Cần 5 lần liên tiếp mới dò lại.

## 7. Không thay đổi

- Không migration: option là kv `VmsResource`, capabilities là JSON.
- Không đụng `api/` (C ABI), `SubnetScan`, `SpeakerController`, logic `MediaGapFillManager`.
- Không đổi tên struct `Onvif*` hay field JSON hiện có.

## 8. Câu hỏi đã chốt (2026-10-03)

- **Q1.** "PSI" là **PSIA**. Làm theo chuẩn `/PSIA`; ISAPI để đợt sau.
- **Q2.** Field cấu hình: `cameraAdvanceConfig.control.protocol` = `AUTO|ONVIF|PSIA`, mặc định `AUTO`.
- **Q3.** `AUTO` thử **ONVIF trước**, rồi PSIA (`camera.control_auto_order`).
- **Q4.** Chưa dò PSIA trong quét subnet; để đợt sau.

## 9. Kết quả

**P0 (2026-10-03) — đạt, giữ gSOAP.** Spike `soap_GET`/`soap_PUT` + plugin `http_da` gọi vào
`tests/integration/stress_vms/psia_mock.py` (Digest qop=auth):
- Lần đầu trả `soap->error = 401`, `soap->authrealm` có realm; `http_da_save` rồi gửi lại → 200. Các request sau
  `http_da_restore` trước khi gửi.
- Đọc body: `soap_begin_recv` → `soap_http_get_body` → `soap_end_recv`; status ở `soap->status`.
- PUT: `soap_PUT(..., "application/xml")` → `soap_send` → `soap_end_send` (gSOAP tự chuyển `SOAP_IO_STORE` để có
  Content-Length); phải trả `soap->http_content = NULL` sau đó.
- HTTP 404 → `soap->error = 404`, không đọc được body lỗi. Port đóng hoặc IP không trả lời → `SOAP_TCP_ERROR` (28),
  `soap->status` không phải mã HTTP.

**P1 (2026-10-03) — xong.** `manager/Control/CameraDeviceControl.{h,cpp}`: interface, enum `ControlProtocol`,
`PtzMoveStatus`, `ConnectError`; các struct `Onvif*`/`VideoEncoderConfig*`/`SDCardInformation` chuyển nguyên từ
`OnvifDeviceControl.h` sang (không đổi tên) để `PsiaControl` không phải kéo header gSOAP ONVIF.
- `OnvifControl` implement interface. `PTZ_GetStatus` trả `PtzMoveStatus`, bản cũ đổi tên `PTZ_GetMoveStatus`.
  `lastError()` = `getSoapErrMsg()`.
- `lastConnectError()` theo `OnvifControl::classifyConnectError(soap->error, fault)`, chỉ xét lỗi ở
  `GetDeviceInformation`; lỗi ở bước sau (capabilities, profiles) là `OTHER` vì camera đã trả lời ONVIF.
- `CameraController`: `_device_ctr`, `createDeviceControl(ControlProtocol, ...)`, `MediaGapFillManager` nhận
  `dynamic_pointer_cast<OnvifControl>`, `caps->isOnvifDevice` theo `protocol()`. Chưa đổi hành vi (luôn ONVIF).
- Test: `OnvifConnectErrorTest`; fake trong `test_camera_controller.cpp` đổi hook. `ctest -R unit.` 201/201.
