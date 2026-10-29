# Biểu đồ 60 phút: `systemStatisticHistory` + `systemStatistic`

Đề xuất cho FE vẽ biểu đồ tài nguyên 60 phút gần nhất của một MediaServer (CPU, RAM, mạng, đĩa,
số luồng xem):

1. Mở biểu đồ: gọi **`systemStatisticHistory` một lần** để lấy 60 điểm gần nhất (mỗi phút một điểm).
2. Sau đó gọi **`systemStatistic` mỗi 60 s**, đổi kết quả thành một điểm, thêm vào cuối biểu đồ
   và bỏ các điểm cũ hơn 60 phút.

Không cần gọi lại `systemStatisticHistory` theo chu kỳ. Chỉ gọi lại khi biểu đồ bị gián đoạn (mục 5).

**Điều kiện:** chỉ gọi `systemStatisticHistory` khi `systemStatistic` trả `data.hasHistoryRecords = true`.
MediaServer bản cũ không có field này (hoặc trả `false`) thì không có API lịch sử: FE bỏ bước 1, bắt
đầu với biểu đồ rỗng và chỉ append từ `systemStatistic` (mục 3, 5).

## 1. Dữ liệu phía server

- Server lấy mẫu mỗi `globalmonitor.metrics_sample_interval_sec` giây (mặc định **60 s**), ghi vào
  bảng `system_metrics`, giữ `globalmonitor.metrics_retention_days` ngày (mặc định 7). Mỗi mẫu là giá
  trị tức thời tại lúc lấy mẫu, không phải trung bình của cả phút.
- `systemStatistic` trả giá trị hiện tại từ cache RAM, rẻ, không đụng DB.
- `systemStatisticHistory` đọc SQLite (chạy trên pool API), không nên gọi liên tục.
- Một mẫu có thể bị bỏ khi DB đang bận, và không có mẫu trong lúc server restart, nên lịch sử có thể
  **hở vài phút**. FE vẽ theo `timestamp`, không vẽ theo chỉ số mảng.

Cả hai API dùng JWT người dùng (`Authorization: Bearer <token>`) và cần quyền đọc MediaServer
(`READ_MSERVER_PERMISSION_CODE`). Thành công trả `code = 0`; thiếu tham số bắt buộc trả
`code = CODE_INVALID_ARGS`.

## 2. `GET /media/mserver/systemStatisticHistory`

| Tham số | Bắt buộc | Ý nghĩa |
|---|---|---|
| `from` | có | unix giây, đầu khoảng (tính cả mốc này) |
| `to` | có | unix giây, cuối khoảng (tính cả mốc này); `0` = không giới hạn trên |
| `limit` | có | số mẫu thô tối đa; `0` = không giới hạn |
| `bucket_sec` | không | gộp mẫu theo khoảng `bucket_sec` giây (trung bình); bỏ trống hoặc `0` = trả mẫu thô |

Lưu ý quan trọng về `limit`: server sắp xếp **tăng dần theo thời gian** rồi mới cắt `limit`, nên
`limit` giữ các mẫu **cũ nhất** của khoảng. Muốn 60 điểm gần nhất thì giới hạn bằng khoảng thời gian,
không bằng `limit`:

```
GET /media/mserver/systemStatisticHistory?from=<now-3600>&to=<now>&limit=0&bucket_sec=60
```

- Khoảng 3600 s với mẫu 60 s cho tối đa 61 mẫu, `limit=0` không làm nặng server.
- `bucket_sec=60` làm tròn `timestamp` về đầu phút và gộp hai mẫu rơi vào cùng phút (nếu có), FE
  nhận đúng một điểm mỗi phút. FE cắt `slice(-60)` nếu nhận 61 điểm.
- Nếu `bucket_sec` khác 0, `limit` vẫn áp lên mẫu thô **trước** khi gộp.

### Kết quả (`data` là mảng, tăng dần theo `timestamp`)

Có `bucket_sec`:

```json
{
  "code": 0,
  "data": [
    {
      "timestamp": 1761620400,
      "samples": 1,
      "cpu_usage_pct": 23.5,
      "cpu_proc_usage_pct": 11.2,
      "ram_used": 3221225472,
      "ram_total": 8589934592,
      "ram_usage_pct": 37.5,
      "reader_total": 12,
      "reader_live": 10,
      "reader_playback": 2,
      "net_rx_mbps": 85.3,
      "net_tx_mbps": 40.1,
      "hdd_usage_pct": 71.0,
      "disk_io_util_pct": 18.4,
      "disk_await_ms": 3.2
    }
  ]
}
```

Không có `bucket_sec` thì mỗi phần tử có thêm `id`, `cpu_cores`, mảng `nets[]` và `disks[]` gốc, không
có `samples`.

| Field | Ý nghĩa |
|---|---|
| `timestamp` | unix giây theo **đồng hồ server**; có `bucket_sec` thì là đầu bucket |
| `samples` | số mẫu thô được gộp vào điểm này |
| `cpu_usage_pct`, `cpu_proc_usage_pct` | CPU toàn máy / của tiến trình MediaServer (%) |
| `ram_used`, `ram_total` (byte), `ram_usage_pct` (%) | RAM toàn máy |
| `reader_total`, `reader_live`, `reader_playback` | số luồng xem: tổng / live / xem lại |
| `net_rx_mbps`, `net_tx_mbps` | **tổng** mọi card mạng (Mbps) |
| `hdd_usage_pct` | % dùng của **ổ đầy nhất** trong `disks` |
| `disk_io_util_pct`, `disk_await_ms` | % bận IO và độ trễ IO của **ổ tệ nhất**; chưa có mẫu IO thì 0 |
| `history_source` | chỉ có khi bằng `"live_fallback"`, xem dưới |

**Chưa có lịch sử** (server vừa cài, hoặc không có mẫu nào trong khoảng): server trả **một** điểm lấy
từ giá trị hiện tại, có `"history_source": "live_fallback"` và `samples = 1`. FE vẽ bình thường như
một điểm đầu tiên. Nếu `to` nằm trước thời điểm hiện tại, mảng rỗng.

## 3. `GET /media/mserver/systemStatistic`

Không có tham số. Các field dùng cho biểu đồ (phần còn lại phục vụ gauge, ngưỡng, danh sách camera
đang được xem):

```json
{
  "code": 0,
  "data": {
    "cpu":  { "cores": 8, "usage_pct": 23.5, "proc_usage_pct": 11.2 },
    "ram":  { "used": 3221225472, "total": 8589934592, "usage_pct": 37.5, "proc_usage_pct": 4.1 },
    "nets": [ { "name": "eth0", "rx_mbps": 85.3, "tx_mbps": 40.1, "speed_mbps": 1000 } ],
    "disks": [ { "mount": "/data", "used_pct": 71.0, "io_util_pct": 18.4, "await_ms": 3.2 } ],
    "reader": { "totalStreamCount": 12, "liveStreamCount": 10, "playbackStreamCount": 2 },
    "threshold": { "cpu_levelLow": 80, "cpu_levelMedium": 90 },
    "hasHistoryRecords": true
  }
}
```

**Không có `timestamp`** trong kết quả. Dùng header HTTP `Date` của chính response (giờ server, độ
phân giải giây) để cùng trục thời gian với lịch sử; nếu trình duyệt không đọc được header đó (CORS
chưa expose `Date`) thì dùng giờ máy client.

`disks[].io_util_pct` và `await_ms` là `null` khi chưa có mẫu IO; coi là 0 như lịch sử.

`hasHistoryRecords`: `true` nghĩa là server có `systemStatisticHistory`. **Chỉ khi field này đúng bằng
`true` FE mới gọi `systemStatisticHistory`**; thiếu field (server bản cũ) hoặc `false` thì coi như
không có lịch sử, không gọi API đó (gọi sẽ lỗi vì endpoint không tồn tại).

## 4. Đổi `systemStatistic` thành một điểm biểu đồ

Tên field hai API khác nhau. FE phải tính các field gộp **đúng cách server tính lịch sử**, nếu không
điểm mới sẽ lệch so với 60 điểm cũ:

| Điểm biểu đồ | Từ `systemStatistic.data` |
|---|---|
| `cpu_usage_pct` | `cpu.usage_pct` |
| `cpu_proc_usage_pct` | `cpu.proc_usage_pct` |
| `ram_used`, `ram_total`, `ram_usage_pct` | `ram.used`, `ram.total`, `ram.usage_pct` |
| `reader_total` | `reader.totalStreamCount` |
| `reader_live` | `reader.liveStreamCount` |
| `reader_playback` | `reader.playbackStreamCount` |
| `net_rx_mbps` | **tổng** `nets[].rx_mbps` |
| `net_tx_mbps` | **tổng** `nets[].tx_mbps` |
| `hdd_usage_pct` | **max** `disks[].used_pct` |
| `disk_io_util_pct` | **max** `disks[].io_util_pct` (null = 0) |
| `disk_await_ms` | **max** `disks[].await_ms` (null = 0) |

## 5. Luồng FE đề xuất

```js
const WINDOW_SEC = 3600;
const STEP_SEC = 60;
let points = [];          // tăng dần theo timestamp, tối đa 60 phút
let timer = null;

const minute = (ts) => Math.floor(ts / STEP_SEC) * STEP_SEC;
const num = (v) => (typeof v === 'number' && isFinite(v) ? v : 0);
const sum = (arr, f) => (arr || []).reduce((s, x) => s + num(x[f]), 0);
const max = (arr, f) => (arr || []).reduce((m, x) => Math.max(m, num(x[f])), 0);

function toPoint(d, ts) {
  return {
    timestamp: minute(ts),
    cpu_usage_pct: d.cpu.usage_pct,
    cpu_proc_usage_pct: d.cpu.proc_usage_pct,
    ram_used: d.ram.used,
    ram_total: d.ram.total,
    ram_usage_pct: d.ram.usage_pct,
    reader_total: d.reader.totalStreamCount,
    reader_live: d.reader.liveStreamCount,
    reader_playback: d.reader.playbackStreamCount,
    net_rx_mbps: sum(d.nets, 'rx_mbps'),
    net_tx_mbps: sum(d.nets, 'tx_mbps'),
    hdd_usage_pct: max(d.disks, 'used_pct'),
    disk_io_util_pct: max(d.disks, 'io_util_pct'),
    disk_await_ms: max(d.disks, 'await_ms'),
  };
}

function append(p) {
  const last = points[points.length - 1];
  if (last && last.timestamp === p.timestamp) {
    points[points.length - 1] = p;            // cùng phút: thay, không thêm
  } else if (!last || p.timestamp > last.timestamp) {
    points.push(p);
  }
  const cutoff = p.timestamp - WINDOW_SEC;
  points = points.filter((x) => x.timestamp > cutoff);
  render(points);
}

let hasHistoryRecords = false;   // lấy từ systemStatistic.data.hasHistoryRecords

async function loadHistory() {
  if (!hasHistoryRecords) { points = []; render(points); return; }   // server không có lịch sử
  const now = Math.floor(Date.now() / 1000);
  const res = await api.get('/media/mserver/systemStatisticHistory', {
    from: now - WINDOW_SEC, to: now, limit: 0, bucket_sec: STEP_SEC,
  });
  points = res.data.slice(-60);
  render(points);
}

async function poll() {
  const res = await api.getWithHeaders('/media/mserver/systemStatistic');
  hasHistoryRecords = res.body.data.hasHistoryRecords === true;
  const dateHeader = res.headers.get('Date');
  const ts = dateHeader ? Date.parse(dateHeader) / 1000 : Date.now() / 1000;
  append(toPoint(res.body.data, ts));
}

async function start() {
  await poll();           // điểm đầu tiên + đọc hasHistoryRecords
  await loadHistory();    // chỉ gọi systemStatisticHistory khi hasHistoryRecords = true
  await poll();           // nối lại điểm hiện tại sau khi thay mảng bằng lịch sử
  timer = setInterval(() => poll().catch(onPollError), STEP_SEC * 1000);
}
```

Quy tắc:

- **Một điểm mỗi phút.** Nếu màn hình đã gọi `systemStatistic` dày hơn (vd 3–5 s cho gauge), dùng lại
  kết quả đó và gọi `append` mỗi lần: điểm cùng phút được thay chứ không thêm, biểu đồ vẫn 60 điểm.
  Không thêm một điểm cho mỗi lần poll, vì 60 phút sẽ thành hàng trăm điểm, độ phân giải không khớp
  phần lịch sử.
- **Cửa sổ trượt**: bỏ điểm có `timestamp` ≤ điểm mới nhất − 3600.
- **Khoảng hở**: vẽ theo `timestamp` trên trục thời gian; hai điểm cách nhau > 2 phút thì ngắt đường
  (hoặc để trống) thay vì nối thẳng.
- **Chỉ dùng `systemStatisticHistory` khi `hasHistoryRecords = true`.** Các trường hợp dưới đây với
  server không hỗ trợ lịch sử thì chỉ tiếp tục poll, phần đã lỡ để trống.
- **Gọi lại `systemStatisticHistory`** (rồi tiếp tục poll) khi:
  - tab bị ẩn rồi hiện lại (`visibilitychange`): khi ẩn thì dừng `setInterval`, khi hiện thì
    `loadHistory()` để lấp phần đã lỡ;
  - poll lỗi liên tiếp (mất mạng, server restart) rồi hồi phục;
  - người dùng chuyển sang MediaServer khác: xoá `points`, tải lại lịch sử của node mới.
- **Đổi khoảng xem** (ngày/tuần/tháng, cần `hasHistoryRecords = true`) không dùng luồng này: gọi `systemStatisticHistory` với khoảng
  tương ứng và `bucket_sec` lớn hơn (vd 300 s cho 1 ngày, 3600 s cho 1 tuần), không append.

## 6. Giới hạn

- Điểm append lấy giá trị tại lúc FE gọi, còn điểm lịch sử lấy tại lúc server ghi mẫu; cả hai đều là
  giá trị tức thời nên cùng bản chất, nhưng lệch nhau tối đa vài chục giây trong cùng phút.
- Nếu server cấu hình `globalmonitor.metrics_sample_interval_sec` khác 60, giữ `bucket_sec=60` vẫn
  đúng với chu kỳ lớn hơn 60 s (phút không có mẫu sẽ hở); chu kỳ nhỏ hơn 60 s sẽ được gộp trung bình.
- `systemStatistic.disks` và lịch sử dùng cùng danh sách mount, nên `hdd_usage_pct` khớp nhau; nếu
  danh sách mount đổi (gắn/tháo ổ) giữa chừng, đường `hdd_usage_pct` có thể nhảy bậc.
