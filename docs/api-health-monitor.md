# API giám sát sức khỏe MediaServer (`/media/monitor/*`)

Hợp đồng cho app desktop monitor (vận hành/NOC). App gọi định kỳ tới **từng** MediaServer; mỗi
node tự đánh giá trạng thái và trả snapshot đã tính sẵn, nên gọi dồn không làm nặng server.

Trạng thái triển khai: nhóm `host`, `process`, `storage` (đợt H1). Các nhóm `ingest`,
`recording`, `delivery`, `cluster` trả `"unknown"` cho tới khi có số liệu.

## 1. Xác thực

- Mọi API `/media/monitor/*`, đọc lẫn cấu hình, dùng **`api.secret`** (cùng khoá với `/index/api/*`),
  không cần JWT. Không có khoá riêng cho monitor.
- Gửi khoá qua header HTTP **`secret: <api.secret>`**. Server cũng nhận tham số `secret` trong
  query/body, nhưng không nên đưa khoá lên URL (lộ trong log proxy).
- IP của app phải nằm trong `http.allow_ip_range` (rỗng = mọi IP). Cấu hình này áp chung cho mọi
  API dùng `api.secret`, nên dải IP phải gồm cả các hệ thống tích hợp khác đang gọi các API đó.
- Lỗi: thiếu khoá → `code=-300`; sai khoá → `code=-100 "Incorrect secret"`; IP không được phép →
  `code=-100 "Your ip is not allowed to access the service."` (HTTP 401).
- `api.secret` là khoá quản trị: app lưu mã hoá (keychain/DPAPI), không ghi thô xuống đĩa, không in
  ra log; nên gọi qua HTTPS.
- `data.apiSecretIsDefault = true` khi node vẫn dùng giá trị `api.secret` mặc định trong code: app
  nên nhắc người vận hành đổi khoá (sửa file cấu hình, API không đổi được khoá này).

## 2. Mức và luật gộp

| Mức | Nghĩa |
|---|---|
| `ok` | bình thường |
| `warning` | vượt ngưỡng cảnh báo |
| `critical` | vượt ngưỡng nghiêm trọng |
| `unknown` | chưa đủ mẫu (vừa khởi động) hoặc chưa có số liệu; không tính vào tổng |
| `disabled` | (chỉ ở `components`) thành phần bị tắt khi build |

- Mỗi chỉ số chỉ đổi mức sau `monitor.sustainSamples` mẫu liên tiếp (mặc định 3 × 10 s), cả lúc xấu
  đi và lúc hồi phục. Chỉ số đã có hysteresis riêng (nghẽn IO đĩa) hoặc dạng sự kiện (ổ không ghi
  được, GPU fallback) đổi mức ngay.
- Mức thành phần = mức xấu nhất của các chỉ số trong nó.
- `overall` = mức xấu nhất của các thành phần, trừ `ingest` và `delivery` (lỗi nằm ở camera/người xem
  bên ngoài) chỉ đẩy `overall` lên tối đa `warning`.
- `score` = 100 − 10 × (số thành phần `warning`) − 30 × (số thành phần `critical`), không âm. Chỉ để
  vẽ xu hướng, không dùng để báo động.
- HTTP luôn 200 khi xác thực qua; trạng thái đọc ở `data.overall`.

| Thành phần | Gồm |
|---|---|
| `host` | CPU, load, RAM, swap, NIC, file descriptor |
| `process` | độ trễ/tải poller, hàng đợi pool blocking, GPU fallback |
| `storage` | dung lượng, nghẽn IO, khả năng ghi của các ổ chứa bản ghi / DB / pool tier |
| `ingest` | camera / luồng vào (chưa có) |
| `recording` | ghi hình (chưa có) |
| `delivery` | người xem / luồng ra (chưa có) |
| `cluster` | sync, cụm, tích hợp (chưa có) |

## 3. Endpoint

Tất cả là GET (trừ `config/set`), envelope `{"code":0,"data":{...}}`, mọi `data` có `schemaVersion: 1`.
Thêm field mới không đổi `schemaVersion`; app phải bỏ qua field lạ.

| Endpoint | Chu kỳ gợi ý | Nội dung |
|---|---|---|
| `/media/monitor/health` | 5–10 s | trạng thái tổng, thành phần, lý do |
| `/media/monitor/summary` | 15–30 s | như `health` + toàn bộ số liệu theo nhóm |
| `/media/monitor/config/get` | khi mở tab cấu hình | ngưỡng và tham số hiện hành |
| `/media/monitor/config/set` | khi bấm lưu | POST JSON, đổi ngưỡng/tham số |

Snapshot được tính mỗi `monitor.intervalSec` (mặc định 10 s); gọi nhanh hơn chỉ nhận lại cùng
snapshot.

### 3.1 `/media/monitor/health`

Tham số tuỳ chọn `rev`: nếu bằng `rev` hiện tại (mức và lý do không đổi) server trả
`{"code":0,"notModified":true}` không kèm `data`.

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1,
    "enabled": true,
    "mediaServerId": "ms-01",
    "generatedAt": 1760500000,
    "rev": 12,
    "overall": "warning",
    "score": 90,
    "uptimeSec": 86400,
    "version": {"buildTime": "2026-10-16T08:50:45", "branchName": "develop", "commitHash": "f1284c3"},
    "apiSecretIsDefault": false,
    "components": {
      "host": "warning", "process": "ok", "storage": "ok",
      "ingest": "unknown", "recording": "unknown", "delivery": "unknown", "cluster": "unknown"
    },
    "reasons": [
      {"component": "host", "metric": "cpu.usage_pct", "level": "warning",
       "value": 84.2, "threshold": 80, "since": 1760499700}
    ]
  }
}
```

- `reasons[]`: mỗi chỉ số đang `warning`/`critical`, xấu nhất trước. `threshold` là ngưỡng đã vượt,
  `since` là lúc mức hiện tại bắt đầu (unix giây), `detail` (nếu có) chỉ thực thể: tên NIC, mount
  point, đường dẫn thư mục, tên pool.
- `rev` tăng khi `overall`, mức một thành phần, hoặc tập lý do đổi; giá trị số đổi thì không tăng.
- Monitor tắt (`monitor.enable=0`): `data = {"schemaVersion":1,"enabled":false,"overall":"unknown",...}`.

### 3.2 `/media/monitor/summary`

`data` gồm các field của `health` (trừ `uptimeSec`, `version` nằm trong `process`/top-level) và:

| Nhóm | Field |
|---|---|
| `host.cpu` | `cores`, `usage_pct` (máy), `proc_usage_pct`, `load1`, `load1_per_core` |
| `host.ram` | `total`, `used`, `usage_pct`, `proc_rss` (byte), `proc_usage_pct` |
| `host.swap` | `total`, `used`, `used_pct` |
| `host.nets[]` | `name`, `rx_mbps`, `tx_mbps`, `speed_mbps`, `util_pct` (null khi NIC ảo không báo tốc độ), `errors_per_sec`, `drops_per_sec` |
| `host.fd` | `used`, `limit`, `used_pct` |
| `host.threads` | số thread của tiến trình |
| `process` | `uptimeSec`, `startedAt` |
| `process.pollers`, `process.work_pollers` | `count`, `delay_max_ms`, `delay_avg_ms`, `load_max` (%), `sampled_age_ms` |
| `process.blocking[]` | theo pool: `name`, `threads`, `queued`, `running`, `completed`, `window_tasks`, `wait_avg_ms`, `wait_max_ms`, `busy_pct` (cửa sổ = 1 chu kỳ) |
| `transcode` | `streams`, `decoders`, `encoders`, `motion`, `mjpeg_ingest` |
| `gpu` | `backends`, `load`, `decoders`, `encoders`, `fallbacks` (từ lúc khởi động), `last_fallback_at` |
| `storage.disks[]` | chỉ các mount chứa bản ghi / DB / pool: `mount`, `device`, `total_bytes`, `used_bytes`, `free_bytes`, `usage_pct`, `read_only`, `io_util_pct`, `await_ms`, `read_mbps`, `write_mbps`, `io_state` |
| `storage.disk_io` | `valid`, `iowait_pct`, `psi_io_full_pct`, `state` (`normal`/`warning`/`critical`) |
| `storage.storage_write[]` | kết quả thử ghi từng thư mục (như `storage_write` của `systemStatistic`) |

Số không đo được trả `null`.

Poller không trả lời lệnh đo độ trễ sau 1 s (bị kẹt trong một tác vụ) thì `delay_max_ms` là thời gian
đã chờ, nên một poller treo hẳn vẫn hiện `critical`.

### 3.3 Chỉ số và ngưỡng mặc định

| Chỉ số (`metric`) | Thành phần | Warn / Crit | Khoá cấu hình |
|---|---|---|---|
| `cpu.usage_pct` | host | 80 / 90 | `globalmonitor.cpu_warning_threshold` / `cpu_critical_threshold` (VMS cũng đặt qua cấu hình server) |
| `cpu.load1_per_core` | host | 1.5 / 3 | `monitor.load1PerCoreWarn` / `Crit` |
| `ram.usage_pct` | host | 80 / 90 | `globalmonitor.memory_warning_threshold` / `memory_critical_threshold` |
| `swap.used_pct` | host | 20 / 50 | `monitor.swapUsedPctWarn` / `Crit` |
| `net.util_pct` | host | 70 / 90 | `monitor.netUtilPctWarn` / `Crit` |
| `net.errors_per_sec` | host | 0.01 / 100 | `monitor.netErrorsPerSecWarn` / `Crit` |
| `net.drops_per_sec` | host | 10 / 100 | `monitor.netDropsPerSecWarn` / `Crit` |
| `proc.fd_used_pct` | host | 70 / 90 | `monitor.fdUsedPctWarn` / `Crit` |
| `pollers.delay_max_ms` | process | 50 / 200 | `monitor.pollerDelayMsWarn` / `Crit` |
| `pollers.load_max` | process | 80 / 95 | `monitor.pollerLoadPctWarn` / `Crit` |
| `work_pollers.delay_max_ms` | process | 100 / 500 | `monitor.workPollerDelayMsWarn` / `Crit` |
| `blocking.wait_max_ms` (theo pool) | process | 1000 / 5000 | `monitor.blockingWaitMsWarn` / `Crit` |
| `blocking.busy_pct` (theo pool) | process | 80 / 95 | `monitor.blockingBusyPctWarn` / `Crit` |
| `gpu.fallbacks` | process | warning trong `monitor.gpuFallbackWindowSec` (300 s) sau mỗi lần GPU rơi về CPU | |
| `disk.usage_pct` (theo mount) | storage | 80 / 90 | `globalmonitor.hdd_warning_threshold` / `hdd_critical_threshold` |
| `disk_io.mount`, `disk_io.host` | storage | theo `globalmonitor.disk_io_*`, `iowait_*` | mức đã sustain của DiskIoMonitor |
| `storage_write.writable` (theo thư mục) | storage | không ghi được = critical | |

Ngưỡng là `>=`. Drop gói ở NIC ảo/multicast thường khác 0 nên ngưỡng `net.drops_per_sec` rộng hơn lỗi.

## 4. Cấu hình

### 4.1 `/media/monitor/config/get`

```json
{
  "code": 0,
  "data": {
    "schemaVersion": 1,
    "values": {"monitor.intervalSec": "10", "monitor.pollerDelayMsWarn": "50", "globalmonitor.cpu_warning_threshold": "80", "...": "..."},
    "effective": {"cpu": {"warn": 80, "crit": 90}, "ram": {...}, "disk": {...}, "reader": {"warn": -1, "crit": -1}},
    "restartRequired": ["monitor.enable", "monitor.intervalSec"],
    "apiSecretIsDefault": false
  }
}
```

- `values`: mọi khoá sửa được, giá trị trong file cấu hình.
- `effective`: ngưỡng CPU/RAM/disk/reader đang áp dụng. VMS cũng đặt CPU/RAM/disk khi đẩy cấu hình
  server (lần đẩy sau ghi đè giá trị đặt từ app), reader lấy theo license (`-1` = không giới hạn) và
  không sửa được qua API này.
- `restartRequired`: khoá chỉ có hiệu lực sau khi khởi động lại MediaServer. Các khoá khác có hiệu
  lực ở chu kỳ đánh giá kế tiếp; trạng thái hysteresis đang tính được giữ nguyên.

### 4.2 `/media/monitor/config/set`

`POST`, `Content-Type: application/json`, body phẳng `{"<khoá>": <giá trị>, ...}`; header `secret`
(hoặc field `secret` trong body).

```json
{"monitor.pollerDelayMsWarn": 80, "globalmonitor.cpu_warning_threshold": 75}
```

- Chỉ nhận khoá có trong `values` của `config/get`; khoá khác (kể cả `api.secret`) →
  `code=902001` HTTP 400, không đổi gì.
- Giá trị phải là số ≥ 0; khoá đếm (`monitor.enable` 0/1, `monitor.intervalSec` ≥ 1,
  `monitor.sustainSamples` ≥ 1, `monitor.gpuFallbackWindowSec` ≥ 0,
  `globalmonitor.disk_io_sustain_samples` ≥ 1) phải là số nguyên.
- Mỗi cặp warn/crit phải có warn < crit (so với giá trị mới hoặc giá trị đang lưu).
- Thành công: `{"code":0,"changed":<số khoá đổi>,"data":<như config/get>}`; ghi file cấu hình.
- Request không phải POST → `code=902001`.

### 4.3 Khoá trong file cấu hình

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `monitor.enable` | 1 | 0 tắt đánh giá (API trả `enabled:false`) |
| `monitor.intervalSec` | 10 | chu kỳ đánh giá (giây) |
| `monitor.sustainSamples` | 3 | số mẫu liên tiếp trước khi đổi mức |
| `monitor.gpuFallbackWindowSec` | 300 | thời gian giữ cảnh báo sau một lần GPU fallback |
| `monitor.<tên>Warn` / `monitor.<tên>Crit` | xem bảng 3.3 | ngưỡng |

## 5. Ảnh hưởng tới API cũ

`/index/api/getBlockingExecutors`: khi monitor chạy, các field `window_*` là của chu kỳ đánh giá gần
nhất (`monitor.intervalSec`), không còn là "từ lần gọi trước", vì `BlockingExecutors::stats()` chỉ
cho một người gọi định kỳ.
