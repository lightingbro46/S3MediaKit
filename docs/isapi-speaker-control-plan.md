# Plan: điều khiển camera bằng ISAPI và cho SpeakerController dùng chung giao thức điều khiển

Ngày lập: 2026-10-04. Nhánh đề xuất: tiếp trên `feature/psi-controller` (cần `CameraDeviceControl`, `PsiaControl` và
logic chọn/khoá giao thức của plan PSIA, chưa merge `develop`).
Trạng thái (2026-10-04): I0–I3, S1–S5 xong theo đề xuất mặc định của mục 8 (user không trả lời Q1–Q6); S4 mới
kiểm trên mock. E (thiết bị thật) chưa làm. Kết quả ở mục 9.

Hai phần:
- **Phần I — ISAPI cho camera.** ISAPI là REST/XML của Hikvision (`/ISAPI/...`), phát triển từ PSIA. Thêm giao thức
  thứ ba vào cơ chế một camera một giao thức đã có.
- **Phần S — SpeakerController.** Hiện cứng ONVIF (`OnvifControl::Ptr _onvif_ctr`) cộng plugin hãng (Bosch). Đổi sang
  controller trung lập `CameraDeviceControl` để speaker chạy được ONVIF/PSIA/ISAPI. Dùng chung logic dò/khoá giao thức
  với camera thay vì chép lại.

## 1. Mục tiêu

1. `ControlProtocol` thêm `ISAPI`. Option camera `cameraAdvanceConfig.control.protocol` nhận `AUTO|ONVIF|PSIA|ISAPI`.
2. Camera ISAPI có đủ các chức năng camera PSIA đang có: thông tin thiết bị, profile + URL luồng, đọc/ghi encoder,
   video config rule, PTZ (continuous/relative/absolute/stop/status), preset, home, focus/iris, relay, đồng bộ giờ.
3. `AUTO` dò được camera Hikvision tắt ONVIF (trường hợp mặc định của nhiều model Hikvision).
4. Speaker có option `speakerAdvanceConfig.control.protocol` = `AUTO|ONVIF|PSIA|ISAPI`, cùng luật chọn/khoá/dò lại
   như camera. Speaker hiện có chạy y như cũ (thiếu field là `AUTO`, `AUTO` thử ONVIF trước, dữ liệu cũ
   `isOnvifDevice=true` đọc thành ONVIF và khoá ngay).
5. Logic dò/khoá giao thức chỉ có **một** bản, dùng chung cho camera và speaker.

Ngoài phạm vi:
- Phát lại thẻ nhớ qua ISAPI (`MediaGapFillManager` vẫn chỉ ONVIF Profile G).
- Sự kiện ISAPI (`/ISAPI/Event/notification/alertStream`), motion phía camera.
- Dò ISAPI trong `SubnetScan`.
- API file âm thanh riêng của loa Hikvision (tương đương plugin Bosch). Chỉ làm khi có thiết bị và tài liệu (Q5).

## 2. Hiện trạng

### 2.1 Camera (sau plan PSIA)

| Thành phần | Vị trí | Ghi chú |
|---|---|---|
| Interface | `manager/Control/CameraDeviceControl.h` | `ControlProtocol{NONE,ONVIF,PSIA}`, `ConnectError`, helper string |
| PSIA | `manager/Control/PsiaDeviceControl.{h,cpp}` | gSOAP `soap_GET/soap_PUT` + `http_da`; parse trong `psia_detail`; **đường dẫn `/PSIA/...` viết thẳng trong từng hàm** (khoảng 15 chỗ) |
| Tạo controller | `CameraController::createDeviceControl` (virtual, fake trong test) | switch ONVIF/PSIA |
| Dò `AUTO` | `CameraController::startProtocolProbe` / `onProbeDone` / `onConnectResult` / `onProtocolConnected` / `unlockProtocol`; `autoProtocolOrder()` static trong `CameraController.cpp` | Logic khoá nằm lẫn trong state của `CameraController` |
| Config | `src/Common/config.{h,cpp}` `CameraControl::kAutoOrder` (`onvif,psia`), `kReprobeFailures`; `PsiaCtrl::kRequestTimeoutMs` | |

### 2.2 Speaker

| Thành phần | Vị trí | Vấn đề |
|---|---|---|
| Controller | `manager/Speaker/SpeakerController.{h,cpp}` | `_onvif_ctr` cứng `OnvifControl`; `onManager` gọi `connect()` đầy đủ mỗi 60 s kể cả khi đang online; `caps->isOnvifDevice = true` cố định |
| Backchannel | `registerAudioBackchannel` → `OnvifControl::setupAudioBackchannel` → `BackchannelManager::registerDevice(id, rtspUrl)` | Chỉ có RTSP backchannel ONVIF (`BackchannelPusher`) |
| Plugin hãng | `ext-plugin/IDevice.h`, `IAudioPlayback.h`, `DeviceFactory`, `Bosch/BoschIpSpeaker` | Chọn theo `getDeviceBrand(manufacturer)`. Bosch cần tạo tài khoản ONVIF riêng (`speaker.onvifUsername`) |
| Thao tác hãng | 6 hàm `playAudioFileByVendorApi/uploadAudio/stopAudio/deleteAudio/listAudio/setAudioVolume` | Lặp cùng một khuôn connect → `dynamic_cast<IAudioPlayback*>` → gọi → disconnect |
| Lưu trữ | `SpeakerStatistic.cpp` (`isOnvifDevice`), `server/Manager.cpp` `fromJson(SpeakerOption&)`, `makeSpeakerOptionJson`, nhánh speaker của `makeDeviceCapabilitiesJson` | Chưa có `controlProtocol` |
| Test | Chưa có unit test cho speaker | |

`SpeakerController` chỉ dùng của ONVIF: `connect`, `getSoapErrMsg`, `getDeviceInfo`, `getAudioOutputProfile`,
`getAudioInputProfile`, `supportAudioBackchannel`, `setupAudioBackchannel`. Ba hàm đầu và hai getter audio đã có trong
`CameraDeviceControl`. Còn thiếu đúng phần backchannel.

## 3. Phần I — ISAPI

### 3.1 Cách làm: `IsapiControl` dùng lại `PsiaControl`

ISAPI và PSIA giống nhau ở: HTTP Digest, XML (tên node gần như trùng, chỉ khác namespace
`http://www.hikvision.com/ver20/XMLSchema` thay `urn:psialliance-org`), `DeviceInfo`, `StreamingChannel`,
`PTZData`, `ResponseStatus`. Parse trong `psia_detail` đã đọc theo local name nên đọc được cả hai.

Hai bước:
1. **Gom đường dẫn của `PsiaControl` vào một bảng** (`struct HttpXmlPaths`), truyền qua constructor. `PsiaControl`
   giữ nguyên hành vi với bảng `/PSIA`. Đây là việc plan PSIA đã hứa ở mục 1 nhưng chưa làm.
2. **`class IsapiControl : public PsiaControl`**: truyền bảng `/ISAPI`, `protocol()` trả `ISAPI`, override vài điểm khác
   biệt (bảng 3.2). Không chép HTTP/Digest/parse.

| Khoá trong bảng | PSIA | ISAPI |
|---|---|---|
| deviceInfo | `/PSIA/System/deviceInfo` | `/ISAPI/System/deviceInfo` |
| channels | `/PSIA/Streaming/channels` | `/ISAPI/Streaming/channels` |
| channel caps | `.../channels/<id>/capabilities` | như PSIA |
| ptz root | `/PSIA/PTZ/channels` | `/ISAPI/PTZCtrl/channels` |
| focus / iris | `/PSIA/System/Video/inputs/channels/<ch>/focus\|iris` | `/ISAPI/System/Video/inputs/channels/<ch>/focus\|iris` |
| outputs | `/PSIA/IO/outputs`, `/<id>/trigger` | `/ISAPI/System/IO/outputs`, `/<id>/trigger` |
| time | `/PSIA/System/time` | `/ISAPI/System/time` |

### 3.2 Khác biệt cần override

| Điểm | PSIA (hiện tại) | ISAPI |
|---|---|---|
| Cổng RTSP | `rtspPortNo` trong `StreamingChannel` | Thường không có trong channel; đọc `GET /ISAPI/Security/adminAccesses` (`AdminAccessProtocol` `protocol=RTSP` → `portNo`), lỗi thì 554 |
| URL profile | `rtsp://ip:port/PSIA/Streaming/channels/<id>` | `rtsp://ip:port/Streaming/Channels/<id>`: dạng người dùng Hikvision hay cấu hình, nên tránh được R3 (khớp nguyên chuỗi URL) mà không phải sửa `CameraStatistic` (Q4) |
| PTZ relative | `.../relative` | Kiểm trên camera thật; không có thì `PTZ_RelativeMove` trả false và cờ relative tắt, như phần PSIA đang làm khi thiếu capabilities |
| Kiểm DeviceInfo | root `DeviceInfo` | như PSIA. 200 mà không phải `DeviceInfo` = `NOT_THIS_PROTOCOL` |
| Codec | `H.264`/`H.265`/`MJPEG` | Thêm `H.265+`/`H.264+`/`Smart264`; `codecFromPsia` đã có `H.265+`, bổ sung phần còn lại |

`classifyConnectError` dùng chung: 404/405 hoặc không phải DeviceInfo = `NOT_THIS_PROTOCOL`, không có trả lời =
`UNREACHABLE`, 401/403 = `AUTH_FAILED`.

### 3.3 Chọn giao thức

- `ControlProtocol::ISAPI`, chuỗi `"ISAPI"`. `controlProtocolSetting("ISAPI")` thành `"ISAPI"` (test hiện đang mong
  `"AUTO"`, sửa lại).
- `camera.control_auto_order` mặc định đổi thành `onvif,isapi,psia` (Q1). Camera đã khoá không bị ảnh hưởng.
  Hikvision firmware cũ trả lời cả `/ISAPI` lẫn `/PSIA`; ISAPI đầy đủ hơn nên thử trước.
- **Vấn đề Hikvision tắt ONVIF (R1).** Tài khoản ONVIF của Hikvision tách khỏi tài khoản web. Khi ONVIF tắt hoặc chưa
  tạo user ONVIF, camera có thể trả 401 thay vì 404. Hiện ONVIF `AUTH_FAILED` làm `AUTO` dừng, không bao giờ thử ISAPI.
  Đề xuất (Q2): **chỉ trong lượt dò**, `AUTH_FAILED` của ONVIF vẫn cho thử giao thức kế tiếp. Nếu giao thức sau cũng
  hỏng, lỗi báo ra là lỗi ONVIF (lỗi đầu tiên có ý nghĩa). `AUTH_FAILED` của ISAPI/PSIA vẫn dừng như cũ. Camera đã khoá
  không đổi luật.
- Config mới `isapi.request_timeout_ms` (5000), đăng ký cạnh `psia.request_timeout_ms`.

### 3.4 API / client

- Webhook capabilities: `controlProtocol = "ISAPI"`, `controlProtocolSetting` nhận thêm `"ISAPI"`.
- Các cờ chức năng (`enableOnvifProfileSelect`, `enableOnvifProfileConfig`, `isPTZ`, `ptzControlMode`) đã trung lập.
  Client chỉ thêm lựa chọn `ISAPI` vào ô chọn giao thức.
- `onvifDevice` vẫn chỉ true với ONVIF.

## 4. Phần S — SpeakerController

### 4.1 Dùng chung phần chọn giao thức (tách khỏi CameraController)

Đưa vào `manager/Control/CameraDeviceControl.{h,cpp}`. Không thêm file, để khỏi phải reconfigure glob:

```cpp
// Factory: ONVIF → OnvifControl, PSIA → PsiaControl, ISAPI → IsapiControl, NONE → nullptr.
CameraDeviceControl::Ptr makeDeviceControl(ControlProtocol protocol, const std::string &address,
                                           const std::string &username, const std::string &password);

// camera.control_auto_order / speaker.control_auto_order → danh sách; rỗng hoặc toàn tên lạ → mặc định.
std::vector<ControlProtocol> parseProtocolOrder(const std::string &order_str);

// Blocking, chạy trên device queue. Thử lần lượt, huỷ controller thua trước khi tạo cái kế tiếp.
struct ProtocolProbeResult { CameraDeviceControl::Ptr winner; std::string err_msg; };
typedef std::function<CameraDeviceControl::Ptr(ControlProtocol)> DeviceControlFactory;
ProtocolProbeResult probeControlProtocols(const std::vector<ControlProtocol> &order,
                                          const DeviceControlFactory &factory, const std::string &log_name);

// Trạng thái khoá, thuần dữ liệu, chỉ dùng trên poller của chủ. Không có thread, không có timer.
class ProtocolLock {
public:
    void configure(const std::string &setting, ControlProtocol last); // setting: AUTO|ONVIF|PSIA|ISAPI
    bool isAuto() const;
    ControlProtocol fixedOrLocked() const;   // giao thức tạo thẳng controller, NONE = phải dò
    // Kết nối thành công. Trả true khi giao thức đổi so với lần trước (chủ xoá state của giao thức cũ).
    bool onConnected(ControlProtocol protocol);
    // Kết nối lỗi. Trả true khi phải mở khoá và dò lại (AUTO + đủ N lần NOT_THIS_PROTOCOL liên tiếp).
    bool onConnectError(ConnectError err, int reprobe_failures);
    void unlock();
};
```

- `CameraController` chuyển sang dùng mấy hàm này. **Hành vi không đổi.** `createDeviceControl` vẫn là hook virtual
  cho test và gọi `makeDeviceControl` bên trong.
- `CameraProtocolTest` (8 case) phải xanh nguyên như cũ. Thêm `ProtocolLockTest` thuần (không poller), phủ các luật
  ở mục 5.1 plan PSIA.

### 4.2 SpeakerController mới

- `OnvifControl::Ptr _onvif_ctr` → `CameraDeviceControl::Ptr _device_ctr`. Đổi tên `IDevice::Ptr _device_ctr` hiện có
  thành `_vendor_ctr` cho khỏi trùng.
- Thêm `ProtocolLock _lock`, `_probing`, `_ctrl_generation`, giống camera. Dò bằng `probeControlProtocols` trên
  `_device_queue`.
- **Loa Bosch** (`requiresSeparateDeviceCredential`) luôn ONVIF với `speaker.onvifUsername`, vì plugin tạo tài khoản
  ONVIF. Option khác `ONVIF` thì ghi `WarnL` và bỏ qua.
- **Giữ kết nối nhẹ hơn:** đang online thì `probeConnection()` thay cho `connect()` đầy đủ mỗi 60 s, như
  `CameraController::onManager`. Chỉ `connect()` + đăng ký backchannel lại khi trước đó offline.
- Caps: `controlProtocol = controlProtocolToString(...)`, `isOnvifDevice = (protocol == ONVIF)`.
  `supportsAudioBackchannel` lấy từ controller (4.3).
- Giao thức đổi (R2 của speaker): `BackchannelManager::unregisterDevice` rồi đăng ký lại theo giao thức mới.
  `remoteId` file của plugin hãng giữ nguyên (không phụ thuộc giao thức).
- **Gom 6 thao tác hãng** vào một helper:

  ```cpp
  typedef std::function<void(IAudioPlayback *, const OnDeviceResult &)> VendorOp;
  void runVendorOp(const VendorOp &op, OnDeviceResult cb, bool autoDisconnect);
  ```

  Helper làm phần chung: `vendorDevice` → `connect` → bọc `doDisconnect`. `playAudioFileByVendorApi` vẫn giữ nhánh
  upload rồi play riêng bên trong `op`. Bớt khoảng 120 dòng lặp. Chữ ký public không đổi.

### 4.3 Backchannel theo giao thức

Thêm vào `CameraDeviceControl` (mặc định không hỗ trợ, nên camera không phải sửa):

```cpp
struct AudioBackchannelInfo {
    enum Transport { NONE, RTSP_ONVIF, HTTP_ISAPI } transport = NONE;
    std::string url;        // RTSP URI (ONVIF) hoặc http://ip:port/ISAPI/System/TwoWayAudio/channels/<id>
    std::string codec;      // G711U/G711A/G726/AAC
    int sampleRate = 8000;
};
virtual bool supportsAudioBackchannel() const { return false; }
virtual bool setupAudioBackchannel(AudioBackchannelInfo &out, bool sharedDspPipeline) { return false; }
```

- `OnvifControl`: bọc hàm `setupAudioBackchannel(std::string&, bool)` sẵn có → `RTSP_ONVIF`. Không đổi logic SOAP.
- `IsapiControl`: `GET /ISAPI/System/TwoWayAudio/channels` (có kênh, `audioCompressionType`) → `HTTP_ISAPI`.
- `PsiaControl`: không hỗ trợ (PSIA chuẩn không có đường two-way audio cố định). Loa PSIA chỉ có thông tin thiết bị
  và trạng thái online, trừ khi có plugin hãng (Q3).
- `BackchannelManager::registerDevice(id, url)` giữ nguyên cho RTSP. Thêm `registerDevice(id, AudioBackchannelInfo)`
  chọn pusher theo `transport`.
- **Pusher ISAPI mới** `src/Backchannel/IsapiTwoWayAudioPusher.{h,cpp}`: async trên poller, như
  `BackchannelPusher`, **không** dùng gSOAP blocking vì phát kéo dài cả phiên.
  - Trình tự: `PUT .../open` → `PUT .../audioData`, gửi G.711 liên tục trên một kết nối TCP, Digest tự tính như
    `BackchannelPusher` đang làm → `PUT .../close`.
  - Cùng các callback `onReady/onFailed/onShutdown/onSourceFinished` để `BackchannelManager` đối xử như pusher RTSP.
    Tách interface nhỏ `IBackchannelPusher` nếu hai lớp cần chung một con trỏ trong `_activePushers`.
  - Camera trả "device busy" khi đang có phiên two-way khác → `StartResult::DeviceBusy`.
- Chế độ phát `BACKCHANNEL` giữ tên, nghĩa mở rộng thành "backchannel của giao thức điều khiển" (RTSP ONVIF hoặc ISAPI
  two-way). Client không phải sửa.

### 4.4 Option / lưu trữ / API

- `SpeakerOption::controlProtocol` (`DeviceOption` đã có field từ plan PSIA). `operator==` thêm field.
  `fromJson(SpeakerOption&)` đọc `speakerAdvanceConfig.control.protocol`; `makeSpeakerOptionJson` ghi
  `controlProtocol`.
- `SpeakerStatistic.cpp`:
  - option và caps thêm `controlProtocol`;
  - dữ liệu cũ `isOnvifDevice=true` đọc thành `"ONVIF"`;
  - lúc khởi động `GenericIPSpeakerImp::setupController` truyền giao thức đã khoá vào controller.
- `makeDeviceCapabilitiesJson` nhánh speaker:
  - thêm `controlProtocol` và `controlProtocolSetting`;
  - `onvifDevice` giữ nghĩa;
  - `supportedAudioPlaybackModes` vẫn theo `supportsAudioBackchannel`.
- Config: `speaker.control_auto_order` (mặc định `onvif,isapi,psia`) và `speaker.control_reprobe_failures` (5). Tách
  khỏi config camera để chỉnh riêng được; đọc qua cùng `parseProtocolOrder`.
- Không migration (kv + JSON như camera).

## 5. Các bước

Mỗi bước một commit tiếng Việt. Build `cmake --build build -j2`, test `build-test` `-R unit.`. Bước nào thêm file
`.cpp` vào `manager/` hoặc `src/` thì reconfigure.

| Bước | Nội dung | File chính | Kiểm tra |
|---|---|---|---|
| I0 | `psia_mock.py --dialect isapi`: phục vụ `/ISAPI/...`, namespace Hikvision, `adminAccesses`, `TwoWayAudio` (ghi byte nhận được ra file). Spike gọi vài đường trên camera Hikvision thật nếu có, ghi lại 401/404 của ONVIF khi tắt ONVIF (R1). | `tests/integration/stress_vms/psia_mock.py` | Ghi mục 9 |
| I1 | Gom đường dẫn `PsiaControl` vào bảng `HttpXmlPaths`. **Không đổi hành vi.** | `PsiaDeviceControl.*` | `unit.psia_control_all` + mock PSIA xanh |
| I2 | `ControlProtocol::ISAPI` + string/setting; `IsapiControl` (cổng RTSP, URL, codec); `createDeviceControl`/`makeDeviceControl` nhánh ISAPI; config `isapi.request_timeout_ms`, `control_auto_order` mới; luật `AUTH_FAILED` ONVIF lúc dò (theo Q2). | `CameraDeviceControl.*`, `PsiaDeviceControl.*`, `CameraController.cpp`, `src/Common/config.*` | Unit parse ISAPI (XML Hikvision mẫu), `CameraProtocolTest` thêm case ONVIF 401 → ISAPI |
| I3 | Docs `camera-control-protocol.md`, `conf/config.ini`; chạy thật MediaServer + mock ISAPI: `AUTO` khoá ISAPI, PTZ/preset/focus/relay/encoder/rule, restart giữ khoá; hồi quy mock ONVIF và PSIA. | `docs/`, `conf/` | Ghi mục 9 |
| S1 | Tách `makeDeviceControl`, `parseProtocolOrder`, `probeControlProtocols`, `ProtocolLock` ra `CameraDeviceControl.*`; `CameraController` dùng lại. **Không đổi hành vi.** | `CameraDeviceControl.*`, `CameraController.*` | `ProtocolLockTest` mới + `CameraProtocolTest` nguyên vẹn |
| S2 | `SpeakerController` dùng `CameraDeviceControl` + `ProtocolLock`; Bosch ép ONVIF; `probeConnection` khi online; option/caps/kv/JSON `controlProtocol`; config `speaker.*`. Backchannel vẫn chỉ ONVIF qua `dynamic_pointer_cast`. | `manager/Speaker/*`, `DeviceOption.h`, `server/Manager.cpp`, `src/Common/config.*` | Unit test speaker mới (fake controller như camera): AUTO/khoá/Bosch ép ONVIF; E2E mock ONVIF speaker phát backchannel như cũ |
| S3 | `runVendorOp` gom 6 thao tác hãng. | `SpeakerController.*` | Build + E2E plugin Bosch nếu có thiết bị, không thì review kỹ diff |
| S4 | `AudioBackchannelInfo` trong interface; `OnvifControl`/`IsapiControl` cài đặt; `IsapiTwoWayAudioPusher`; `BackchannelManager` chọn pusher. | `CameraDeviceControl.h`, `OnvifDeviceControl.*`, `PsiaDeviceControl.*`, `src/Backchannel/*` | Mock ISAPI nhận đủ byte G.711 của file phát, busy → `DeviceBusy`; hồi quy RTSP backchannel ONVIF |
| S5 | Docs speaker (`docs/camera-control-protocol.md` thêm mục speaker hoặc file riêng), webhook. | `docs/` | |
| E | Thiết bị thật: camera Hikvision tắt ONVIF ở `AUTO`; Hikvision chế độ `ISAPI`; camera ONVIF hồi quy; loa Bosch hồi quy; loa/camera Hikvision two-way audio. | | Ghi mục 9 |

Phần I và S1 độc lập nhau. S2 cần S1. S4 cần I2.

## 6. Rủi ro

- **R1: Hikvision tắt ONVIF trả 401 chứ không phải 404.** Không xử lý thì `AUTO` không bao giờ tới ISAPI. Xem Q2.
  I0 phải đo trên thiết bị thật.
- **R2: khoá tài khoản do sai mật khẩu.** Hikvision mặc định khoá IP sau vài lần đăng nhập sai liên tiếp. Mật khẩu sai
  ở `AUTO` thì mỗi 60 s có thêm một lượt ISAPI sai. Giảm nhẹ: `AUTH_FAILED` từ ISAPI/PSIA dừng dò ngay (đã đúng). Đề
  xuất thêm: sau `AUTH_FAILED`, giãn chu kỳ thử lại (60 s → 5 phút). Áp cho mọi giao thức nên đổi hành vi ONVIF; chỉ
  làm nếu Q6 đồng ý.
- **R3: URL profile ISAPI không khớp URL luồng đã cấu hình.** Chọn dạng `/Streaming/Channels/<id>` (Q4) cho trường hợp
  phổ biến. Người dùng cấu hình `/ISAPI/Streaming/channels/101`, có `?transportmode=` hoặc khác hoa/thường thì vẫn lệch,
  như giới hạn PSIA hiện tại.
- **R4: tách logic dò khỏi `CameraController` làm đổi hành vi camera.** S1 là bước thuần refactor, giữ nguyên toàn bộ
  `CameraProtocolTest`.
- **R5: pusher ISAPI chiếm phiên two-way.** Mỗi thiết bị chỉ một phiên. Pusher phải `close` cả khi lỗi hoặc huỷ giữa
  chừng, nếu không camera bận tới khi hết timeout phía thiết bị.
- **R6: thread.** Mọi lời gọi `CameraDeviceControl` của speaker phải qua `_device_queue`. Pusher ISAPI chạy trên
  poller, không gọi gSOAP. Không capture `this` raw.
- **R7: loa Bosch.** Plugin cần ONVIF; ép ONVIF ở 4.2 tránh việc `AUTO` khoá loa Bosch sang giao thức khác.

## 7. Không thay đổi

- Không migration. Không đụng `api/` (C ABI), `SubnetScan`, `MediaGapFillManager`.
- Không đổi tên struct `Onvif*`, field JSON hay tên chế độ phát `BACKCHANNEL`/`VENDOR_API_FILE`/`AUTO`.
- `ext-plugin/IDevice` giữ vai trò plugin API hãng (file âm thanh, volume, tạo user). Không dồn điều khiển giao thức
  vào đó.

## 8. Câu hỏi cần chốt

Mỗi câu có đề xuất kèm theo. Câu nào không trả lời thì làm theo đề xuất.

- **Q1.** Thứ tự `AUTO` mới: `onvif,isapi,psia`? (Đề xuất: có. Camera đã khoá không đổi.)
- **Q2.** Lúc dò `AUTO`, ONVIF `AUTH_FAILED` có cho thử ISAPI/PSIA không? (Đề xuất: có, chỉ trong lượt dò. Camera đã
  khoá giữ luật cũ.)
- **Q3.** Loa cần hỗ trợ cụ thể model nào? Two-way audio ISAPI (S4) có làm đợt này không? (Đề xuất: làm S1–S3 trước, S4
  khi có thiết bị Hikvision để kiểm. Loa PSIA chỉ có thông tin/trạng thái.)
- **Q4.** URL profile ISAPI dùng dạng `rtsp://ip:port/Streaming/Channels/<id>`? (Đề xuất: có.)
- **Q5.** API file âm thanh của loa Hikvision (tương đương Bosch) để đợt sau? (Đề xuất: để sau.)
- **Q6.** Giãn chu kỳ thử lại sau `AUTH_FAILED` (R2) cho mọi giao thức? (Đề xuất: chưa làm, chờ kết quả I0.)

## 9. Kết quả

Q1–Q6 làm theo đề xuất: thứ tự `onvif,isapi,psia`; ONVIF `AUTH_FAILED` lúc dò vẫn thử tiếp; S4 để khi có thiết bị
Hikvision; URL `/Streaming/Channels/<id>`; API file loa Hikvision để sau; chưa giãn chu kỳ sau `AUTH_FAILED`.

**I0 — xong (mock), chưa spike thiết bị thật.** `psia_mock.py --dialect isapi` (namespace Hikvision, DeviceInfo
không có manufacturer, channel không có `rtspPortNo`, `adminAccesses`, TwoWayAudio open/close/audioData, mở lần hai
403 `deviceBusy`), `--onvif-401`, `--audio-out`. Kiểm bằng `curl --digest`. Chưa đo 401/404 ONVIF trên Hikvision
thật (R1) — làm ở bước E.

**I1–I2 — xong.** Khác plan:
- Bảng tên `HttpXmlDialect` (không phải `HttpXmlPaths`) vì có thêm tên giao thức, khoá timeout, đường RTSP và
  manufacturer mặc định, ngoài đường dẫn.
- `IsapiControl` override đúng một điểm `defaultRtspPort()`; URL, codec và manufacturer (`Hikvision`, vì
  `deviceName` của Hikvision là tên người dùng đặt) đi qua bảng.
- `IsapiControl` nằm trong `PsiaDeviceControl.{h,cpp}` (không thêm file, không phải reconfigure).
- Unit: `IsapiParseTest`, `IsapiMockTest` (chạy với mock), `CameraProtocolTest` thêm 3 case (ONVIF 401 → ISAPI,
  ISAPI 401 dừng và báo lỗi ONVIF, ONVIF đã khoá không đổi).

**S1 — xong, không đổi hành vi.** `makeDeviceControl`, `parseProtocolOrder`, `probeControlProtocols`,
`ProtocolLock` trong `CameraDeviceControl.{h,cpp}`. `ProtocolLock` có `setSetting`/`restore` riêng thay cho
`configure(setting, last)`, vì camera đặt hai thứ ở hai thời điểm. `CameraProtocolTest` giữ nguyên và đạt,
thêm `ProtocolLockTest`, `ProtocolOrderTest`.

**S2–S3 — xong, một commit.** Khác plan / cần biết:
- Trước đây `SpeakerStatistic` **không lưu** `isOnvifDevice` (đọc nhưng không ghi) và `load()` không khôi phục
  caps. Nay lưu `isOnvifDevice` + `controlProtocol` và khôi phục hai field đó. Loa cũ chưa có dữ liệu nên lần đầu
  sau nâng cấp dò `AUTO` (ONVIF trước), kết quả như cũ với loa ONVIF.
- Âm lượng (`audioVolumeInfo` từ plugin) chỉ cập nhật vào caps khi kết nối lại, không còn mỗi 60 s, vì loa online
  chỉ `probeConnection`. Giá trị đặt qua API vẫn ghi thẳng vào statistic như trước.
- `onvifUsername/onvifPassword` của option chỉ ghi khi loa chạy ONVIF.
- `runVendorOp`: lỗi "file chưa sẵn sàng" và "upload hỏng" của `playAudioFileByVendorApi` nay disconnect trước khi
  trả lời (trước đây bỏ phiên hãng mở).
- Unit: `SpeakerProtocolTest` 6 case trong `test_camera_controller.cpp` (dùng chung fake); target link thêm
  `ext-plugin`.

**I3 — chạy thật đạt (2026-10-04).** MediaServer Debug (harness `e2e_real`) + `psia_mock.py --dialect isapi
--onvif-401 --rtsp-port 18554` + `rtsp_source.py`, luồng `rtsp://127.0.0.1:18554/Streaming/Channels/101|102`:
- `AUTO`: ONVIF `AUTH_FAILED` (401) → ISAPI connect, khoá ISAPI (`Hikvision DS-2DE4425IW-DE, 2 streaming channel(s)`).
- API qua ISAPI đều `code 0`: `ptz_control` left/zoomIn, `get_presets`, `goto_preset` (camera + người dùng),
  `set_preset`, `imageMoveControl` focusIn/irisOut, `relayOutputControl` relayOn, `onvifGetConfigProfiles` (URL
  `rtsp://127.0.0.1:18554/Streaming/Channels/101`, khớp URL luồng đã cấu hình; tuỳ chọn encoder từ capabilities),
  `onvifSetVideoConfigs` kênh 102. State mock khớp (kênh 102 1280x720/768/10 fps, relay high, preset 2, giờ lệch
  300 s đặt lại `manual`). Mọi request đi dưới `/ISAPI/...`, có `GET /ISAPI/Security/adminAccesses`.
- Restart: `AUTO` dùng ngay ISAPI, không dò.
- Đổi camera sang mock PSIA: `ISAPI` cố định báo `NOT_THIS_PROTOCOL`, không thử giao thức khác; đổi sang `AUTO`
  → ONVIF 404 → ISAPI 404 → PSIA, log đổi giao thức ISAPI → PSIA (xoá profile config/rule/preset/stream_settings).
- Hồi quy ONVIF: trỏ sang `onvif_mock.py` → `AUTO` ra ONVIF ngay.
- Không crash. Chưa chạy `onvifApplyVideoConfigRule` (cần API key; luồng rule chung với PSIA đã chạy ở plan PSIA).
- Loa chưa chạy thật: harness `e2e_real` chỉ dựng camera. Phần loa có unit test.

**S5 — xong.** `docs/camera-control-protocol.md` (ISAPI, luật 401 lúc dò, ánh xạ ISAPI, mục Loa, config),
`conf/config.ini` (`[camera]` order mới, `[speaker]`, `[isapi]`).

**S4 — xong trên mock, chưa kiểm thiết bị Hikvision.** Khác plan / cần biết:
- `AudioBackchannelInfo` nằm ở `src/Backchannel/IBackchannelPusher.h` (namespace `mediakit`, `CameraDeviceControl.h`
  dùng lại bằng `using`) vì `BackchannelManager` ở tầng `mediakit` không được phụ thuộc `manager/`. Thêm field
  `username`/`password` cho ISAPI; RTSP vẫn nhúng credential vào URI như cũ.
- `supportsAudioBackchannel()` không `const`: ISAPI phải `GET /ISAPI/System/TwoWayAudio/channels` (blocking, trên
  device queue như phần còn lại của `publishConnected`). Chỉ nhận G.711ulaw/alaw; codec khác log `WarnL` và báo không
  hỗ trợ (không tự đổi `audioCompressionType` của thiết bị).
- Có interface `IBackchannelPusher` (callback chuyển lên interface); `BackchannelPusher` RTSP chỉ đổi lớp cha.
- Pusher tự viết HTTP trên `TcpClient` (`HttpClient` sẵn có chỉ có Basic). `audioData` gửi `Content-Length: 0`,
  thiết bị trả 200 rồi đọc G.711 thô trên cùng kết nối (cách go2rtc làm với Hikvision); mock được sửa theo đúng kiểu
  này. `close` đi trên kết nối mới; pusher tự giữ mình (`_self_hold`) tới khi `close` có trả lời hoặc hết 5 s, nên
  `BackchannelManager` bỏ con trỏ ngay vẫn đóng được phiên (R5).
- **Busy không thành `StartResult::DeviceBusy`:** 403 `deviceBusy` về bất đồng bộ sau khi `startLiveSession`/
  `playFile` đã trả. Pusher báo `onFailed` với custom code `kDeviceBusyCode`, `BackchannelManager` log "device busy"
  và dừng phiên, giống lỗi bắt tay RTSP hiện nay. Muốn trả `DeviceBusy` cho client thì phải đổi API phát sang chờ kết
  quả bắt tay, cho cả RTSP.
- Không gửi `close` trước `open` (go2rtc làm thế để dọn phiên treo) vì sẽ cướp phiên của client khác. Phiên treo do
  server chết giữa chừng tự hết theo timeout của thiết bị.
- Unit: `IsapiParseTest.TwoWayAudioChannel`; `IsapiMockTest.TwoWayAudioSendsG711AndCloses` (cần `ISAPI_MOCK_ADDR`
  và `ISAPI_MOCK_STATE`): `BackchannelManager::registerDevice(id, info)` → `startLiveSession` → 50 frame PCM 8 kHz →
  `stopSession`. Mock nhận đúng 8000 byte và khớp từng byte với μ-law của tín hiệu gửi; `open` thứ hai trong lúc
  đang phát nhận `kDeviceBusyCode` và không làm đóng phiên đầu; trình tự trên mock `open → audioData → open(403) →
  close`, `two_way_open` về `false`.
- `IsapiMockTest.TwoWayAudioPlaysWholeFile`: `playFile` WAV 1 s → mock nhận đủ 8000 byte, khớp từng byte, `close`
  tự gửi khi file hết.
- **Hồi quy RTSP backchannel ONVIF** với mock mới `tests/integration/stress_vms/rtsp_backchannel_mock.py` (Digest,
  `Require` backchannel, SETUP `/audioback`, RTP/TCP interleaved). `RtspBackchannelMockTest` (cần
  `RTSP_BC_MOCK_ADDR` + `RTSP_BC_MOCK_STATE`):
  - live qua `registerDevice(id, AudioBackchannelInfo)` (dạng `SpeakerController` dùng từ S4): DESCRIBE 401 →
    DESCRIBE/SETUP/PLAY có Digest → 50 gói RTP PT 0, 8000 byte khớp từng byte μ-law, seq liên tục, RTCP SR + BYE,
    TEARDOWN; phiên live thứ hai cùng thiết bị trả `StartResult::DeviceBusy`;
  - `playFile` qua `registerDevice(id, url)` (dạng cũ): đủ 8000 byte, file hết thì TEARDOWN tự gửi.
- Chưa chạy: loa ISAPI/ONVIF đi qua `SpeakerController` trong MediaServer (loa và file âm thanh đến từ đồng bộ ESC),
  `OnvifControl::setupAudioBackchannel` với thiết bị (logic SOAP không đổi), thiết bị Hikvision thật. Cần kiểm trên thiết bị: `audioData` cùng kết nối với `open` có được không (pusher tự nối lại nếu thiết bị
  trả `Connection: close`), có cần `?sessionId=` không, nhịp gửi 20 ms có đủ mượt.

**E — chưa làm:** camera Hikvision tắt ONVIF ở `AUTO` (đo R1), Hikvision ở `ISAPI` (PTZ relative), camera ONVIF
thật, loa Bosch hồi quy (đặc biệt `runVendorOp` và ép ONVIF), two-way audio Hikvision (các điểm ở S4).
