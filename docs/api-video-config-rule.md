# Rule cấu hình video có thời hạn

Service khác (không phải người dùng) đặt cấu hình encoder tạm thời lên một loạt camera ONVIF. Hết
thời hạn thì MediaServer tự trả camera về cấu hình gốc (baseline). Kết quả áp của từng camera được
gửi qua webhook, đồng thời lưu vào SQLite của MediaServer để tra cứu lại theo `logId`.

## Xác thực

Cả hai endpoint dành cho giao tiếp service-to-service, không dùng JWT và không kiểm tra quyền
người dùng hay quyền trên camera:

- khi `manager.enableAuthorize=1`, request phải có `X-Secret-Key` khớp `manager.apiSecret`. Nên gửi
  ở header, nhưng MediaServer cũng nhận nếu nằm ở query string hoặc body;
- khi `manager.enableAuthorize=0`, bỏ qua kiểm tra;
- nếu `manager.apiSecret` rỗng thì chỉ cần có header, không so giá trị. Vì vậy môi trường triển
  khai phải cấu hình secret khác rỗng;
- thiếu hoặc sai secret → HTTP 401, `code=901001`.

## Áp rule

```http
POST /media/mserver/device/onvifApplyVideoConfigRule
X-Secret-Key: <manager.apiSecret>
Content-Type: application/json
```

```json
{
  "logId": "7f0c2a4e-rule-001",
  "cameras": [
    { "deviceId": "cam-001", "width": 1280, "height": 720, "fps": 15, "bitrate": 1024, "applyDuration": 600 }
  ]
}
```

| Field | Kiểu | Ràng buộc |
|---|---|---|
| `logId` | string | Bắt buộc; khoá tra cứu kết quả, và là `logId` trong webhook |
| `cameras` | mảng | Bắt buộc, không rỗng; mỗi `deviceId` chỉ xuất hiện một lần |
| `width`, `height`, `bitrate` | số nguyên | `> 0`; `bitrate` tính bằng kbps |
| `fps` | số | `> 0` |
| `applyDuration` | số nguyên | `> 0`, tính bằng giây |

Kết quả xử lý được trả riêng cho từng camera, và HTTP luôn là 200 khi request hợp lệ:

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "results": [
      { "deviceId": "cam-001", "code": 0, "msg": "accepted" },
      { "deviceId": "cam-404", "code": 905001, "msg": "Device not found" }
    ]
  }
}
```

Chỉ camera có `code=0` (`accepted`) mới có kết quả sau đó, qua webhook và qua API tra cứu. Camera bị
từ chối ngay (`902001` sai tham số hoặc `deviceId` trùng, `905001` không tìm thấy, `905003` đang bị
người khác điều khiển) thì không có dòng kết quả nào.

Hành vi khi camera đang có rule:

- gửi lại **cùng cấu hình** trước khi hết hạn thì coi là gia hạn: thời hạn lấy mốc muộn hơn, job
  mang `logId` mới, và cả `logId` cũ lẫn mới đều nhận kết quả;
- gửi **cấu hình khác** thì request mới xếp hàng chờ job đang chạy hết hạn. Hàng chờ chỉ có một chỗ,
  request đang chờ trước đó nhận `RULE_SUPERSEDED`.

Camera dừng, restart hoặc bị xoá:

- **MediaServer restart** trong lúc rule chạy: job được khôi phục khi controller ONVIF của camera
  kết nối lại. Rule còn hạn thì được áp tiếp, và kết quả đã báo trước khi restart không bị báo lại.
  Rule hết hạn trong lúc server không chạy thì camera được trả về baseline; nếu chưa có kết quả thì
  báo `RULE_EXPIRED`;
- **camera bị tắt (disable) hoặc dừng**: job giữ nguyên trong DB và được khôi phục khi camera chạy
  lại. Kết quả vẫn ở `PENDING` trong lúc đó;
- **camera bị xoá khỏi node** (kể cả khi chuyển sang node khác): job bị xoá, không rollback; mọi
  request của camera còn `PENDING` nhận `DEVICE_REMOVED`.

Người dùng đổi cấu hình qua `onvifSetVideoConfigs` trong lúc rule đang giữ profile đó:

- cấu hình **không** được gửi tới camera ngay (nếu gửi thì mỗi chu kỳ đồng bộ sẽ đẩy rule đè lại, làm
  encoder khởi động lại liên tục). Cấu hình chỉ được lưu làm baseline, nên sống sót qua restart;
- hết hạn rule thì bước rollback đẩy cấu hình mới này lên camera. Nếu có request đang xếp hàng
  thì nó được áp trước, rồi mới tới rollback;
- API vẫn trả thành công (`code=0`, `data.flag=true`, `data.config` như cũ), kèm
  `data.deferred=true` và `data.deferredUntil`. `deferredUntil` là thời điểm dự kiến tính bằng ms:
  hạn của request đang xếp hàng nếu có, không thì hạn của rule đang chạy. Đây chỉ là ước lượng vì
  rule còn có thể được gia hạn. Khi không bị hoãn thì `data.deferred=false`;
- gửi đúng cấu hình đang là baseline thì vẫn trả `912001` (không đổi) như trước.

## Tra cứu kết quả

```http
GET /media/mserver/device/onvifGetVideoConfigRuleResult?logId=7f0c2a4e-rule-001
X-Secret-Key: <manager.apiSecret>
```

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "results": [
      {
        "logId": "7f0c2a4e-rule-001",
        "deviceId": "cam-001",
        "status": "SUCCESS",
        "errorCode": null,
        "errorMessage": null,
        "createdAt": 1790000000000,
        "updatedAt": 1790000061234
      }
    ]
  }
}
```

- `status`: `PENDING` (đã nhận, chưa có kết quả), `SUCCESS` hoặc `FAILED`;
- `errorCode`, `errorMessage` chỉ có giá trị khi `FAILED`, còn lại là `null`;
- `createdAt`: lúc camera được nhận; `updatedAt`: lần đổi trạng thái cuối (ms);
- sắp xếp theo `deviceId`. Nếu `logId` không có hoặc đã bị dọn thì trả mảng rỗng, không báo lỗi;
- lần tra cứu gửi sau khi `onvifApplyVideoConfigRule` đã trả về luôn thấy các dòng `PENDING` của
  request đó (đọc và ghi đi chung một hàng đợi tuần tự);
- kết quả chỉ lưu trên node nhận request, nên phải tra cứu đúng node đó;
- dòng kết quả bị xoá sau `video_config_rule.result_keep_hours` giờ (mặc định 168) kể từ lần đổi
  trạng thái cuối. Giá trị `<= 0` thì giữ vĩnh viễn.

## Webhook kết quả

Địa chỉ là `hook.api_url` + `hook.on_video_config_rule_result`, mặc định
`/api/media-server/action-callback`. Để trống thì không gửi. Webhook dùng `POST` JSON, retry theo
`hook.retry`/`hook.retry_delay`, và có header `Idempotency-Key: <logId>|<deviceId>` để phía nhận bỏ
bản lặp khi retry.

```json
{
  "logId": "7f0c2a4e-rule-001",
  "deviceId": "cam-001",
  "status": "FAILED",
  "errorCode": "CONFIG_NOT_APPLIED",
  "errorMessage": "...",
  "mediaServerId": "...",
  "hook_index": 42
}
```

Body giữ đúng hợp đồng của bản fork recording-encryption: `status` là `SUCCESS`/`FAILED`, còn
`errorCode`/`errorMessage` là `null` khi thành công. `mediaServerId` và `hook_index` do cơ chế hook
chung thêm vào.

Mỗi cặp (`logId`, `deviceId`) đã được nhận sẽ có đúng một kết quả. Kết quả được báo khi camera
**xác nhận** đã áp cấu hình (đọc lại profile, thường trong khoảng một chu kỳ đồng bộ ~60 s), chứ
không phải ngay khi lệnh set trả về.

## Mã lỗi

| `errorCode` | Khi nào |
|---|---|
| `DEVICE_OFFLINE` | Camera ngắt kết nối giữa lúc được nhận và lúc xử lý |
| `NOT_A_CAMERA` | `deviceId` không phải camera (ví dụ loa) |
| `PROFILE_TOKEN_NOT_FOUND` | Không xác định được profile chính của camera |
| `GET_CONFIG_FAILED` | Không đọc được cấu hình gốc của camera |
| `SET_CONFIG_FAILED` | Camera trả lỗi SOAP khi đặt cấu hình trong lần đồng bộ |
| `CONFIG_NOT_APPLIED` | Camera nhận lệnh nhưng vẫn giữ cấu hình cũ sau các lần thử lại |
| `CONFIG_NOT_CONFIRMED` | Hết hạn khi controller vẫn kết nối nhưng chưa xác nhận được cấu hình |
| `CONTROLLER_OFFLINE` | Hết hạn khi controller ONVIF không kết nối được |
| `RULE_EXPIRED` | Hết hạn trước khi kịp áp (đang đọc cấu hình gốc, đang xếp hàng, hoặc lúc MediaServer không chạy) |
| `RULE_SUPERSEDED` | Request đang xếp hàng bị một request cấu hình khác thay thế |
| `DEVICE_REMOVED` | Camera bị xoá khỏi node trước khi có kết quả |

## Hạn chế đã biết

Camera bật `keepConfigProfileAndStream`, và MediaServer restart trong lúc rule chạy: lần đồng bộ
profile đầu tiên sau khi kết nối lại có thể chạy trước khi job được khôi phục. Khi đó cấu hình rule
đang có trên camera bị nhận làm baseline (chế độ này coi camera là nguồn đúng), nên hết hạn thì
camera vẫn giữ cấu hình rule. Tình huống tương tự xảy ra nếu camera offline lúc server restart rồi
mới kết nối lại. Muốn sửa triệt để thì phải lưu baseline kèm job, tức đổi schema bằng migration
mới. Chưa làm.

## Cấu hình

```ini
[hook]
api_url=
on_video_config_rule_result=/api/media-server/action-callback

[video_config_rule]
result_keep_hours=168
```
