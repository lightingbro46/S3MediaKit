# Điều khiển camera bằng ONVIF hoặc PSIA

MediaServer điều khiển camera (thông tin thiết bị, profile và URL luồng, cấu hình encoder, video config rule,
PTZ, preset, focus/iris, relay, đồng bộ giờ) bằng **ONVIF** hoặc **PSIA**. Mỗi camera tại một thời điểm chỉ dùng
**một** giao thức. Không có trường hợp PTZ đi PSIA trong khi profile đi ONVIF.

PSIA ở đây là chuẩn REST/XML dưới đường dẫn `/PSIA/...`, xác thực HTTP Digest (Basic nếu camera chỉ hỗ trợ
Basic). ISAPI của Hikvision (`/ISAPI/...`) chưa hỗ trợ.

Thiết kế và tiến độ: [psia-device-control-plan.md](./psia-device-control-plan.md).

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
| `AUTO` (mặc định; thiếu field hoặc giá trị lạ cũng là `AUTO`) | Thử ONVIF trước, rồi PSIA. Giao thức nào kết nối được thì camera **khoá** vào giao thức đó, kể cả sau khi restart. |
| `ONVIF` | Chỉ ONVIF. Không bao giờ thử PSIA, kể cả khi ONVIF lỗi mãi. |
| `PSIA` | Chỉ PSIA. Không bao giờ thử ONVIF. |

Chế độ `AUTO` chỉ chuyển sang giao thức kế tiếp khi camera trả lời rõ là không nói giao thức đó: HTTP 404/405,
hoặc nội dung trả về không phải SOAP (ONVIF) / không phải `DeviceInfo` (PSIA). Camera mất mạng, quá thời gian
hoặc sai mật khẩu **không** làm camera bị chuyển giao thức. Khi đó camera báo offline kèm lỗi, và hệ thống thử lại
sau 60 giây.

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
| `controlProtocol` | Giao thức đang dùng: `"ONVIF"`, `"PSIA"`, hoặc `""` khi chưa kết nối được lần nào |
| `controlProtocolSetting` | Giá trị đã cấu hình: `"AUTO"`, `"ONVIF"` hoặc `"PSIA"` |
| `enableOnvifProfileSelect` | `true` khi camera có controller (ONVIF hoặc PSIA) và đã báo ít nhất một profile: client hiện mục chọn profile cho luồng chính/phụ (`onvifProfiles`) theo cờ này |

Các field cũ giữ nguyên tên. Camera PSIA dùng chung các field đó:
- `manufacturer`, `model`, `serialNumber`, `firmwareVersion`, `hardwareId` (= `deviceID` của PSIA), `macAddress`,
  `webPage`.
- `onvifProfiles.isPTZ`, `onvifProfiles.ptzControlMode`, `onvifProfiles.profiles` (token = id kênh PSIA,
  ví dụ `101`).
- `enableOnvifProfileConfig` = true khi camera có controller (ONVIF hoặc PSIA).

**Lưu ý cho client:** `onvifDevice` vẫn chỉ true khi camera đang chạy ONVIF. Client hiện từng mục theo cờ chức
năng, không theo tên giao thức:
- mục chọn profile (`onvifProfiles`): `enableOnvifProfileSelect` (trước đây là `onvifDevice`);
- tab cấu hình thông số stream theo token profile: `enableOnvifProfileConfig` (không đổi);
- cấu hình chế độ PTZ: `onvifProfiles.isPTZ`, `onvifProfiles.ptzControlMode` (không đổi);
- `supportsSdCardPlayback`, `supportsAudioBackchannel`: chỉ có với ONVIF.

`controlProtocol` dùng để hiện nhãn giao thức. Thêm giao thức mới (ví dụ ISAPI) không cần sửa client.

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

Giới hạn hiện tại:
- URL luồng sinh ra phải khớp nguyên chuỗi với URL luồng đã cấu hình cho camera thì cấu hình encoder và video
  config rule mới tìm được profile. Nếu luồng cấu hình bằng đường dẫn khác (ví dụ `/Streaming/Channels/101`),
  hai chức năng này không tìm được profile của luồng.
- Focus/iris bật khi camera có kênh PTZ, vì PSIA không có cờ năng lực riêng cho ống kính.

## Config toàn cục

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `camera.control_auto_order` | `onvif,psia` | Thứ tự thử khi `AUTO` |
| `camera.control_reprobe_failures` | `5` | Số lần liên tiếp báo "không phải giao thức này" trước khi camera đã khoá dò lại |
| `psia.request_timeout_ms` | `5000` | Timeout mỗi request PSIA |

## Kiểm thử

- Unit test: `unit.camera_controller_all` (`CameraProtocolTest`), `unit.psia_control_all`.
- Camera PSIA giả: `tests/integration/stress_vms/psia_mock.py --port 18680` (Digest, `admin`/`admin123`;
  `--basic` cho Basic). Chạy `PSIA_MOCK_ADDR=127.0.0.1:18680 release/linux/Debug/unit_psia_control` để chạy cả
  các case gọi mock.
