# Hướng dẫn tích hợp phát media

Tài liệu này mô tả URL để tích hợp xem trực tiếp và xem lại trên S3MediaKit.

Quy ước dùng trong tài liệu:

- `BASE_URL`: địa chỉ HTTP của MediaServer, ví dụ `https://media.example.com`.
- `WS_URL`: đổi `http` thành `ws`, hoặc `https` thành `wss`.
- `cameraId`: mã camera.
- `streamId`: mã profile/luồng của camera, ví dụ `main` hoặc `sub`.
- `stamp`: thời điểm bắt đầu xem lại, Unix timestamp tính bằng giây.

## 1. Xác thực

Các URL media được kiểm tra quyền thông qua hook xác thực của server. Player có thể gửi JWT theo một trong hai cách:

```text
Authorization: Bearer <JWT>
```

hoặc thêm query string:

```text
?token=<JWT>
```

Header `Authorization` phù hợp với `fetch` và WebSocket client tự quản lý header. Với thẻ `<video>` hoặc player không cho đặt header, dùng `token` trên URL. Không ghi token vào log hoặc URL công khai nếu có thể tránh được.

## 2. Xem trực tiếp

Luồng trực tiếp bắt đầu bằng `/media/live`. Phần `live` là prefix truy cập HTTP; app media thực tế được server suy ra từ phần còn lại của URL.

### 2.1. HTTP-MP4

Đây là fragmented MP4 truyền liên tục qua một kết nối HTTP. Dùng khi player hỗ trợ MSE/fMP4.

Stream cụ thể:

```text
GET {BASE_URL}/media/live/{cameraId}/{streamId}.live.mp4
```

Ví dụ:

```text
https://media.example.com/media/live/camera-001/main.live.mp4?token=<JWT>
```

Không chọn stream cụ thể — server tự chọn chất lượng:

```text
GET {BASE_URL}/media/live/{cameraId}.live2.mp4?quality=auto&prefered=hi
```

`quality` nhận `hi`, `lo` hoặc `auto`. Khi dùng `auto`, `prefered=hi` ưu tiên luồng chất lượng cao và tự fallback sang `lo` nếu cần.

### 2.2. WS-MP4

WS-MP4 dùng cùng path với HTTP-MP4 nhưng đổi scheme:

```text
ws://media.example.com/media/live/{cameraId}/{streamId}.live.mp4
wss://media.example.com/media/live/{cameraId}/{streamId}.live.mp4?token=<JWT>
```

URL tự chọn chất lượng:

```text
wss://media.example.com/media/live/{cameraId}.live2.mp4?quality=auto&prefered=hi&token=<JWT>
```

Sau khi WebSocket kết nối, dữ liệu nhận được là byte stream fMP4, bắt đầu bằng init segment. Ứng dụng web thường chuyển các segment nhận được vào `MediaSource`/`SourceBuffer`.

### 2.3. HLS

HLS dùng playlist của đúng stream:

```text
GET {BASE_URL}/media/live/{cameraId}/{streamId}/hls.m3u8?session_id={SESSION_ID}
```

Mỗi player nên tạo một `session_id` riêng có độ ngẫu nhiên cao (khuyến nghị UUID), giữ nguyên giá trị đó trong suốt một phiên xem và không dùng chung giữa các tab/player. Nếu client không truyền `session_id`, server tái sử dụng session từ cookie đúng phạm vi camera hoặc tự sinh một giá trị, sau đó trả HTTP 302 đến chính playlist với `session_id` trong query. HLS.js và trình phát HLS native sẽ theo redirect rồi tiếp tục reload URL có session này. Server cũng đưa ID đã chọn vào URI của playlist con, init segment, key và media segment để nhiều HTTP connection của cùng player vẫn chỉ được tính là một người xem. Phiên sẽ hết hạn nếu không có request trong `hls.viewerTimeoutSec` (mặc định 60 giây).

Server hỗ trợ đồng thời query `session_id` và cookie `S3_COOKIE`. Query ID có độ ưu tiên cao hơn cookie để các tab/player trong cùng trình duyệt vẫn được đếm riêng. Nếu URI con làm mất `session_id`, cookie được dùng làm fallback; ngược lại, nếu cookie bị chặn nhưng URI vẫn giữ ID thì server vẫn nhận diện được phiên. `session_id` có ký tự không hợp lệ hoặc dài quá 128 ký tự bị từ chối với HTTP 400.

Nếu không biết trước `streamId`, dùng HLS master playlist:

```text
GET {BASE_URL}/media/live/{cameraId}/hls.master.m3u8?session_id={SESSION_ID}
```

Master playlist liệt kê các stream con và player có thể tự chọn bitrate/chất lượng. Nếu client không truyền `session_id` vào master playlist hoặc media playlist trực tiếp, server redirect đến URL có ID trước khi trả playlist. Các URI con dùng cùng ID. Redirect transcode cũng đi thẳng đến derived playlist có session để tránh thêm một vòng redirect. Cookie-only vẫn tương thích, nhưng client nên truyền ID riêng cho từng player để đếm chính xác khi nhiều tab cùng phát một camera.

Ví dụ với hls.js:

```js
const sessionId = crypto.randomUUID(); // tạo một lần cho mỗi player
const params = new URLSearchParams({ token, session_id: sessionId });
const url = `${BASE_URL}/media/live/${encodeURIComponent(cameraId)}`
  + `/${encodeURIComponent(streamId)}/hls.m3u8?${params}`;

if (Hls.isSupported()) {
  const hls = new Hls();
  hls.loadSource(url);
  hls.attachMedia(videoElement);
} else {
  videoElement.src = url; // Safari/iOS native HLS
}
```

### 2.4. HLS fMP4

HLS fMP4 dùng playlist tương tự HLS nhưng các media segment là fragmented MP4:

```text
GET {BASE_URL}/media/live/{cameraId}/{streamId}/hls.fmp4.m3u8
```

Một số player hỗ trợ HLS fMP4 trực tiếp; với player web, kiểm tra khả năng hỗ trợ trước khi chọn URL này. Nếu không hỗ trợ, dùng HLS MPEG-TS (`hls.m3u8`) hoặc HTTP/WS-MP4.

## 3. Xem lại recording

Luồng xem lại bắt đầu bằng `/media/record` và dùng `app=record` của hệ thống recording. `stamp` phải nằm trong khoảng thời gian có dữ liệu recording; nên lấy timeline trước bằng API [`/media/esc/recordedTimePeriod`](./playback-flow.md#1-lấy-timeline-recording).

### 3.1. HTTP-MP4 xem lại

Stream cụ thể:

```text
GET {BASE_URL}/media/record/{cameraId}/{streamId}/vod/{stamp}.live.mp4
```

Ví dụ:

```text
https://media.example.com/media/record/camera-001/main/vod/1710000000.live.mp4?token=<JWT>
```

Tự chọn chất lượng:

```text
GET {BASE_URL}/media/record/{cameraId}/vod/{stamp}.live2.mp4?quality=auto
```

Các giá trị `quality` là `auto`, `hi` và `lo`. URL `.live.mp4` dành cho stream đã biết; URL `.live2.mp4` dùng khi backend cần chọn giữa các stream chất lượng.

### 3.2. WS-MP4 xem lại

Đổi scheme của URL HTTP-MP4 sang `ws`/`wss`, giữ nguyên path:

```text
wss://media.example.com/media/record/{cameraId}/{streamId}/vod/{stamp}.live.mp4?token=<JWT>
wss://media.example.com/media/record/{cameraId}/vod/{stamp}.live2.mp4?quality=auto&token=<JWT>
```

Client xử lý dữ liệu fMP4 giống WS-MP4 trực tiếp. Khi người dùng seek, nên đóng kết nối hiện tại và tạo URL mới với `stamp` mới.

### 3.3. HLS và HLS fMP4 khi xem lại

HLS (`hls.m3u8`) và HLS fMP4 (`hls.fmp4.m3u8`) là các endpoint phát trực tiếp của MediaSource. Replay recording hỗ trợ HLS fMP4 on-demand theo URL riêng bên dưới; không dùng cách đổi trực tiếp URL `.live.mp4` thành `hls.m3u8`.

Replay HLS fMP4 on-demand dùng URL:

```text
/media/record/{cameraId}/{streamId}/vod/{stamp}/hls.fmp4.m3u8?session_id={SESSION_ID}
```

Trong đó `{stamp}` là Unix timestamp giây tại điểm bắt đầu replay. Giữ nguyên `session_id` khi seek nếu vẫn là cùng player. Server đọc recording MP4 tương ứng, tạo một `HlsMediaSource` dùng chung cho các request đồng thời và dùng cùng sliding window với HLS live theo `hls.segNum`, `hls.segRetain`, `hls.deleteDelaySec`. Segment cũ không được giữ toàn bộ; source tự đóng/xoá khi replay không còn người xem. Seek được thực hiện bằng cách mở URL mới với `{stamp}` khác.

URL trên dành cho replay single-stream đã biết chất lượng. Master playlist `quality=auto` cho nhiều stream sẽ được bổ sung ở bước tiếp theo; hiện vẫn dùng endpoint `.live2.mp4` cho trường hợp đó.

## 4. Hàm tạo URL

```js
function toWsUrl(baseUrl) {
  return baseUrl.replace(/^https:/i, 'wss:').replace(/^http:/i, 'ws:');
}

function liveUrl(baseUrl, cameraId, streamId, protocol, token, sessionId) {
  const camera = encodeURIComponent(cameraId);
  const stream = encodeURIComponent(streamId);
  let url;
  if (protocol === 'hls') {
    url = `${baseUrl}/media/live/${camera}/${stream}/hls.m3u8`;
  } else if (protocol === 'hls.fmp4') {
    url = `${baseUrl}/media/live/${camera}/${stream}/hls.fmp4.m3u8`;
  } else {
    const base = protocol === 'ws-mp4' ? toWsUrl(baseUrl) : baseUrl;
    url = `${base}/media/live/${camera}/${stream}.live.mp4`;
  }
  const params = new URLSearchParams();
  if (token) params.set('token', token);
  if ((protocol === 'hls' || protocol === 'hls.fmp4') && sessionId) {
    params.set('session_id', sessionId);
  }
  return params.size ? `${url}?${params}` : url;
}

function replayUrl(baseUrl, cameraId, streamId, stamp, ws, token) {
  const base = ws ? toWsUrl(baseUrl) : baseUrl;
  const url = `${base}/media/record/${encodeURIComponent(cameraId)}`
    + `/${encodeURIComponent(streamId)}/vod/${stamp}.live.mp4`;
  return token ? `${url}?token=${encodeURIComponent(token)}` : url;
}
```

## 5. Ma trận chọn giao thức

| Nhu cầu | URL/kết nối nên dùng |
|---|---|
| Live, player hỗ trợ HLS | `/media/live/{cameraId}/{streamId}/hls.m3u8` |
| Live, muốn segment fMP4 qua HTTP | `/media/live/{cameraId}/{streamId}.live.mp4` |
| Live, client dùng WebSocket | `ws(s)://.../media/live/{cameraId}/{streamId}.live.mp4` |
| Live, HLS segment fMP4 | `/media/live/{cameraId}/{streamId}/hls.fmp4.m3u8` |
| Replay, stream đã biết | `/media/record/{cameraId}/{streamId}/vod/{stamp}.live.mp4` |
| Replay, tự chọn chất lượng | `/media/record/{cameraId}/vod/{stamp}.live2.mp4?quality=auto` |

## 6. Lỗi thường gặp

- `404`: camera/stream không online hoặc `stamp` không thuộc timeline recording.
- `401`: thiếu hoặc hết hạn JWT; kiểm tra `Authorization` hoặc `token`.
- HLS không phát: kiểm tra player có hỗ trợ HLS/HLS fMP4 và URL playlist có thể truy cập từ trình duyệt.
- WS-MP4 không phát: dùng `wss` khi trang chạy trên HTTPS và bảo đảm proxy hỗ trợ WebSocket upgrade.
- Replay sai nội dung: không dùng `/media/live`; replay luôn phải bắt đầu bằng `/media/record` và app recording là `record`.

## 7. Reverse proxy có rewrite prefix

Khi Nginx bỏ một prefix trước khi chuyển request tới MediaServer, cấu hình Nginx ghi đè URI public ban đầu vào một header riêng:

```nginx
location /media2/ {
    proxy_set_header X-Original-URI $request_uri;
    proxy_pass http://media_server/;
}
```

Chỉ bật header này cho địa chỉ proxy kết nối trực tiếp tới MediaServer:

```ini
[http]
original_url_header=X-Original-URI
original_url_trusted_proxy=127.0.0.1,10.0.0.10
```

`original_url_trusted_proxy` hỗ trợ IPv4, IPv6 và khoảng địa chỉ giống `allow_ip_range`. Danh sách rỗng không tin proxy nào. Nginx phải ghi đè `X-Original-URI`, không chuyển tiếp hoặc nối giá trị do client gửi. Không ghi toàn bộ header này vào log vì query string có thể chứa JWT.

Phiên bản hiện tại hỗ trợ rewrite theo dạng thêm/bỏ prefix và giữ nguyên phần path nội bộ cùng query string. Ví dụ `/media2/media/...` được proxy thành `/media/...`; MediaServer vẫn route bằng `/media/...`, nhưng dùng `/media2/media/...` cho `Set-Cookie Path`, HLS redirect và URL con trong master playlist. Forwarded scheme/host và thuộc tính cookie `Secure` khi TLS kết thúc tại Nginx cần được cấu hình/xử lý riêng.
