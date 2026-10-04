# Mã lỗi điều khiển thiết bị: phân biệt lỗi mạng và lỗi xác thực

Áp dụng từ 2026-10-04. Dành cho FE hiển thị đúng nguyên nhân khi thao tác ONVIF với camera thất bại.

## API áp dụng

| API | Ghi chú |
|---|---|
| `/media/mserver/device/ptz_control` | PTZ (absolute/relative/continuous, home) |
| `/media/mserver/device/ptz_control/goto_preset` | ONVIF preset và user preset |
| `/media/mserver/device/ptz_control/set_preset` | đọc vị trí hiện tại (GetStatus) để lưu preset |
| `/media/mserver/device/imageMoveControl` | focus / iris |
| `/media/mserver/device/relayOutputControl` | relay output |
| `/media/mserver/device/onvifSetVideoConfigs`, `onvifGetConfigProfiles`, `onvifApplyVideoConfigRule` | cấu hình encoder, kết quả từng camera |

`ptz_control/get_presets` và `remove_preset` không gọi camera nên không trả các mã này.

## Mã lỗi mới

| `code` | HTTP | Nguyên nhân | Gợi ý hiển thị |
|---|---|---|---|
| `905004` | 403 | Camera từ chối tài khoản/mật khẩu ONVIF (HTTP 401/403, hoặc SOAP fault NotAuthorized/FailedAuthentication). Cũng gặp khi giờ camera lệch quá xa nên chữ ký WS-Security hết hạn | "Sai tài khoản hoặc mật khẩu camera. Kiểm tra lại thông tin đăng nhập của camera." |
| `905005` | 502 | Không mở được kết nối tới camera: bị từ chối, không có route, quá thời gian kết nối | "Không kết nối được tới camera. Kiểm tra mạng/địa chỉ IP/cổng ONVIF." |
| `905006` | 504 | Đã kết nối nhưng camera không trả lời trong 10 s hoặc ngắt kết nối giữa chừng | "Camera không phản hồi. Thử lại sau." |
| `905007` | 403 | Camera chỉ chấp nhận HTTP Basic, server đang tắt (`onvif.allowHttpBasicAuth=0`) vì Basic gửi mật khẩu dạng rõ | "Camera chỉ hỗ trợ xác thực Basic. Đổi camera sang Digest hoặc liên hệ quản trị." |

HTTP status chỉ để tham khảo: FE phân nhánh theo `code`. Mã xác thực của camera dùng 403, **không** dùng 401, để FE
không nhầm với token người dùng hết hạn (`901001`).

## Quy tắc trả mã

1. **Thao tác gửi tới camera thất bại** (camera đang kết nối): nếu nguyên nhân là mạng hoặc xác thực thì trả
   `905004`–`905007`. Nguyên nhân khác (camera trả SOAP fault, thao tác không hỗ trợ…) giữ mã riêng của thao tác như
   trước: `907004` PTZ absolute, `907005` continuous, `907006` relative, `907009` home, `907010` get status,
   `907011`/`907012` goto preset, `912005` focus, `912007` iris, `912009` relay, `912002`/`912003` set/get config.
2. **Camera chưa kết nối được** (trước đây luôn là `905002 Device offline`): nếu lần kết nối ONVIF gần nhất thất bại
   vì mạng hoặc xác thực thì trả `905004`–`905007`, `msg` có dạng `Device controller is not ready: <nguyên nhân>`.
   Không biết nguyên nhân (camera chưa từng kết nối, đang dừng, lỗi khác) thì vẫn `905002`.
3. Camera tự kết nối lại mỗi 60 s. Sau khi người dùng sửa mật khẩu camera, lỗi `905004` hết sau lần kết nối lại kế tiếp.

## Gợi ý xử lý ở FE

```js
switch (res.code) {
  case 905004: showError('Sai tài khoản hoặc mật khẩu camera'); break;
  case 905007: showError('Camera chỉ hỗ trợ xác thực Basic (đang bị tắt)'); break;
  case 905005: showError('Không kết nối được tới camera'); break;
  case 905006: showError('Camera không phản hồi, thử lại sau'); break;
  case 905002: showError('Camera đang offline'); break;
  default:     showError(res.msg);
}
```
