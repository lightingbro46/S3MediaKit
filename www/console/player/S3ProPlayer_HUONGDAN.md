# S3ProPlayer: Kiến trúc & Hướng dẫn sử dụng

## Tổng quan kiến trúc

`S3ProPlayer` (file: `s3pro-player.js`) là thư viện JavaScript phát video FMP4 (live/replay) với đặc điểm:
- **Export dạng UMD**: Dùng được với cả script tag, AMD và CommonJS (`require`/`import`).
- **API theo hướng class**: Tạo nhiều instance độc lập, không phụ thuộc DOM, không tạo biến global.
- **Tích hợp Video.js**: Cho phép truyền instance hoặc `videoElId`, không phụ thuộc vào global `videojs`.
- **Callback/Event-driven**: Log, error, timeupdate, statechange đều qua callback hoặc event.
- **Tùy biến message**: Không hardcode tiếng Việt, mọi thông báo đều override được qua `options.messages`.

### Sơ đồ kiến trúc

```
+---------------------+
|    S3ProPlayer      |<-------------------+
+---------------------+                    |
| - _vjsPlayer        |                    |
| - _activeHandler    |                    |
| - _listeners        |                    |
+---------------------+                    |
| play(url)           |                    |
| stop()              |                    |
| dispose()           |                    |
| getState()          |                    |
| on(event, cb)       |                    |
| off(event, cb)      |                    |
+---------------------+                    |
        |                                   |
        v                                   |
+----------------------------+              |
| S3ProLiveSourceHandler     |              |
| (internal — MSE streaming) |              |
+----------------------------+              |
        ^                                   |
        |                                   |
+----------------------------+              |
| Video.js Html5Tech         |--------------+
+----------------------------+
```

---

## Hướng dẫn sử dụng theo hướng class

### 1. Tích hợp cơ bản với script tag

```html
<script src="video.js"></script>
<script src="s3pro-player.js"></script>
<div id="vjs-player"></div>
<script>
  const player = new S3ProPlayer({
    videojs:   window.videojs,
    videoElId: 'vjs-player',

    onLog:        (level, msg) => console.log('[' + level + ']', msg),
    onFatalError: (msg) => alert('Lỗi: ' + msg),
    onTimeUpdate: (ct, fmt, codec, bufLen) => {
      document.getElementById('time').textContent = fmt;
    },
  });

  player.on('statechange', state => console.log('State:', state));
  player.play('http://host/stream.live.mp4');
</script>
```

### 2. Dùng với import module (ES module / CommonJS)

```js
// ES module
import S3ProPlayer from './s3pro-player.js';
import videojs from 'video.js';

const player = new S3ProPlayer({ videojs, videoElId: 'vjs-player' });
player.play('http://host/stream.live.mp4');

// CommonJS
const S3ProPlayer = require('./s3pro-player');
```

---

## API class S3ProPlayer

### Constructor options

| Option | Kiểu | Bắt buộc | Mô tả |
|---|---|---|---|
| `videojs` | Function | Có | Thư viện video.js (`window.videojs` hoặc import) |
| `videoElId` | string | Không | Id của `<video>` hoặc `<div>` (mặc định: `'vjs-player'`) |
| `vjsPlayer` | Object | Không | Instance videojs có sẵn (bỏ qua việc khởi tạo mới) |
| `messages` | Object | Không | Override thông báo lỗi/codec |
| `onLog` | Function | Không | `(level, msg)` — nhận mọi log |
| `onFatalError` | Function | Không | `(msg)` — lỗi nặng, dừng phát |
| `onTimeUpdate` | Function | Không | `(currentTime, formatted, codec, bufferSec)` |
| `onStateChange` | Function | Không | `(state)` |

### Phương thức công khai

| Phương thức | Mô tả |
|---|---|
| `play(url)` | Bắt đầu phát stream từ URL |
| `stop()` | Dừng phát, giải phóng MSE |
| `dispose()` | Giải phóng toàn bộ tài nguyên + videojs |
| `getState()` | Trả về `{ state, codec, bufferLen, currentTime }` |
| `on(event, cb)` | Đăng ký lắng nghe event |
| `off(event, cb)` | Hủy lắng nghe event (bỏ `cb` để xóa toàn bộ) |

### Events

| Event | Tham số | Mô tả |
|---|---|---|
| `'log'` | `(level, msg)` | Mọi log (`'info'\|'warn'\|'error'`) |
| `'error'` | `(msg)` | Lỗi nặng — `null` = xóa lỗi cũ |
| `'timeupdate'` | `(ct, fmt, codec, bufLen)` | Cập nhật thời gian 200ms/lần |
| `'statechange'` | `(state)` | `'idle'\|'connecting'\|'playing'\|'stopped'\|'error'\|'disposed'` |

### Override error messages

Các key có thể override trong `options.messages`:

| Key | Mặc định (tiếng Anh) |
|---|---|
| `noMSE` | Browser does not support MediaSource Extensions (MSE). |
| `hevcUnsupported` | Browser does not support H.265/HEVC decoding... |
| `av1Unsupported` | Browser does not support AV1 codec: |
| `vp9Unsupported` | Browser does not support VP9 codec: |
| `vp8Unsupported` | Browser does not support VP8 codec: |
| `codecUnsupported` | Codec not supported: |
| `connectionFailed` | Connection failed: |
| `readError` | Stream read error: |

---

## Ví dụ sử dụng nâng cao

### a. Lắng nghe log, error, timeupdate riêng biệt

```js
const player = new S3ProPlayer({ videojs, videoElId: 'vjs-player' });

player.on('log', (level, msg) => {
  if (level === 'error') console.error(msg);
  else console.log(msg);
});

// error = null khi xóa lỗi cũ (ví dụ khi bấm play lại)
player.on('error', msg => {
  if (msg) showErrorOverlay(msg);
  else     hideErrorOverlay();
});

player.on('timeupdate', (ct, fmt, codec, bufLen) => {
  timeEl.textContent  = fmt;
  codecEl.textContent = codec || '--';
  bufEl.textContent   = bufLen ? bufLen.toFixed(2) + 's' : '--';
});

player.on('statechange', state => {
  statusBadge.textContent = state;
});
```

### b. Tùy biến thông báo lỗi

```js
const player = new S3ProPlayer({
  videojs,
  messages: {
    noMSE:           'Trình duyệt không hỗ trợ MSE',
    hevcUnsupported: 'Không hỗ trợ H.265/HEVC',
    connectionFailed: 'Kết nối thất bại: ',
  },
});
```

### c. Dùng với videojs player đã tạo sẵn

```js
// Nếu bạn tự khởi tạo videojs trước:
const vjsPlayer = videojs('vjs-player', { controls: false, muted: true });

const player = new S3ProPlayer({
  videojs,
  vjsPlayer,  // truyền thẳng instance, bỏ qua việc tạo mới
});
player.play('http://host/stream.live.mp4');
```

### d. Tích hợp với React (hoặc Vue/Svelte)

```jsx
import { useEffect, useRef } from 'react';
import S3ProPlayer from './s3pro-player.js';
import videojs from 'video.js';

export function VideoPlayer({ url }) {
  const containerRef = useRef(null);
  const playerRef    = useRef(null);

  useEffect(() => {
    const el = containerRef.current;
    el.id = 'vjs-' + Date.now(); // unique id

    playerRef.current = new S3ProPlayer({
      videojs,
      videoElId: el.id,
      onFatalError: msg => console.error('Fatal:', msg),
    });
    if (url) playerRef.current.play(url);

    return () => playerRef.current.dispose();
  }, []);

  useEffect(() => {
    if (url && playerRef.current) playerRef.current.play(url);
  }, [url]);

  return <div ref={containerRef} className="video-wrap" />;
}
```

### e. Lấy trạng thái hiện tại

```js
const state = player.getState();
console.log(state.state);       // 'idle' | 'connecting' | 'playing' | ...
console.log(state.codec);       // 'video/mp4; codecs="avc1.4D401F,mp4a.40.2"'
console.log(state.bufferLen);   // số giây đã buffer
console.log(state.currentTime); // currentTime hiện tại
```

---

## Tích hợp với player-controls.js

`player-controls.js` hỗ trợ chế độ Live/Replay, timeline ghi hình, custom control bar. Nó nhận instance `S3ProPlayer` làm tham số và tự động wire toàn bộ UI.

```html
<script src="video.js"></script>
<script src="s3pro-player.js"></script>
<script src="player-controls.js"></script>
<script>
  // Khởi tạo player
  const player = new S3ProPlayer({
    videojs:   window.videojs,
    videoElId: 'vjs-player',
  });

  // Khởi tạo UI controls và truyền player vào
  initPlayerControls(player);
</script>
```

`initPlayerControls(player)` tự động:
- Wire `player.on('log', ...)` → hiển thị log vào `#log`
- Wire `player.on('error', ...)` → hiển thị/ẩn overlay lỗi `#player-error`
- Wire `player.on('timeupdate', ...)` → cập nhật `#info-text`, `#time-display`
- Wire `#btn-play` click → gọi `player.play(url)` (sau khi URL đã được build)
- Wire `#btn-stop` click → gọi `player.stop()`
- Wire `url-input` Enter key → trigger `#btn-play` click
- Hỗ trợ auto-play từ `?url=` query parameter

---

## Lưu ý kỹ thuật

- Không tạo biến global nào. Class được gắn vào `window.S3ProPlayer` chỉ khi dùng script tag.
- Có thể tạo nhiều instance player độc lập trên cùng trang.
- Không phụ thuộc DOM ngoài `videoElId` truyền vào.
- Hỗ trợ codec: H.264, H.265/HEVC, VP8, VP9, AV1, AAC, Opus — tự detect từ init segment.
- Stall detection: tự động seek/nudge khi video bị stuck (đọc buffer liên tục).
- Codec change trong luồng: hỗ trợ `changeType()` nếu browser hỗ trợ, fallback recreate MediaSource.
- Buffer management: tự xóa buffer cũ (> 30s trước currentTime) để tránh tràn bộ nhớ.

---

**Project:** S3MediaKit — s3pro-player v2
