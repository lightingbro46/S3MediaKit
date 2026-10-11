# Thiết kế plugin hãng (camera, loa)

Tài liệu này là khuôn cho mọi plugin hãng trong `ext-plugin/`. Đọc trước khi thêm hãng mới hoặc thêm tính năng
qua API hãng. Cơ chế chọn giao thức điều khiển ở [camera-control-protocol.md](camera-control-protocol.md).

## 1. Nguyên tắc: API hãng là kênh song song, không phải giao thức

Một thiết bị có hai kênh:

| | Kênh giao thức | Kênh hãng (plugin) |
|---|---|---|
| Gồm | ONVIF, ISAPI, PSIA (`DeviceControl`, `ProtocolLock`, AUTO) | lớp `IDevice` của hãng trong `ext-plugin/<Hãng>/` |
| Có khi | theo cấu hình hoặc dò AUTO, không cần biết hãng | chỉ khi biết hãng (khai `manufacturer` hoặc nhận ra) |
| Làm | PTZ, profile, encoder, giờ, focus/iris, relay, replay thẻ SD, backchannel | 1. nhận hãng · 2. chuẩn bị giao thức (bật ONVIF, tạo tài khoản) · 3. tính năng giao thức không có |
| Số lượng | một giao thức tại một thời điểm | luôn chạy bên cạnh, không thay giao thức |

- Plugin **không** cài lại chức năng mà giao thức đã có. Mỗi tính năng chỉ có một đường đi.
- Một API hãng mà nhiều hãng khác cũng nói (ISAPI: các hãng OEM của Hikvision) và đủ chức năng thì là
  **giao thức** (`IsapiControl`), không phải plugin. Plugin cùng hãng vẫn dùng API đó cho phần việc của kênh hãng.
- Kênh hãng lỗi không làm thiết bị offline và không làm dò lại giao thức. Giao thức lỗi cũng không đụng tới kênh hãng.

## 2. Pattern

| Pattern | Ở đâu | Quy tắc |
|---|---|---|
| Registry + đặc tính khai báo | `DeviceFactory::plugins()`, `VendorPluginInfo` (`ext-plugin/IDevice.h`) | Mỗi hãng một dòng. Controller rẽ nhánh theo **đặc tính**, không bao giờ `if (brand == X)` trong `manager/` |
| Factory | `DeviceFactory::create()` | Nơi duy nhất ánh xạ brand → lớp plugin và include header của hãng |
| Template Method | `VendorCamera` (`ext-plugin/VendorCamera.h`) | Lớp chung lo HTTP + Digest/Basic (`request()`), timeout, `DeviceFailure`, khoá. Hãng chỉ cho phần riêng: `deviceInfoPath()`, `parseDeviceInfo()` |
| Interface năng lực | `IAudioPlayback`, `IOnvifSetup`, … | Mỗi năng lực một interface nhỏ. Controller lấy bằng `dynamic_cast`; không có thì `Not supported`. `IDevice` chỉ giữ phần **mọi** plugin đều có |

### Đặc tính trong registry

| Trường | Nghĩa | Đang dùng |
|---|---|---|
| `name` | tên ghi ngược khi nhận hãng qua API hãng | mọi plugin |
| `ownControlAccount` | plugin tạo user ONVIF riêng (`speaker.onvifUsername`, `camera.onvifUsername`) | Bosch, Hikvision |
| `enablesOnvif` | plugin bật ONVIF qua API hãng khi đang tắt | Hikvision, Hanwha |
| `requiredProtocol` | giao thức plugin bắt buộc (chuỗi, ví dụ `"ONVIF"`); rỗng = theo cấu hình | Bosch (backchannel cần ONVIF) |
| `sharedDsp` | backchannel và file âm thanh của plugin dùng chung DSP | Bosch |

Thêm đặc tính mới khi một hãng cần controller cư xử khác: thêm trường có mặc định trung lập, controller đọc trường
đó. Registry viết inline trong `DeviceFactory.h` vì `SpeakerStatistic` đọc nó trong các target test không link
`ext-plugin`; vì vậy `DeviceFactory.h` và `IDevice.h` không được include `manager/Control/`. File `.cpp` của plugin
thì được dùng lại parser có sẵn (ví dụ `psia_detail::parseDeviceInfo`).

### Interface năng lực hiện có

| Interface | Việc | Cài đặt |
|---|---|---|
| `IAudioPlayback` | file âm thanh, volume của loa | `BoschIpSpeaker` |
| `IOnvifSetup::prepareOnvif(username, password, cb)` | bật ONVIF; `username` khác rỗng thì tạo user ONVIF hoặc ghi lại mật khẩu | `BoschIpSpeaker` (chỉ tạo user), `HikvisionCamera` (bật + tạo/cập nhật user), `HanwhaCamera` (chỉ bật) |

Tính năng hãng mới (ví dụ cấu hình sự kiện AI) là một interface mới cùng kiểu, không thêm vào `IDevice`.

## 3. Ràng buộc với mọi plugin

- **Bất đồng bộ.** Gọi HTTP qua `VendorHttp` (`vendorHttpRequest`, `VendorCamera::request`) hoặc SDK có callback.
  Không chặn poller. Callback chạy trên poller HTTP; plugin tự khoá dữ liệu của mình.
- **Vòng đời.** Không capture `this` trần: dùng `weak_ptr` tới chính plugin. Người gọi giữ plugin sống bằng cách
  capture `shared_ptr` của nó trong callback (xem `VendorStage::probe`, `CameraController::prepareVendorOnvif`).
- **Nhận diện chặt.** `connect()` chỉ thành công khi nội dung đúng là của hãng mình (HTTP 200 **và** parse đúng
  tài liệu của hãng): mọi plugin cùng loại được dò lần lượt trên cùng một thiết bị.
- **Ghi idempotent.** Đọc trước, chỉ ghi khi khác. Giữ nguyên các node/khóa không liên quan khi ghi lại tài liệu
  của thiết bị. Không đọc được giá trị (mật khẩu) thì ghi lại một lần mỗi khi controller được cấu hình, không mỗi chu kỳ.
- **Phân loại lỗi** qua `lastFailure()`: `Unauthorized` chỉ khi chính tài khoản bị từ chối (401 ở lần đọc đầu);
  401/403 khi ghi là tài khoản không có quyền, báo `Other`. Controller dựa vào khác biệt này: `Unauthorized` thì
  bỏ dò giao thức trong chu kỳ (chống khoá tài khoản), `Other` thì chờ 1 giờ rồi thử lại.
- **Không log mật khẩu.** Thông báo lỗi chỉ gồm bước và mã HTTP.

## 4. Chu kỳ của controller

```
onManager (60 s, ReconnectPacer):
    detecting (manufacturer rỗng) → VendorStage::probe: hỏi API từng plugin theo thứ tự registry
        nhận ra hãng → chuẩn bị ngay trong chu kỳ này (nếu cần)
        không nhận ra → giao thức; giao thức kết nối được thì deviceInfo của nó đặt tên hãng
    có plugin && (enablesOnvif || ownControlAccount) && chưa chuẩn bị → IOnvifSetup::prepareOnvif
        thành công     → ONVIF dùng camera.onvifUsername (nếu ownControlAccount)
        Unauthorized   → báo AUTH, không dò giao thức trong chu kỳ này
        Unreachable    → chu kỳ sau thử lại
        lỗi khác       → ONVIF dùng tài khoản cấu hình, thử lại sau 1 giờ
    → giao thức (connect / dò AUTO), trong cùng chu kỳ
```

- Tài khoản theo giao thức: `CameraController::controlUsername(protocol)`. ONVIF dùng user của plugin khi plugin
  tạo nó; ISAPI/PSIA luôn dùng tài khoản cấu hình. Gap fill (replay thẻ SD qua ONVIF) dùng tài khoản của ONVIF.
- Không đổi giao thức đang khoá vì kênh hãng vừa chuẩn bị xong (đổi giao thức xoá profile và preset); không thay
  controller đang kết nối chỉ để đổi tài khoản.
- Không chuẩn bị khi `camera.prepareOnvif=0` hoặc camera cố định ISAPI/PSIA.
- Dò hãng dừng ngay khi plugin đầu báo `Unreachable` (không kết nối được cổng web). `Timeout` thì vẫn hỏi plugin
  sau, vì có web server bỏ ngang kết nối với đường dẫn lạ.

## 5. Thêm một hãng

Ví dụ camera Dahua:

1. `DeviceBrand::DAHUA` trong `ext-plugin/IDevice.h`, tên và alias trong `getDeviceBrand()`.
2. `ext-plugin/Dahua/DahuaCamera.{h,cpp}`: kế thừa `VendorCamera` (đường dẫn + parser deviceInfo); thêm
   `IOnvifSetup` hay interface năng lực khác nếu hãng cần.
3. Một dòng trong `DeviceFactory::makeCameraPlugins()` với đặc tính đúng. Vị trí dòng là thứ tự dò: hãng có dấu
   hiệu chắc chắn đứng trước.
4. Một `case` trong `DeviceFactory::createCamera()`.
5. Cấu hình lại CMake: `ext-plugin` dùng `file(GLOB_RECURSE)`, thư mục build cũ không thấy file mới.
6. Kiểm thử:
   - parser thuần trong `tests/unit/manager/test_vendor_stage.cpp` (tài liệu thật của hãng, tài liệu của hãng
     khác, rỗng);
   - một mock HTTP trong `tests/integration/stress_vms/` và test `VendorCameraMockTest.*` bỏ qua khi thiếu biến
     môi trường `<HÃNG>_MOCK_ADDR`;
   - `CameraProtocolTest` chỉ khi controller cần đặc tính mới.

Không sửa `CameraController` hay `SpeakerController` cho một hãng. Nếu thấy cần, đó là dấu hiệu thiếu một đặc
tính trong registry hoặc một interface năng lực.

## 6. Plugin hiện có

| Plugin | Loại | Nhận hãng | Chuẩn bị | Tính năng hãng |
|---|---|---|---|---|
| `BoschIpSpeaker` | loa | đăng nhập API Bosch | tạo user ONVIF `speaker.onvifUsername` | file âm thanh, volume (`IAudioPlayback`) |
| `HikvisionCamera` | camera | `GET /ISAPI/System/deviceInfo` | bật ONVIF (`/ISAPI/System/Network/Integrate`), tạo/cập nhật user administrator `camera.onvifUsername` (`/ISAPI/Security/ONVIF/users`) | — |
| `HanwhaCamera` | camera | SUNAPI `system.cgi?msubmenu=deviceinfo` | bật ONVIF qua SUNAPI (`network.cgi?msubmenu=onvif`); ONVIF dùng user web | — |

Chưa kiểm trên thiết bị thật: đường dẫn ISAPI chuẩn bị ONVIF của Hikvision và CGI ONVIF của Hanwha (hiện chỉ đối
chiếu với `psia_mock.py --dialect isapi --onvif-off` và `sunapi_mock.py --onvif-off`).
