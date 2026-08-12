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
GET {BASE_URL}/media/live/{cameraId}/{streamId}/hls.m3u8
```

Nếu không biết trước `streamId`, dùng HLS master playlist:

```text
GET {BASE_URL}/media/live/{cameraId}/hls.master.m3u8
```

Master playlist liệt kê các stream con và player có thể tự chọn bitrate/chất lượng.

Ví dụ với hls.js:

```js
const url = `${BASE_URL}/media/live/${encodeURIComponent(cameraId)}`
  + `/${encodeURIComponent(streamId)}/hls.m3u8?token=${encodeURIComponent(token)}`;

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

HLS (`hls.m3u8`) và HLS fMP4 (`hls.fmp4.m3u8`) là các endpoint phát trực tiếp của MediaSource. Chức năng xem lại recording hiện được cung cấp qua replay MP4 (`.live.mp4`/`.live2.mp4`), không phải bằng cách đổi `/media/live` thành `/media/record` rồi gắn `hls.m3u8`.

Nếu cần HLS cho recording, ứng dụng phải có bước đóng gói/serve playlist recording riêng và player dùng URL playlist do bước đó tạo ra. Không dùng URL HLS trực tiếp để thay thế endpoint replay MP4.

## 4. Hàm tạo URL

```js
function toWsUrl(baseUrl) {
  return baseUrl.replace(/^https:/i, 'wss:').replace(/^http:/i, 'ws:');
}

function liveUrl(baseUrl, cameraId, streamId, protocol, token) {
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
  return token ? `${url}?token=${encodeURIComponent(token)}` : url;
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
