# Mã lỗi điều khiển thiết bị: phân biệt lỗi mạng và lỗi xác thực

Áp dụng từ 2026-10-04 (905008 thêm sau, cùng ngày). Dành cho FE hiển thị đúng nguyên nhân khi thao tác ONVIF với camera thất bại.

## API áp dụng

| API | Ghi chú |
|---|---|
| `/media/mserver/device/ptz_control` | PTZ (absolute/relative/continuous, home) |
| `/media/mserver/device/ptz_control/goto_preset` | ONVIF preset và user preset |
| `/media/mserver/device/ptz_control/set_preset` | đọc vị trí hiện tại (GetStatus) để lưu preset |
| `/media/mserver/device/imageMoveControl` | focus / iris |
| `/media/mserver/device/relayOutputControl` | relay output |
| `/media/mserver/device/onvifSetVideoConfigs`, `onvifGetConfigProfiles`, `onvifApplyVideoConfigRule` | cấu hình encoder, kết quả từng camera |
| `/media/mserver/device/setAudioVolume` | loa: chưa kết nối được (mục "Loa"), hoặc API hãng (Bosch) từ chối tài khoản/không kết nối được |
| `/media/mserver/device/reconnect` | camera và loa: `data.code` là nguyên nhân lần thử vừa hỏng (mục "API kết nối lại") |

`ptz_control/get_presets` và `remove_preset` không gọi camera nên không trả các mã này.

## Mã lỗi mới

| `code` | HTTP | Nguyên nhân | Gợi ý hiển thị |
|---|---|---|---|
| `905004` | 403 | Camera từ chối tài khoản/mật khẩu ONVIF (HTTP 401/403, hoặc SOAP fault NotAuthorized/FailedAuthentication). Lệch giờ từ 30 s trở lên thì báo `905008` thay vì mã này | "Sai tài khoản hoặc mật khẩu camera. Kiểm tra lại thông tin đăng nhập của camera." |
| `905005` | 502 | Không mở được kết nối tới camera: bị từ chối, không có route, quá thời gian kết nối | "Không kết nối được tới camera. Kiểm tra mạng/địa chỉ IP/cổng ONVIF." |
| `905006` | 504 | Đã kết nối nhưng camera không trả lời trong 10 s hoặc ngắt kết nối giữa chừng | "Camera không phản hồi. Thử lại sau." |
| `905007` | 403 | Camera chỉ chấp nhận HTTP Basic, server đang tắt (`onvif.allowHttpBasicAuth=0`) vì Basic gửi mật khẩu dạng rõ | "Camera chỉ hỗ trợ xác thực Basic. Đổi camera sang Digest hoặc liên hệ quản trị." |
| `905008` | 403 | Camera từ chối chữ ký WS-Security và **đồng hồ camera lệch** server từ `onvif.clockSkewThresholdSec` (30 s) trở lên, hoặc camera báo `MessageExpired`. Kèm `data` (bên dưới) | "Giờ camera lệch {clockOffsetSec} giây so với server. Hệ thống sẽ tự đồng bộ giờ trong chu kỳ kiểm tra kế tiếp (≤ 1 phút)." |

Camera điều khiển bằng ISAPI/PSIA (xem [camera-control-protocol.md](camera-control-protocol.md)) cũng trả
`905004` (HTTP 401/403), `905005` (không kết nối được), `905006` (kết nối rồi nhưng không trả lời); `905007` và
`905008` chỉ có với ONVIF. Camera AUTO chưa dò được giao thức nào trả nguyên nhân của lượt dò gần nhất.
Loa áp dụng y hệt (mục "Loa").

HTTP status chỉ để tham khảo: FE phân nhánh theo `code`. Mã xác thực của camera dùng 403, **không** dùng 401, để FE
không nhầm với token người dùng hết hạn (`901001`).

### `data` của `905008`

```json
{
  "code": 905008,
  "msg": "Device execute ptz absolute move failed: ...",
  "data": {
    "cameraTime": "2026-10-04T08:34:41Z",
    "serverTime": "2026-10-04T08:24:41Z",
    "clockOffsetSec": 600,
    "source": "GetSystemDateAndTime"
  }
}
```

- `clockOffsetSec` = giờ camera − giờ server (dương: camera chạy nhanh), theo cách server đồng bộ giờ camera
  (giờ hiển thị trên camera bằng giờ server). `cameraTime` = `serverTime` + `clockOffsetSec`.
- `source`: `GetSystemDateAndTime`, hoặc `HttpDate` khi camera không cho đọc giờ và server lấy từ header `Date`.
- `data` có thể không có (không đo được giờ camera) — FE vẫn hiển thị theo `code`.

### Lệch giờ: server tự xử lý thế nào

1. Lệnh bị camera từ chối chữ ký → server **đọc** giờ camera (`GetSystemDateAndTime`, không cần xác thực; không
   được thì lấy header `Date` của chính lần bị từ chối). Giờ đổi so với lúc ký từ 5 s trở lên thì ký lại và gửi lại
   **một lần** — thường lệnh thành công luôn, FE không thấy lỗi.
2. Ký lại vẫn bị từ chối, hoặc giờ không đổi: lệch ≥ 30 s → `905008`, ngược lại → `905004`.
3. API **không bao giờ đặt giờ camera**. Chỉ chu kỳ kiểm tra kết nối của camera (60 s) đặt giờ: chu kỳ kế tiếp sau
   khi một lệnh phát hiện lệch ≥ 10 s, mỗi `onvif.clockSyncIntervalSec` (3600 s) khi đang kết nối, và ngay khi kết nối
   thất bại vì lệch giờ (`905008`).
4. Tắt đồng bộ giờ: toàn server `onvif.clockSyncEnable=0`, hoặc từng camera
   `cameraAdvanceConfig.onvif.enableTimeSync=false` (mặc định `true`). Khi tắt, lỗi vẫn báo `905008` nhưng server
   không sửa giờ — người dùng phải chỉnh giờ trên camera.

## Quy tắc trả mã

1. **Thao tác gửi tới camera thất bại** (camera đang kết nối): nếu nguyên nhân là mạng hoặc xác thực thì trả
   `905004`–`905008`. Không xác định được nguyên nhân (camera trả SOAP fault…) thì trả `-1` (`CODE_OTHER_EXCEPTION`),
   chi tiết ở `msg`. Từ 2026-10-05 không còn trả các mã chung theo thao tác `907004`–`907006`, `907009`–`907012`,
   `912002`/`912003`, `912005`, `912007`, `912009`. Camera không có relay output nào trả `912008`.
2. **Camera chưa kết nối được** (trước đây luôn là `905002 Device offline`): nếu lần kết nối ONVIF gần nhất thất bại
   vì mạng hoặc xác thực thì trả `905004`–`905008`, `msg` có dạng `Device controller is not ready: <nguyên nhân>`.
   Không biết nguyên nhân (camera chưa từng kết nối, đang dừng, lỗi khác) thì vẫn `905002`.
3. Camera tự kết nối lại mỗi 60 s. Sau khi người dùng sửa mật khẩu camera, lỗi `905004` hết sau lần kết nối lại kế tiếp.

## Loa

Áp dụng từ 2026-10-04.

- `setAudioVolume` khi loa chưa kết nối được: `905004`–`905008` theo nguyên nhân lần kết nối/lượt dò gần nhất, như
  quy tắc 2 ở trên (trước đây luôn `905002`). `905008` có `data` giờ thiết bị như camera.
- `setAudioVolume` qua API hãng (Bosch) thất bại vì API hãng trả 401/403 → `905004`; không kết nối được → `905005`;
  kết nối rồi không trả lời → `905006`; lỗi khác trả `-1`. Loa không hỗ trợ chỉnh âm lượng trả `902005`, âm lượng
  yêu cầu trùng âm lượng hiện tại trả `912001` (trước 2026-10-05 các trường hợp này đều là `915003`).
- `/device/speaker/playAudioFile` **không** trả các mã này: API trả 200 ngay cho cả danh sách loa, kết quả từng loa
  chỉ ghi log. Lỗi của backchannel (RTSP/ISAPI two-way audio) cũng chỉ ghi log.
- Loa được đồng bộ giờ như camera: khi kết nối, mỗi `onvif.clockSyncIntervalSec`, và ngay khi kết nối thất bại vì
  lệch giờ. Tắt toàn server `onvif.clockSyncEnable=0`, hoặc từng loa `speakerAdvanceConfig.onvif.enableTimeSync=false`
  (mặc định `true`). Loa Bosch đặt giờ bằng tài khoản ONVIF do plugin tạo (nhóm `ONVIF_OPERATOR`); thiết bị có thể
  không cho nhóm này đặt giờ — khi đó lệnh đặt giờ chỉ ghi log, người dùng chỉnh giờ trên loa.

## Gợi ý xử lý ở FE

```js
switch (res.code) {
  case 905004: showError('Sai tài khoản hoặc mật khẩu camera'); break;
  case 905007: showError('Camera chỉ hỗ trợ xác thực Basic (đang bị tắt)'); break;
  case 905005: showError('Không kết nối được tới camera'); break;
  case 905006: showError('Camera không phản hồi, thử lại sau'); break;
  case 905008: showError(`Giờ camera lệch ${res.data?.clockOffsetSec ?? '?'} giây so với server`); break;
  case 905002: showError('Camera đang offline'); break;
  default:     showError(res.msg);
}
```
