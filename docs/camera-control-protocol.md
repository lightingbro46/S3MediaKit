# Điều khiển camera và loa bằng ONVIF, ISAPI hoặc PSIA

MediaServer điều khiển camera (thông tin thiết bị, profile và URL luồng, cấu hình encoder, video config rule,
PTZ, preset, focus/iris, relay, đồng bộ giờ) bằng **ONVIF**, **ISAPI** hoặc **PSIA**. Mỗi camera tại một thời điểm
chỉ dùng **một** giao thức. Không có trường hợp PTZ đi PSIA trong khi profile đi ONVIF. Loa IP dùng cùng cơ chế
chọn giao thức (mục [Loa](#loa)).

PSIA ở đây là chuẩn REST/XML dưới đường dẫn `/PSIA/...`, xác thực HTTP Digest (Basic nếu camera chỉ hỗ trợ
Basic). ISAPI là REST/XML của Hikvision dưới `/ISAPI/...`, phát triển từ PSIA: cùng tài liệu XML, cùng cách xác
thực, khác đường dẫn.

Phần còn phải kiểm trên camera/loa thật: [pending-real-device-checks.md](./pending-real-device-checks.md).

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

### Sai mật khẩu: giãn chu kỳ thử lại

Khi lần kết nối (hoặc lượt dò) gần nhất kết thúc vì thiết bị từ chối tài khoản (`AUTH_FAILED`), lần thử kế tiếp
cách `camera.control_auth_retry_sec` giây (mặc định 300) thay vì 60 giây; loa dùng `speaker.control_auth_retry_sec`.
Nhiều thiết bị (Hikvision) khoá IP của client sau vài lần đăng nhập sai liên tiếp, nên mật khẩu sai chỉ tốn một lần
đăng nhập mỗi 5 phút. Kết nối được, lỗi khác (mất mạng, 404) hoặc đổi tài khoản/địa chỉ trong cấu hình thì quay về
60 giây ngay. Log `rejected the credentials: next attempt in N s` ghi một lần khi bắt đầu bị từ chối.

Camera ONVIF từ chối chữ ký WS-Security vì giờ camera lệch (`DeviceFailure::ClockSkew`) không tính là `AUTH_FAILED` mà là `OTHER`: chu kỳ đó đặt giờ
camera (khi bật `onvif.clockSyncEnable` và `enableTimeSync` của camera), thử lại sau 60 giây, và lượt dò AUTO dừng ở
ONVIF thay vì chuyển sang ISAPI/PSIA.

Người dùng sửa mật khẩu ngay trên thiết bị thì không phải chờ: client gọi `/media/mserver/device/reconnect` (mục
"API kết nối lại"). Sửa mật khẩu trên giao diện VMS thì không cần gọi: service API gọi `/incur`, cấu hình camera được
nạp lại và controller được tạo lại với mật khẩu mới.

Camera đã khoá chỉ dò lại giao thức khi:
- địa chỉ, cổng, tài khoản hoặc `control.protocol` của camera đổi;
- hoặc `camera.control_reprobe_failures` lần kết nối liên tiếp (mặc định 5, khoảng 5 phút) đều báo "không phải
  giao thức này".

Camera ONVIF đang có không phải làm gì: capabilities cũ có `isOnvifDevice=true` được đọc thành giao thức `ONVIF`,
nên camera khoá ngay vào ONVIF và chạy như trước.

### Plugin hãng chuẩn bị ONVIF

Camera biết hãng (khai `manufacturer`, hoặc `manufacturer` rỗng và plugin nhận ra) có plugin Hikvision hoặc Hanwha
thì trước các giao thức của chu kỳ, plugin chuẩn bị ONVIF qua API hãng, một lần cho mỗi bộ cấu hình camera:

| Hãng | Plugin làm | ONVIF đăng nhập bằng |
|---|---|---|
| Hikvision | bật ONVIF nếu đang tắt; tạo user ONVIF `camera.onvifUsername` (administrator, mật khẩu của camera), hoặc ghi lại mật khẩu nếu user đã có | `camera.onvifUsername` |
| Hanwha | bật ONVIF nếu đang tắt (firmware cũ) | tài khoản cấu hình |

ISAPI/PSIA luôn đăng nhập bằng tài khoản cấu hình. Replay thẻ nhớ (gap fill) dùng tài khoản của ONVIF. API hãng là
kênh song song, không phải một giao thức: nó không điều khiển PTZ/profile (thiết kế ở
[vendor-plugin-design.md](vendor-plugin-design.md)).

- Chuẩn bị không được (tài khoản không có quyền sửa cấu hình, firmware không có API đó): ONVIF dùng tài khoản cấu
  hình, `AUTO` vẫn dò tiếp ISAPI/PSIA như trước; plugin thử lại sau 1 giờ.
- API hãng từ chối chính tài khoản (401 ở lần đọc đầu): chu kỳ đó không dò giao thức nào (chúng dùng cùng mật khẩu),
  tính là `AUTH_FAILED` và giãn chu kỳ theo `camera.control_auth_retry_sec`.
- Camera đang khoá ISAPI không tự chuyển sang ONVIF khi plugin chuẩn bị xong; camera cố định `ISAPI`/`PSIA` không
  được chuẩn bị. `camera.prepareOnvif=0` tắt hẳn việc sửa cấu hình camera.

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

## API kết nối lại

`GET|POST /media/mserver/device/reconnect?deviceId=<id>`: JWT như các API điều khiển, người dùng phải có quyền với
thiết bị. Dùng cho camera và loa.

Chạy ngay chu kỳ kết nối của thiết bị, không chờ hết 60 giây hay `control_auth_retry_sec`, rồi trả kết quả của chính
lần thử đó:
- thiết bị đang online: một request kiểm tra (`probeConnection`), lỗi thì `connect()` đầy đủ;
- đang offline: `connect()` với giao thức đang dùng;
- `AUTO` chưa có giao thức: dò lại theo `control_auto_order`;
- một lượt dò đang chạy: chờ lượt đó xong.

```json
{"code": 0, "msg": "connected",
 "data": {"finished": true, "connected": true, "controlProtocol": "ISAPI", "error": ""}}
```

| Field | Ý nghĩa |
|---|---|
| `finished` | `false`: lần thử chưa xong sau 30 giây (dò nhiều giao thức với thiết bị không trả lời). Lần thử vẫn chạy, kết quả xem ở trạng thái thiết bị |
| `connected` | Lần thử kết nối được |
| `controlProtocol` | Giao thức của controller vừa thử: `ONVIF`, `ISAPI`, `PSIA`; rỗng khi không có |
| `error` | Rỗng khi kết nối được; không thì `AUTH_FAILED` (sai tài khoản), `UNREACHABLE`, `NOT_THIS_PROTOCOL`, `OTHER` |
| `msg` | Lỗi chi tiết của thiết bị, ví dụ `ISAPI GET /ISAPI/System/deviceInfo: HTTP 401` |
| `code` | Nguyên nhân theo mã lỗi thiết bị: `905004` sai tài khoản, `905005` không kết nối được, `905006` không trả lời, `905007` chỉ có Basic, `905008` lệch giờ; `0` khi kết nối được hoặc không có nguyên nhân cụ thể |
| `clock` | Chỉ khi `code` = `905008`: `serverTime`, `cameraTime`, `clockOffsetSec`, `source` như `data` của lỗi `905008` |

Thiết bị không tồn tại trả `905001`; thiết bị chưa cấu hình điều khiển (camera RTSP thuần) trả `code 0`,
`finished=true`, `connected=false`, `msg` ghi lý do. Camera đang chạy lệnh PTZ dài thì trả ngay `msg` "busy", không
chen vào lệnh đó. Sau khi kết nối được, chu kỳ quay về 60 giây.

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
`manager/Control/DeviceControl.cpp`):

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
- Đồng bộ giờ như camera (khi kết nối, mỗi `onvif.clockSyncIntervalSec`, ngay khi kết nối thất bại vì lệch giờ;
  lượt dò AUTO lệch giờ ở lại ONVIF). Tắt từng loa bằng `speakerAdvanceConfig.onvif.enableTimeSync=false`
  (mặc định `true`, có trong `makeSpeakerOptionJson`). Mã lỗi khi loa chưa sẵn sàng: xem
  [api-device-control-errors.md](api-device-control-errors.md) mục "Loa".
- Backchannel (`supportsAudioBackchannel`, chế độ phát `BACKCHANNEL`) theo giao thức đang dùng:
  - ONVIF: RTSP backchannel như trước (`BackchannelPusher`).
  - ISAPI: two-way audio Hikvision (`IsapiTwoWayAudioPusher`). Có khi `GET /ISAPI/System/TwoWayAudio/channels` trả
    một kênh G.711ulaw/G.711alaw; codec khác (G.726, AAC...) thì báo không hỗ trợ (log `WarnL`). Trình tự: `PUT
    .../channels/<id>/open` → `PUT .../audioData` với `Content-Length: 0` rồi gửi G.711 8 kHz thô trên chính kết nối
    đó → `PUT .../close` trên kết nối mới. Digest (qop=auth) hoặc Basic theo challenge. `close` luôn được gửi khi
    `open` đã thành công, kể cả khi lỗi hay bị dừng giữa chừng.
  - Thiết bị chỉ giữ một phiên two-way: đang có phiên khác thì `open` trả 403 `deviceBusy`, phiên mới dừng với log
    `device busy`. Lỗi này đến bất đồng bộ nên API phát vẫn trả thành công như lỗi kết nối RTSP hiện nay.
  - PSIA: không có. Loa PSIA có thông tin thiết bị và trạng thái online; phát file chỉ qua plugin hãng nếu có.

Webhook capabilities của loa có thêm `controlProtocol` và `controlProtocolSetting` như camera; `onvifDevice` giữ
nghĩa đang chạy ONVIF. Option loa (`makeSpeakerOptionJson`) có thêm `controlProtocol`.

## Sự kiện ONVIF

Camera điều khiển bằng ONVIF được đọc khả năng sự kiện mỗi lần `connect()` đầy đủ, giống PTZ: `Events.XAddr` và
`WSPullPointSupport` của `GetCapabilities`, rồi danh sách topic (`GetEventProperties`). Lượt kiểm tra 60 giây
(`probeConnection`) không đọc lại. Đọc lỗi không làm hỏng kết nối: camera chỉ không có topic.

Mỗi topic được so với bảng **topic chuẩn hoá** của MediaServer (`src/Onvif/OnvifEventTopics.cpp`), so theo đường dẫn
bỏ tiền tố namespace, không phân biệt hoa thường:

| `eventType` | Topic (bỏ tiền tố) | Trạng thái trong `Data` |
|---|---|---|
| `MOTION` | `RuleEngine/CellMotionDetector/Motion` | `IsMotion` |
| `MOTION` | `VideoSource/MotionAlarm` | `State` |
| `TAMPER` | `RuleEngine/TamperDetector/Tamper` | `IsTamper` |
| `TAMPER` | `VideoSource/GlobalSceneChange/ImagingService` | `State` |
| `VIDEO_LOSS` | `VideoSource/SignalLoss` | `State` |
| `DIGITAL_INPUT` | `Device/Trigger/DigitalInput` | `LogicalState` |
| `RELAY` | `Device/Trigger/Relay` | `LogicalState` |
| `LINE_CROSSING` | `RuleEngine/LineDetector/Crossed` | (không phải trạng thái) |
| `INTRUSION` | `RuleEngine/FieldDetector/ObjectsInside` | `IsInside` |

Nhận thêm topic = thêm dòng vào bảng đó. Topic không có trong bảng (topic riêng của hãng...) là `isSupported=false`.

### `info.txt`

`capabilities.onvifProfile.eventProfile` giữ **mọi** topic của camera, kể cả topic không hỗ trợ, để đối chiếu khi mở
rộng bảng:

```json
"eventProfile": {
  "isEventSupported": true,
  "isPullPointSupported": true,
  "topics": [
    { "topic": "tns1:RuleEngine/CellMotionDetector/Motion", "eventType": "MOTION", "isSupported": true,
      "isProperty": true, "source": ["VideoSourceConfigurationToken", "VideoAnalyticsConfigurationToken", "Rule"],
      "data": ["IsMotion"] },
    { "topic": "tnshik:RuleEngine/FaceDetection", "eventType": "", "isSupported": false,
      "isProperty": true, "source": ["Source"], "data": ["State"] }
  ]
}
```

`eventType`/`isSupported` được tính lại theo bảng khi đọc file, nên bản mới có bảng rộng hơn báo đúng cả camera chưa
kết nối lại.

### Webhook `media-device-info`

Khối mới `onvifEvents`, **chỉ** gồm topic được MediaServer hỗ trợ:

```json
"onvifEvents": {
  "isSupported": true,
  "enable": true,
  "eventTypes": ["MOTION", "TAMPER"],
  "topics": [
    { "eventType": "MOTION", "topic": "tns1:RuleEngine/CellMotionDetector/Motion" },
    { "eventType": "MOTION", "topic": "tns1:VideoSource/MotionAlarm" },
    { "eventType": "TAMPER", "topic": "tns1:RuleEngine/TamperDetector/Tamper" }
  ]
}
```

| Field | Ý nghĩa |
|---|---|
| `isSupported` | Camera đang điều khiển bằng ONVIF, có Event service và ít nhất một topic được hỗ trợ |
| `enable` | Nhận sự kiện theo cấu hình: `onvif.eventEnable` **và** `cameraAdvanceConfig.onvif.enableEvent` |
| `eventTypes` | Các `eventType` khác nhau, theo thứ tự bảng — client hiển thị theo field này |
| `topics` | Topic gốc của camera đã lọc, để tra cứu |

Camera ISAPI/PSIA/RTSP thuần: `isSupported=false`, mảng rỗng. Đổi `enableEvent` của camera gửi lại webhook. Đổi
`onvif.eventEnable` lúc chạy có hiệu lực ngay nhưng `enable` trong webhook chỉ cập nhật ở lần gửi kế tiếp.

### Bật/tắt theo camera

```json
{ "cameraAdvanceConfig": { "onvif": { "enableEvent": true } } }
```

Thiếu field = `true`. Đổi giá trị không làm controller kết nối lại. Option đi theo `info.txt` và đồng bộ ESC
(`enableOnvifEvent`), không cần migration.

### Nhận sự kiện

Camera có ít nhất một topic hỗ trợ, bật toàn cục và bật ở camera thì được đăng ký **PullPoint**:
`CreatePullPointSubscription` (kèm filter `TopicExpression` chỉ các topic hỗ trợ, `InitialTerminationTime`
= `onvif.eventTerminationSec`) rồi `PullMessages` liên tục (camera giữ tối đa `onvif.eventPullTimeoutSec`), `Renew`
trước khi hết hạn, `Unsubscribe` khi camera mất kết nối, bị tắt sự kiện, đổi giao thức hoặc bị xoá.

- Request HTTP bất đồng bộ trên poller của camera: không thread nào bị giữ khi chờ camera (300 camera ≈ 30
  request/s lúc yên lặng). Không dùng pool `device()` của PTZ.
- Sau khi mất mạng, tối đa `onvif.eventMaxConcurrentSubscribe` lệnh đăng ký chạy cùng lúc, mỗi camera chờ ngẫu
  nhiên tới `onvif.eventStartJitterMs` trước khi bắt đầu.
- Camera từ chối filter: đăng ký lại không filter, server tự bỏ topic không hỗ trợ.
- Camera không hỗ trợ `Renew`: tạo subscription mới khi cái cũ sắp hết.
- Subscription mất (camera reboot, `ResourceUnknown`): đăng ký lại ngay một lần, sau đó chờ 5 s nhân đôi tới
  `onvif.eventRetryMaxSec`.
- Camera trả 401/403/`NotAuthorized`: dừng, thử lại khi controller kết nối lại hoặc sau `onvif.eventRetryMaxSec`.
  Không thêm lần đăng nhập sai nào (camera Hikvision khoá IP).

Đợt này sự kiện chỉ được **ghi log**, qua broadcast `kBroadcastOnvifEvent` (đợt webhook sau gắn listener vào đây).
Sự kiện trạng thái chỉ phát khi giá trị đổi (khoá: `eventType` + các `SimpleItem` của `Source`); trạng thái ban đầu
ngay sau khi đăng ký (`Initialized`) ghi mức Debug. Camera có cả `CellMotionDetector/Motion` lẫn `VideoSource/MotionAlarm`
báo một lần chuyển động bằng hai topic với `Source` khác nhau (token cấu hình analytics và token nguồn video), nên log
cả hai dòng:

```
OnvifEvent <deviceId> 10.0.0.5 MOTION Changed tns1:RuleEngine/CellMotionDetector/Motion cam=2026-10-11T03:12:04Z lag=180ms src{VideoSourceConfigurationToken=VSC_1,Rule=R1} data{IsMotion=true}
OnvifEvent <deviceId> 10.0.0.5 subscribed http://10.0.0.5/onvif/Events/PullSubManager_1 filter=2 topics
OnvifEvent stats: pulling=287 backoff=9 suspended=1 events=1243 dropped_log=17 top=[...]
```

Mỗi camera ghi tối đa `onvif.eventLogPerCameraPerMin` dòng/phút, phần còn lại được đếm vào `dropped_log`.

Hạn chế: camera khoá ISAPI/PSIA không nhận sự kiện kể cả khi có ONVIF; sự kiện lúc mất kết nối bị mất (PullPoint
không có hàng đợi bền); topic chỉ đọc lại khi kết nối lại (`/media/mserver/device/reconnect`).

## Config toàn cục

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `camera.control_auto_order` | `onvif,isapi,psia` | Thứ tự thử khi `AUTO`. Camera đã khoá không bị ảnh hưởng khi đổi |
| `camera.control_reprobe_failures` | `5` | Số lần liên tiếp báo "không phải giao thức này" trước khi camera đã khoá dò lại |
| `camera.control_auth_retry_sec` | `300` | Khoảng cách giữa hai lần thử khi camera từ chối tài khoản (thay cho 60 giây) |
| `camera.prepareOnvif` | `1` | Plugin Hikvision/Hanwha bật ONVIF và tạo user ONVIF trước các giao thức; `0` = không sửa cấu hình camera |
| `camera.onvifUsername` | `onvif_vms` | User ONVIF plugin Hikvision tạo, mật khẩu là mật khẩu cấu hình của camera |
| `speaker.control_auto_order` | `onvif,isapi,psia` | Như trên, cho loa |
| `speaker.control_reprobe_failures` | `5` | Như trên, cho loa |
| `speaker.control_auth_retry_sec` | `300` | Như trên, cho loa |
| `speaker.detect_max_interval_sec` | `600` | Loa có `manufacturer` rỗng: mỗi chu kỳ thử API hãng (Bosch) rồi các giao thức, nhận ra hãng thì ghi đè `manufacturer` và báo qua webhook `media-device-info`. Khi không gì trả lời, chu kỳ nhân đôi từ 60 s tới giá trị này (0 = giữ 60 s) |
| `psia.request_timeout_ms` | `5000` | Timeout mỗi request PSIA |
| `isapi.request_timeout_ms` | `5000` | Timeout mỗi request ISAPI |
| `onvif.eventEnable` | `1` | `0` tắt nhận sự kiện ONVIF mọi camera (topic vẫn được đọc khi kết nối) |
| `onvif.eventPullTimeoutSec` | `10` | `Timeout` của `PullMessages` |
| `onvif.eventPullGraceSec` | `5` | Cộng vào timeout HTTP của một pull |
| `onvif.eventMessageLimit` | `64` | `MessageLimit` của `PullMessages` |
| `onvif.eventTerminationSec` | `60` | Thời hạn subscription, gia hạn bằng `Renew` |
| `onvif.eventRenewMarginSec` | `10` | Gia hạn trước khi hết hạn (cộng thêm một pull) |
| `onvif.eventRetryMaxSec` | `300` | Trần thời gian chờ giữa các lần thử lỗi, và thời gian chờ sau khi bị từ chối tài khoản |
| `onvif.eventMaxConcurrentSubscribe` | `16` | Số `CreatePullPointSubscription` chạy cùng lúc trên node |
| `onvif.eventStartJitterMs` | `5000` | Chờ ngẫu nhiên tối đa chừng này trước khi đăng ký |
| `onvif.eventSubscribeFilter` | `1` | `0` không gửi filter topic cho camera |
| `onvif.eventMaxTopics` | `512` | Số topic tối đa giữ từ `GetEventProperties` |
| `onvif.eventLogPerCameraPerMin` | `60` | Số dòng log sự kiện tối đa mỗi camera mỗi phút |
| `onvif.eventStatsIntervalSec` | `60` | Chu kỳ dòng log thống kê; `0` tắt |

## Kiểm thử

- Unit test: `unit.camera_controller_all` (`CameraProtocolTest`, `ProtocolLockTest`, `SpeakerProtocolTest`),
  `unit.psia_control_all` (`PsiaParseTest`, `IsapiParseTest`, ...; `IsapiMockTest.TwoWayAudioSendsG711AndCloses` chạy
  với `psia_mock.py --dialect isapi --state PATH` khi đặt `ISAPI_MOCK_ADDR` và `ISAPI_MOCK_STATE`;
  `RtspBackchannelMockTest` chạy với `rtsp_backchannel_mock.py --state PATH` khi đặt `RTSP_BC_MOCK_ADDR` và
  `RTSP_BC_MOCK_STATE`).
- Camera PSIA giả: `tests/integration/stress_vms/psia_mock.py --port 18680` (Digest, `admin`/`admin123`;
  `--basic` cho Basic). Camera ISAPI giả: thêm `--dialect isapi --rtsp-port 8554`; `--onvif-401` cho
  `/onvif/device_service` trả 401 như Hikvision tắt ONVIF; `--onvif-off` cho Integrate báo ONVIF tắt và chưa có
  user ONVIF (plugin Hikvision chuẩn bị ONVIF).
- Plugin camera: `unit.vendor_stage_all`; `VendorCameraMockTest.*` chạy với `psia_mock.py --dialect isapi
  --onvif-off` (`ISAPI_MOCK_ADDR`) và `sunapi_mock.py --onvif-off` (`SUNAPI_MOCK_ADDR`).
- Sự kiện ONVIF: `unit.OnvifEventTopicsTest.*`, `unit.OnvifEventTest.*` (binary `unit_xml`: bảng chuẩn hoá,
  parser TopicSet/PullMessages), `unit.onvif_event_subscriber_all` (state machine với transport giả). Camera giả:
  `onvif_mock.py --events --motion-period-sec 20`; fault spec `events_filter_refuse`, `events_no_renew`,
  `events_epoch` (đổi = camera reboot), `events_401`; `--state` ghi thống kê `events` từng thiết bị.
- `PSIA_MOCK_ADDR=127.0.0.1:18680 ISAPI_MOCK_ADDR=127.0.0.1:18681 release/linux/Debug/unit_psia_control` chạy cả
  các case gọi mock.
