# Kịch bản integration transcode khi xem với camera thật

Kịch bản này xác minh xuyên suốt tính năng `enableReduceQualityOnView` với một camera thật có hai
stream. Đây là test thủ công có kiểm soát vì cần camera, API service và player/`ffprobe`; không chạy
trong `ctest` mặc định.

## Phạm vi

- MediaServer nhận đúng metadata của hai stream camera.
- API service nhận capability giảm chất lượng qua
  `POST /api/media-server/media-device-info`.
- Các profile được đề xuất có thể phát thật và tạo đúng codec, độ phân giải, bitrate, FPS.
- Request ngoài danh sách đề xuất bị từ chối.
- Tắt option chặn transcode giảm chất lượng nhưng không chặn watermark/privacy mask.
- Capability được cập nhật sau thay đổi option, thay đổi metadata, reconnect và restart.

## Điều kiện chạy

- Build có `ENABLE_FFMPEG=ON` và encoder H264; nếu kiểm tra H265 thì build phải có encoder H265.
- Một camera thật có hai stream đang hoạt động, khuyến nghị:
  - `main`: 1920x1080 hoặc cao hơn, 20–30 FPS;
  - `sub`: 1280x720 hoặc thấp hơn, 10–20 FPS.
- Camera đã được khai báo trên API service và cả hai stream xem được khi chưa transcode.
- MediaServer trỏ `hook.api_url` tới API service thật. API service phải lưu được request gửi tới
  `/api/media-server/media-device-info` để đối chiếu payload.
- Có `curl`, `jq` và `ffprobe`. Có JWT được phép xem camera nếu hệ thống bật authorization.
- Đồng hồ MediaServer và API service đã đồng bộ.

Không ghi JWT, mật khẩu camera hoặc payload chứa bí mật vào report. Các ví dụ dưới đây dùng biến
môi trường cục bộ:

```bash
export MEDIA_BASE_URL='https://media.example.com'
export CAMERA_ID='camera-001'
export STREAM_ID='main'
export VIEW_TOKEN='<JWT>'
export SESSION_ID="real-camera-$(date +%s)"
```

## Chuẩn bị và baseline

1. Trên API service, cấu hình đủ `main` và `sub`, đặt `enableReduceQualityOnView=false`, tắt
   watermark/privacy mask.
2. Khởi động MediaServer và chờ cả hai stream online.
3. Xác nhận từng stream gốc phát được ít nhất 30 giây, không có decode error:

   ```bash
   ffprobe -v error -show_entries stream=index,codec_name,width,height,avg_frame_rate,bit_rate \
     -of json "${MEDIA_BASE_URL}/media/live/${CAMERA_ID}/main/hls.m3u8?token=${VIEW_TOKEN}&session_id=${SESSION_ID}-main"
   ffprobe -v error -show_entries stream=index,codec_name,width,height,avg_frame_rate,bit_rate \
     -of json "${MEDIA_BASE_URL}/media/live/${CAMERA_ID}/sub/hls.m3u8?token=${VIEW_TOKEN}&session_id=${SESSION_ID}-sub"
   ```

4. Lưu metadata nguồn và CPU/RAM baseline của MediaServer vào report.

## Ma trận test

| ID | Trạng thái | Thao tác | Kết quả bắt buộc |
|---|---|---|---|
| RC-01 | Option tắt | Chờ hook capability | `supported=false`, `profiles=[]` |
| RC-02 | Option bật, hai stream online | Chờ hook mới | Có profile hợp lệ từ metadata hai stream |
| RC-03 | Option bật | Phát từng profile được đề xuất | HTTP thành công; codec/resolution/FPS đúng profile |
| RC-04 | Option bật | Thay đổi một giá trị profile | HTTP 400, không tạo derived stream |
| RC-05 | Option tắt | Gửi lại profile hợp lệ cũ | HTTP 403 |
| RC-06 | Option tắt, bật watermark | Phát luồng không yêu cầu giảm chất lượng | Luồng overlay phát được |
| RC-07 | Option tắt, bật privacy mask | Phát luồng không yêu cầu giảm chất lượng | Luồng overlay phát được |
| RC-08 | Option bật và overlay bật | Yêu cầu profile hợp lệ | Luồng quality-overlay phát được |
| RC-09 | Option bật | Đổi metadata/reconnect camera | Capability mới được gửi, không còn profile lỗi thời |
| RC-10 | Option bật | Restart MediaServer | Option được phục hồi; capability và playback vẫn đúng |

## Các bước chi tiết

### RC-01 — option tắt

Đặt `enableReduceQualityOnView=false` trên API service và chờ MediaServer nhận cấu hình. Payload mới
nhất của `media-device-info` cho đúng `deviceId` phải chứa:

```json
{
  "reduceQualityOnView": {
    "supported": false,
    "profiles": []
  }
}
```

Nếu API service gộp hoặc lưu payload, phải kiểm tra bản ghi có timestamp sau thời điểm đổi option.

### RC-02 — kiểm tra danh sách đề xuất

Đặt `enableReduceQualityOnView=true`. Chờ tối đa hai chu kỳ reconnect/health-check của camera để cả
hai stream có metadata. Lấy payload hook mới nhất và lưu phần sau vào `profiles.json`:

```bash
jq '.reduceQualityOnView' media-device-info.json
jq -e '.reduceQualityOnView.supported == true and
       (.reduceQualityOnView.profiles | length > 0)' media-device-info.json
```

Ghi lại FPS mà MediaServer báo tại thời điểm phát capability. Nếu FPS nguồn đang bằng `0` hoặc chưa
xác định, capability vẫn phải có profile và FPS profile dùng mức tương ứng trong quality ladder.
Không cần reconnect chỉ để nhận profile ban đầu. Nếu FPS runtime sau đó trở thành hợp lệ và làm thay
đổi danh sách profile, API service phải nhận capability mới; payload giống hệt không được gửi lặp.

Với từng profile, xác minh:

- `sourceStreamId` là ID của `main` hoặc `sub` đã cấu hình;
- `codec` chỉ là `H264` hoặc `H265`;
- `width`, `height`, `bitrate`, `fps` đều lớn hơn 0;
- width và height là số chẵn;
- không profile nào upscale so với source stream tương ứng;
- bitrate và FPS không vượt metadata source;
- không có profile trùng toàn bộ năm thông số đầu ra và source;
- thứ tự profile ổn định từ độ phân giải cao xuống thấp.

### RC-03 — phát từng profile đề xuất

Với mỗi phần tử trong `profiles`, đặt `STREAM_ID`, `CODEC`, `WIDTH`, `HEIGHT`, `BITRATE`, `FPS` theo
payload rồi gọi:

```bash
TRANSCODE_URL="${MEDIA_BASE_URL}/media/live/${CAMERA_ID}/${STREAM_ID}/hls.m3u8"
TRANSCODE_URL="${TRANSCODE_URL}?token=${VIEW_TOKEN}&session_id=${SESSION_ID}"
TRANSCODE_URL="${TRANSCODE_URL}&transcode=true&target_vcodec=${CODEC}"
TRANSCODE_URL="${TRANSCODE_URL}&target_width=${WIDTH}&target_height=${HEIGHT}"
TRANSCODE_URL="${TRANSCODE_URL}&target_bitrate=${BITRATE}&target_fps=${FPS}"

curl --fail --location --silent --show-error --max-time 20 "$TRANSCODE_URL" -o transcode.m3u8
ffprobe -v error -read_intervals '%+15' \
  -show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate \
  -of json "$TRANSCODE_URL"
```

Kết quả bắt buộc:

- redirect, nếu có, trỏ tới derived stream chứa `.transcode.quality.`;
- playlist và segment tải được liên tục ít nhất 30 giây;
- codec, width và height khớp tuyệt đối profile;
- FPS quan sát phù hợp profile, chấp nhận sai số do camera/VFR không quá 10%;
- bitrate đo trong cửa sổ 30 giây không vượt target quá 20%;
- âm thanh, nếu source có, vẫn nghe được và timestamp không trôi rõ rệt;
- dừng player thì derived stream tự giải phóng theo timeout cấu hình.

### RC-04 — từ chối profile không được công bố

Điều kiện của case này là `protocol.allow_unadvertised_transcode_profile=0` (mặc định). Khi đặt biến
 thành `1`, cùng request ngoài danh sách phải được phép đi tiếp nếu camera bật
 `enableReduceQualityOnView`; các giới hạn parser và resource guard vẫn áp dụng.


Giữ nguyên một profile hợp lệ nhưng tăng `target_bitrate` thêm 1. Request phải trả HTTP 400:

```bash
curl --silent --output invalid-profile.body --write-out '%{http_code}\n' \
  "${MEDIA_BASE_URL}/media/live/${CAMERA_ID}/${STREAM_ID}/hls.m3u8?token=${VIEW_TOKEN}&session_id=${SESSION_ID}-invalid&transcode=true&target_vcodec=${CODEC}&target_width=${WIDTH}&target_height=${HEIGHT}&target_bitrate=$((BITRATE + 1))&target_fps=${FPS}"
```

Body phải chỉ ra profile giảm chất lượng không được hỗ trợ. Không được xuất hiện derived stream mới
cho request này.

### RC-05 — tắt option khi profile từng hợp lệ

Đặt lại `enableReduceQualityOnView=false`, chờ hook báo `supported=false`, rồi gửi lại URL hợp lệ ở
RC-03 với `session_id` mới. Request phải trả HTTP 403 và không khởi tạo encoder mới. Luồng gốc vẫn
phát bình thường. Body phải là `Reduce quality on view is disabled`. Gửi thêm một request với token
không hợp lệ để xác nhận lỗi xác thực thực sự vẫn trả HTTP 401.

### RC-06/RC-07 — ngoại lệ watermark và privacy mask

Giữ option giảm chất lượng ở trạng thái tắt:

1. Bật watermark bắt buộc cho role test, phát URL không có `transcode=true`; luồng phải phát được và
   derived stream chứa `.transcode.overlay.`. Kiểm tra watermark hiển thị đúng.
2. Tắt watermark, bật privacy mask cho role test, phát lại bằng `session_id` mới; luồng phải phát
   được qua `.transcode.overlay.` và vùng che phải đúng tọa độ.
3. Trong cả hai bước, thêm `transcode=true` vẫn phải bị HTTP 403.

### RC-08 — giảm chất lượng kết hợp overlay

Bật lại option và giữ một overlay có hiệu lực. Gửi profile hợp lệ ở RC-03. Luồng phải phát được,
derived stream chứa `.transcode.quality-overlay.`, thông số video vẫn khớp profile và overlay vẫn
hiển thị đúng.

### RC-09 — metadata thay đổi và reconnect

Trong giao diện camera thật, đổi độ phân giải hoặc FPS của một stream rồi reconnect stream đó. Sau
khi stream online:

- API service nhận một payload `media-device-info` mới;
- profile được tính lại từ metadata mới;
- profile không còn phù hợp biến mất;
- request dùng profile cũ trả HTTP 400;
- request dùng profile mới phát được.

Khôi phục cấu hình camera ban đầu và xác minh capability được cập nhật lần nữa. Nếu metadata không
đổi thì reconnect đơn thuần không được tạo chuỗi payload capability trùng lặp liên tục.

### RC-10 — persistence qua restart

Để option ở trạng thái bật, restart MediaServer theo quy trình vận hành bình thường, không xóa data
directory. Sau khi camera online:

- cấu hình trả về vẫn có `enableReduceQualityOnView=true`;
- hook báo `supported=true` và có profile tương ứng metadata hiện tại;
- phát lại một profile thành công.

Sau đó đặt option về giá trị ban đầu của môi trường test.

## Kiểm tra tải tối thiểu

Mở đồng thời 5 player cùng một URL/profile nhưng dùng 5 `session_id` khác nhau trong 5 phút. Xác
minh tất cả player phát liên tục, chỉ một derived MediaSource/encoder được dùng chung cho cùng tổ
hợp profile/overlay, không có tăng bộ nhớ liên tục và CPU trở về gần baseline sau khi đóng player.

## Bằng chứng cần lưu

- Phiên bản/commit và cấu hình build MediaServer.
- Model/firmware camera; metadata thực tế của `main` và `sub`.
- Payload hook cho trạng thái tắt, bật và sau reconnect.
- HTTP status, redirect `Location`, output `ffprobe` cho từng case.
- Log MediaServer từ lúc đổi option đến khi derived stream được giải phóng.
- CPU/RAM trước, trong và sau kiểm tra tải; ảnh xác minh watermark/privacy mask.

## Tiêu chí hoàn tất

Kịch bản đạt khi RC-01 đến RC-10 đều pass, không có crash/deadlock/decode error kéo dài, không có
profile lỗi thời sau reconnect/restart và không làm lộ credential trong log/report. Nếu camera không
hỗ trợ H265 thì ghi `N/A` riêng cho profile H265; không được bỏ qua các case H264 còn lại.
