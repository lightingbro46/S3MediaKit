# Điều khiển camera và loa bằng ONVIF, ISAPI hoặc PSIA

MediaServer điều khiển camera (thông tin thiết bị, profile và URL luồng, cấu hình encoder, video config rule,
PTZ, preset, focus/iris, relay, đồng bộ giờ) bằng **ONVIF**, **ISAPI** hoặc **PSIA**. Mỗi camera tại một thời điểm
chỉ dùng **một** giao thức. Không có trường hợp PTZ đi PSIA trong khi profile đi ONVIF. Loa IP dùng cùng cơ chế
chọn giao thức (mục [Loa](#loa)).

PSIA ở đây là chuẩn REST/XML dưới đường dẫn `/PSIA/...`, xác thực HTTP Digest (Basic nếu camera chỉ hỗ trợ
Basic). ISAPI là REST/XML của Hikvision dưới `/ISAPI/...`, phát triển từ PSIA: cùng tài liệu XML, cùng cách xác
thực, khác đường dẫn.

Thiết kế và tiến độ: [psia-device-control-plan.md](./psia-device-control-plan.md),
[isapi-speaker-control-plan.md](./isapi-speaker-control-plan.md).

## Cấu hình theo camera

Trường mới trong body tạo/sửa camera:

```json
{
  "cameraAdvanceConfig": {
    "control": { "protocol": "AUTO" }
  }
}
```

| Giá trị | Hành vi |
|---|---|
| `AUTO` (mặc định; thiếu field hoặc giá trị lạ cũng là `AUTO`) | Thử ONVIF, rồi ISAPI, rồi PSIA. Giao thức nào kết nối được thì camera **khoá** vào giao thức đó, kể cả sau khi restart. |
| `ONVIF` | Chỉ ONVIF. Không bao giờ thử giao thức khác, kể cả khi ONVIF lỗi mãi. |
| `ISAPI` | Chỉ ISAPI. Dùng cho camera Hikvision. |
| `PSIA` | Chỉ PSIA. |

Chế độ `AUTO` chỉ chuyển sang giao thức kế tiếp khi camera trả lời rõ là không nói giao thức đó: HTTP 404/405,
hoặc nội dung trả về không phải SOAP (ONVIF) / không phải `DeviceInfo` (ISAPI, PSIA). Camera mất mạng hoặc quá thời
gian **không** làm camera bị chuyển giao thức. Khi đó camera báo offline kèm lỗi, và hệ thống thử lại sau 60 giây.

Sai mật khẩu cũng dừng lượt dò, trừ một trường hợp: trong lượt dò, ONVIF trả 401/403 thì vẫn thử ISAPI và PSIA.
Camera Hikvision tắt ONVIF, hoặc chưa tạo user ONVIF (tài khoản ONVIF của Hikvision tách khỏi tài khoản web), trả
401 cho ONVIF trong khi tài khoản web vẫn dùng được cho ISAPI. Nếu không giao thức nào kết nối được, lỗi báo ra là
lỗi ONVIF. ISAPI/PSIA trả 401 thì dừng ngay, để camera không bị khoá IP vì đăng nhập sai nhiều lần. Camera đã khoá
vào ONVIF vẫn giữ luật cũ: 401 không làm nó chuyển giao thức.

Camera đã khoá chỉ dò lại giao thức khi:
- địa chỉ, cổng, tài khoản hoặc `control.protocol` của camera đổi;
- hoặc `camera.control_reprobe_failures` lần kết nối liên tiếp (mặc định 5, khoảng 5 phút) đều báo "không phải
  giao thức này".

Camera ONVIF đang có không phải làm gì: capabilities cũ có `isOnvifDevice=true` được đọc thành giao thức `ONVIF`,
nên camera khoá ngay vào ONVIF và chạy như trước.

## API trả về

Webhook capabilities của thiết bị (`hook.on_device_caps_changed`, dựng bởi `makeDeviceCapabilitiesJson`) có thêm:

| Field | Ý nghĩa |
|---|---|
| `controlProtocol` | Giao thức đang dùng: `"ONVIF"`, `"ISAPI"`, `"PSIA"`, hoặc `""` khi chưa kết nối được lần nào |
| `controlProtocolSetting` | Giá trị đã cấu hình: `"AUTO"`, `"ONVIF"`, `"ISAPI"` hoặc `"PSIA"` |
| `enableOnvifProfileSelect` | `true` khi camera có controller (bất kỳ giao thức nào) và đã báo ít nhất một profile: client hiện mục chọn profile cho luồng chính/phụ (`onvifProfiles`) theo cờ này |

Các field cũ giữ nguyên tên. Camera ISAPI/PSIA dùng chung các field đó:
- `manufacturer`, `model`, `serialNumber`, `firmwareVersion`, `hardwareId` (= `deviceID` của ISAPI/PSIA),
  `macAddress`, `webPage`. `DeviceInfo` của Hikvision không có `manufacturer`, nên camera ISAPI báo `Hikvision`.
- `onvifProfiles.isPTZ`, `onvifProfiles.ptzControlMode`, `onvifProfiles.profiles` (token = id kênh,
  ví dụ `101`).
- `enableOnvifProfileConfig` = true khi camera có controller (bất kỳ giao thức nào).

**Lưu ý cho client:** `onvifDevice` vẫn chỉ true khi camera đang chạy ONVIF. Client hiện từng mục theo cờ chức
năng, không theo tên giao thức:
- mục chọn profile (`onvifProfiles`): `enableOnvifProfileSelect` (trước đây là `onvifDevice`);
- tab cấu hình thông số stream theo token profile: `enableOnvifProfileConfig` (không đổi);
- cấu hình chế độ PTZ: `onvifProfiles.isPTZ`, `onvifProfiles.ptzControlMode` (không đổi);
- `supportsSdCardPlayback`, `supportsAudioBackchannel`: chỉ có với ONVIF.

`controlProtocol` dùng để hiện nhãn giao thức. Client chỉ cần thêm lựa chọn `ISAPI` vào ô chọn giao thức; các cờ
chức năng đã trung lập nên camera ISAPI hiện đúng các mục như camera PSIA.

Option camera (`makeCameraOptionJson`, dùng khi đồng bộ cluster) có thêm `controlProtocol`.

## Khi giao thức của camera đổi

Token profile và toạ độ PTZ của hai giao thức không dùng lẫn được. Khi camera kết nối bằng giao thức khác lần
trước:
- rule cấu hình video đang chờ kết quả được báo thất bại với mã `DEVICE_PROTOCOL_CHANGED`;
- cấu hình profile đã lưu (`stream_settings`) và preset người dùng của camera bị xoá;
- server ghi log `WarnL` có giao thức cũ và giao thức mới.

Lệnh PTZ/preset/imaging/relay gửi trong lúc đang dò giao thức trả `CODE_DEVICE_OFFLINE` (905002).

## Ánh xạ PSIA

| Chức năng | PSIA |
|---|---|
| Thông tin thiết bị, kiểm tra sống | `GET /PSIA/System/deviceInfo` |
| Profile + URL luồng | `GET /PSIA/Streaming/channels`; URL `rtsp://<ip>:<rtspPortNo>/PSIA/Streaming/channels/<id>` |
| Tuỳ chọn encoder | `GET /PSIA/Streaming/channels/<id>/capabilities` (thuộc tính `opt`, `min`, `max`) |
| Ghi encoder | `GET` rồi `PUT /PSIA/Streaming/channels/<id>`, giữ nguyên các node khác của hãng |
| PTZ | `/PSIA/PTZ/channels/<ch>/continuous`, `relative`, `absolute`, `status` |
| Preset, home | `/PSIA/PTZ/channels/<ch>/presets[/<id>/goto]`, `/homeposition/goto` |
| Focus, iris | `PUT /PSIA/System/Video/inputs/channels/<ch>/focus`, `/iris` |
| Relay | `GET /PSIA/IO/outputs`, `PUT /PSIA/IO/outputs/<id>/trigger` |
| Giờ | `GET`/`PUT /PSIA/System/time` (đặt `manual` khi lệch từ 10 giây) |

Phần nào camera trả 404 thì chức năng đó tắt (ví dụ không có PTZ), không làm hỏng kết nối.

## Ánh xạ ISAPI

Cùng tài liệu XML và cùng cách quy đổi toạ độ PTZ như PSIA (`IsapiControl` kế thừa `PsiaControl`), khác đường dẫn:

| Chức năng | ISAPI |
|---|---|
| Thông tin thiết bị, kiểm tra sống | `GET /ISAPI/System/deviceInfo` |
| Profile + URL luồng | `GET /ISAPI/Streaming/channels`; URL `rtsp://<ip>:<cổng RTSP>/Streaming/Channels/<id>` |
| Cổng RTSP | `GET /ISAPI/Security/adminAccesses` (mục `protocol=RTSP`), lỗi hoặc không có thì 554 |
| Tuỳ chọn encoder, ghi encoder | `/ISAPI/Streaming/channels/<id>/capabilities`, `GET`/`PUT /ISAPI/Streaming/channels/<id>` |
| PTZ, preset, home | `/ISAPI/PTZCtrl/channels/<ch>/continuous`, `relative`, `absolute`, `status`, `presets[/<id>/goto]`, `homeposition/goto` |
| Focus, iris | `PUT /ISAPI/System/Video/inputs/channels/<ch>/focus`, `/iris` |
| Relay | `GET /ISAPI/System/IO/outputs`, `PUT /ISAPI/System/IO/outputs/<id>/trigger` |
| Giờ | `GET`/`PUT /ISAPI/System/time` |

Codec `Smart264`/`Smart265`/`H.264+`/`H.265+` đọc thành `H264`/`H265`.

Giới hạn hiện tại:
- URL luồng sinh ra phải khớp nguyên chuỗi với URL luồng đã cấu hình cho camera thì cấu hình encoder và video
  config rule mới tìm được profile. Nếu luồng cấu hình bằng đường dẫn khác (ví dụ camera PSIA cấu hình
  `/Streaming/Channels/101`, hoặc camera ISAPI cấu hình `/ISAPI/Streaming/channels/101`, có `?transportmode=`,
  khác hoa/thường), hai chức năng này không tìm được profile của luồng. Camera ISAPI dùng dạng
  `/Streaming/Channels/<id>`, dạng người dùng Hikvision hay cấu hình.
- Focus/iris bật khi camera có kênh PTZ, vì PSIA/ISAPI không có cờ năng lực riêng cho ống kính.
- Phát lại thẻ nhớ (`supportsSdCardPlayback`) chỉ có với ONVIF Profile G.
- Lệnh PTZ relative của ISAPI chưa kiểm trên camera Hikvision thật.

## Loa

Loa IP chọn giao thức theo đúng luật của camera (cùng `ProtocolLock` và `probeControlProtocols` trong
`manager/Control/CameraDeviceControl.cpp`):

```json
{
  "speakerAdvanceConfig": {
    "control": { "protocol": "AUTO" }
  }
}
```

- Giá trị như camera: `AUTO` (mặc định, thử theo `speaker.control_auto_order`), `ONVIF`, `ISAPI`, `PSIA`. Loa
  đang có thiếu field là `AUTO` và thử ONVIF trước, nên chạy như cũ.
- Loa có plugin hãng (Bosch) luôn dùng ONVIF với tài khoản `speaker.onvifUsername`, vì plugin tạo tài khoản ONVIF
  đó và backchannel của loa cần ONVIF. Option khác `ONVIF` bị bỏ qua (log `WarnL`).
- Loa đang online thì mỗi chu kỳ 60 giây chỉ gửi một request kiểm tra (`probeConnection`), không chạy lại toàn bộ
  `connect()`. Chỉ khi kiểm tra lỗi mới kết nối lại và đăng ký lại backchannel.
- Giao thức đang dùng lưu ở capabilities của loa (`controlProtocol`) và được khôi phục khi khởi động.
- Backchannel (`supportsAudioBackchannel`, chế độ phát `BACKCHANNEL`) hiện chỉ có với ONVIF. Loa ISAPI/PSIA có
  thông tin thiết bị và trạng thái online; phát file chỉ qua plugin hãng nếu có. Two-way audio ISAPI để đợt sau
  (bước S4 của plan).

Webhook capabilities của loa có thêm `controlProtocol` và `controlProtocolSetting` như camera; `onvifDevice` giữ
nghĩa đang chạy ONVIF. Option loa (`makeSpeakerOptionJson`) có thêm `controlProtocol`.

## Config toàn cục

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `camera.control_auto_order` | `onvif,isapi,psia` | Thứ tự thử khi `AUTO`. Camera đã khoá không bị ảnh hưởng khi đổi |
| `camera.control_reprobe_failures` | `5` | Số lần liên tiếp báo "không phải giao thức này" trước khi camera đã khoá dò lại |
| `speaker.control_auto_order` | `onvif,isapi,psia` | Như trên, cho loa |
| `speaker.control_reprobe_failures` | `5` | Như trên, cho loa |
| `psia.request_timeout_ms` | `5000` | Timeout mỗi request PSIA |
| `isapi.request_timeout_ms` | `5000` | Timeout mỗi request ISAPI |

## Kiểm thử

- Unit test: `unit.camera_controller_all` (`CameraProtocolTest`, `ProtocolLockTest`, `SpeakerProtocolTest`),
  `unit.psia_control_all` (`PsiaParseTest`, `IsapiParseTest`, ...).
- Camera PSIA giả: `tests/integration/stress_vms/psia_mock.py --port 18680` (Digest, `admin`/`admin123`;
  `--basic` cho Basic). Camera ISAPI giả: thêm `--dialect isapi --rtsp-port 8554`; `--onvif-401` cho
  `/onvif/device_service` trả 401 như Hikvision tắt ONVIF.
- `PSIA_MOCK_ADDR=127.0.0.1:18680 ISAPI_MOCK_ADDR=127.0.0.1:18681 release/linux/Debug/unit_psia_control` chạy cả
  các case gọi mock.
