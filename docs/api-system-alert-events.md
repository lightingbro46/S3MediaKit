# Sự kiện cảnh báo hệ thống (webhook `system-event`)

MediaServer gửi cảnh báo tài nguyên (CPU, RAM, số người xem, dung lượng ổ, nghẽn IO, không ghi được ổ) lên
service API qua **một** webhook chung. Tài liệu này liệt kê mọi `eventCode`, body của từng loại, thời điểm
MediaServer gửi và cách phía API nên xử lý.

Các mục đánh dấu **MỚI** hoặc **ĐỔI** là thay đổi của đợt theo dõi IO ổ lưu trữ (nhánh `feature/disk-io-monitor`).
Tóm tắt thay đổi ở mục [Thay đổi so với bản trước](#thay-đổi-so-với-bản-trước).

## Cách gửi

```http
POST {hook.api_url}{hook.on_system_alert}
Content-Type: application/json
```

- `hook.on_system_alert` mặc định là `/api/event-rule/system-event`. Webhook chỉ gửi khi `hook.enable=1` và
  `hook.api_url` khác rỗng.
- Không có header xác thực.
- Phía API trả **bất kỳ mã 2xx** là thành công, body có thể rỗng.
- Lỗi mạng, mã khác 2xx, hoặc body không phải JSON hợp lệ: MediaServer gửi lại `hook.retry` lần, mỗi lần cách
  `hook.retry_delay` giây. **Mặc định `hook.retry=0`, tức không gửi lại**: sự kiện bị mất nếu API không nhận được.
- Timeout mỗi request: `hook.timeoutSec` (mặc định 10 s).
- Các sự kiện được gửi bất đồng bộ và độc lập với nhau, **không đảm bảo thứ tự đến**. Khi có retry, một
  `*Warning` gửi lại có thể đến sau `*Recovered`. Hãy sắp theo `eventTime` (xem
  [Gợi ý xử lý phía API](#gợi-ý-xử-lý-phía-api)).

## Trường chung

Mọi body đều có:

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `eventCode` | string | Loại sự kiện, xem bảng dưới |
| `eventTime` | integer | Unix time (giây) lúc MediaServer phát hiện |
| `mediaServerId` | string | Id của MediaServer gửi sự kiện (`general.mediaServerId`) |
| `hook_index` | integer | Bộ đếm tăng dần của mọi webhook trong một lần chạy, về 0 khi khởi động lại. Chỉ dùng để dò log, **không** dùng làm khoá chống trùng |

## Danh sách eventCode

Mỗi nhóm có một **khoá nhận diện**. Một cảnh báo đang mở được xác định bằng `mediaServerId` + nhóm + khoá.
Sự kiện hồi phục đóng cảnh báo có cùng khoá.

| Nhóm | Mở / nâng mức | Đóng (hồi phục) | Khoá nhận diện | Ghi chú |
|---|---|---|---|---|
| CPU | `CpuWarning`, `CpuCritical` | `CpuRecovered` **MỚI** | — | |
| RAM | `RamWarning`, `RamCritical` | `RamRecovered` **MỚI** | — | |
| Tổng người xem | `LiveStreamLimitExceeded` | `LiveStreamLimitRecovered` **MỚI** | — | Một mã cho cả hai mức, phân biệt bằng `threshold` |
| Người xem theo camera | `CameraStreamLimitExceeded` | `CameraStreamLimitRecovered` **MỚI** | `cameraId` | |
| Dung lượng ổ | `DiskWarning`, `DiskCritical` (**ĐỔI**) | — (không có) | `mountPoint` | |
| Nghẽn IO ổ | `DiskIoWarning`, `DiskIoCritical` **MỚI** | `DiskIoRecovered` **MỚI** | `mountPoint` (rỗng = toàn máy) | |
| Không ghi được ổ | `DiskReadOnly`, `DiskWriteFailed` **MỚI** | `DiskWritable` **MỚI** | `path` | |

Không có sự kiện nào cho mạng (`NetWork*`), vì MediaServer không đặt ngưỡng cho mạng.

## Khi nào MediaServer gửi

Áp dụng cho mọi nhóm, trừ khi ghi chú khác:

1. **Chỉ gửi khi đổi mức** (bình thường → warning → critical, hoặc giảm lại). Không gửi lại sau mỗi lần đo.
2. **Nhắc lại** chính sự kiện đó sau mỗi chu kỳ nhắc lại (mặc định 300 s), chừng nào tình trạng còn kéo dài.
   Bản nhắc lại có cùng `eventCode` với bản đầu, `eventTime` và `currentValue` là giá trị mới.
3. **Hồi phục:** khi trở về bình thường, MediaServer gửi đúng một sự kiện `*Recovered` / `DiskWritable`.
   Nhóm dung lượng ổ không có sự kiện hồi phục.
4. **Mức nặng giảm xuống mức nhẹ** (critical → warning) gửi sự kiện của mức nhẹ, ví dụ `CpuWarning`.

Độ trễ trước khi báo:

| Nhóm | Chu kỳ đo | Phải vượt liên tiếp | Báo sau khoảng |
|---|---|---|---|
| CPU, RAM | 10 s | 3 mẫu | ~30 s |
| Nghẽn IO | 5 s | 3 mẫu | ~15 s |
| Người xem (tổng, theo camera) | theo sự kiện mở/đóng player | 1 | ngay |
| Dung lượng ổ | 10 s | 1 | ≤ 10 s |
| Không ghi được ổ | 10 s | 1 | ≤ 10 s |

Nếu các mẫu liên tiếp lẫn warning và critical (ví dụ CPU 95 %, 85 %, 95 % với ngưỡng 80/90), MediaServer báo
mức nhẹ hơn là `CpuWarning`.

Trạng thái chỉ nằm trong bộ nhớ của MediaServer, nên sau khi MediaServer khởi động lại:

- tình trạng vẫn còn sẽ được báo lại như cảnh báo mới;
- tình trạng đã hết trong lúc MediaServer dừng sẽ **không bao giờ** có sự kiện hồi phục.

## Chi tiết từng nhóm

Trong các ví dụ dưới đây, trường chung `mediaServerId` và `hook_index` được lược bớt.

### CPU, RAM

```json
{"eventCode": "CpuCritical", "eventTime": 1759300000, "currentValue": "93.10", "threshold": "90.00"}
{"eventCode": "CpuRecovered", "eventTime": 1759300420, "currentValue": "41.70", "threshold": "90.00"}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `currentValue` | string, 2 chữ số thập phân | % sử dụng của toàn máy |
| `threshold` | string | Ngưỡng bị vượt. Với `*Recovered`: ngưỡng của cảnh báo mà nó đóng |

Ngưỡng lấy từ `thresholdConfig.CPU_levelLow/CPU_levelMedium` và `RAM_levelLow/RAM_levelMedium` mà API quản lý
trả cho MediaServer. Nếu thiếu thì dùng mặc định 80/90.

### Tổng người xem

```json
{"eventCode": "LiveStreamLimitExceeded", "eventTime": 1759300000, "currentValue": "120.00", "threshold": "110.00"}
{"eventCode": "LiveStreamLimitRecovered", "eventTime": 1759300900, "currentValue": "80.00", "threshold": "110.00"}
```

`currentValue` là tổng số người xem live và replay trên MediaServer, kiểu string. Mã `LiveStreamLimitExceeded`
dùng cho cả hai mức:

- mức warning: `threshold` bằng giới hạn luồng;
- mức critical: `threshold` bằng giới hạn × 1,1.

Vì vậy khi số người xem tăng qua cả hai ngưỡng, API nhận **hai** `LiveStreamLimitExceeded` có `threshold` khác nhau.

### Người xem theo camera

```json
{"eventCode": "CameraStreamLimitExceeded", "eventTime": 1759300000, "currentValue": 5, "threshold": 5, "cameraId": "cam-01"}
{"eventCode": "CameraStreamLimitRecovered", "eventTime": 1759300300, "currentValue": 2, "threshold": 5, "cameraId": "cam-01"}
```

Riêng nhóm này `currentValue` và `threshold` là **số nguyên**, không phải string.

### Dung lượng ổ — ĐỔI

```json
{"eventCode": "DiskCritical", "eventTime": 1759300000, "currentValue": "91.20", "threshold": "90.00",
 "mountPoint": "/data", "device": "/dev/sdb1"}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `currentValue` | string | % dung lượng đã dùng của mount |
| `threshold` | string | Ngưỡng bị vượt (`thresholdConfig.STORAGE_levelLow/STORAGE_levelMedium`, mặc định 80/90) |
| `mountPoint` | string | **MỚI.** Mount point bị đầy |
| `device` | string | **MỚI.** Thiết bị của mount, ví dụ `/dev/sdb1` hoặc `nas:/export` |

Nhóm này áp dụng cho mọi mount mà MediaServer thấy (bỏ qua các mount hệ thống như `/boot`, `/run`, `/var`),
không chỉ ổ lưu trữ. Không có sự kiện hồi phục: phía API tự đóng cảnh báo khi quá lâu không nhận được bản
nhắc lại (xem [Gợi ý xử lý phía API](#gợi-ý-xử-lý-phía-api)).

### Nghẽn IO ổ — MỚI

Chỉ theo dõi các ổ lưu trữ: ổ chứa thư mục ghi hình, ổ chứa database và ổ của các pool tier.
Ổ mạng (NFS/CIFS) không có số liệu IO.

```json
{"eventCode": "DiskIoCritical", "eventTime": 1759300000, "currentValue": "310.50", "threshold": "200.00",
 "mountPoint": "/data", "device": "/dev/sdb1", "metric": "await", "isMainStorage": true}

{"eventCode": "DiskIoWarning", "eventTime": 1759300000, "currentValue": "35.00", "threshold": "20.00",
 "mountPoint": "", "device": "", "metric": "iowait", "isMainStorage": false}

{"eventCode": "DiskIoRecovered", "eventTime": 1759300600, "currentValue": "12.00", "threshold": "50.00",
 "mountPoint": "/data", "device": "/dev/sdb1", "metric": "await", "isMainStorage": true}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `currentValue` | string | Giá trị của `metric` |
| `threshold` | string | Ngưỡng bị vượt của `metric`. Với `DiskIoRecovered`: ngưỡng warning của `metric` |
| `metric` | string | Chỉ số được báo, xem bảng dưới |
| `mountPoint` | string | Mount bị nghẽn. **Rỗng** khi là cảnh báo toàn máy (`metric` là `iowait` hoặc `psi`) |
| `device` | string | Thiết bị của mount, rỗng với cảnh báo toàn máy |
| `isMainStorage` | bool | `true` nếu mount chứa thư mục ghi hình (nghẽn ở đây gây mất bản ghi) |

| `metric` | Đơn vị | Ý nghĩa | Ngưỡng mặc định warning / critical |
|---|---|---|---|
| `util` | % | Tỉ lệ thời gian thiết bị bận | 80 / 95 |
| `await` | ms | Thời gian trung bình mỗi lần đọc/ghi | 50 / 200 |
| `iowait` | % | Tỉ lệ CPU chờ IO của toàn máy (kernel không có PSI, ví dụ CentOS 7) | 20 / 40 |
| `psi` | % | Tỉ lệ thời gian mọi tác vụ cùng bị chặn chờ IO (PSI `full avg10`, kernel ≥ 4.20) | 10 / 30 |

Quy tắc mức của một mount:

- **warning** khi `util` **hoặc** `await` vượt ngưỡng warning;
- **critical** chỉ khi `util` **và** `await` cùng vượt ngưỡng critical. `util` 100 % mà mỗi IO vẫn nhanh (SSD,
  RAID nhiều hàng đợi) chỉ là warning;
- `metric` là `await` nếu độ trễ đã vượt ngưỡng của mức được báo, ngược lại là `util`. `DiskIoRecovered` dùng lại
  `metric` của cảnh báo mà nó đóng.

Cảnh báo toàn máy dùng `psi` nếu kernel có, không thì dùng `iowait`. Một máy có thể cùng lúc có cảnh báo
toàn máy (`mountPoint` rỗng) và cảnh báo cho từng mount; các cảnh báo này độc lập với nhau.

### Không ghi được ổ — MỚI

MediaServer kiểm tra mỗi 10 s các thư mục nó phải ghi: thư mục ghi hình, thư mục database và base path của
từng pool tier (kể cả NAS). Cách kiểm tra: xem mount có bị gắn `ro` không, rồi ghi thử một file nhỏ, fsync và xoá.

```json
{"eventCode": "DiskReadOnly", "eventTime": 1759300000, "path": "/opt/media/www/record",
 "mountPoint": "/opt/media", "device": "/dev/sdb1", "isMainStorage": true, "error": "Read-only file system"}

{"eventCode": "DiskWriteFailed", "eventTime": 1759300000, "path": "/mnt/nas/pool1",
 "mountPoint": "/mnt/nas", "device": "nas:/export", "isMainStorage": false, "error": "Input/output error"}

{"eventCode": "DiskWritable", "eventTime": 1759300120, "path": "/opt/media/www/record",
 "mountPoint": "/opt/media", "device": "/dev/sdb1", "isMainStorage": true, "error": ""}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `path` | string | Thư mục không ghi được (khoá nhận diện) |
| `mountPoint`, `device` | string | Mount chứa `path`; rỗng nếu MediaServer không xác định được |
| `isMainStorage` | bool | `true` với thư mục ghi hình |
| `error` | string | Mô tả lỗi của hệ điều hành; rỗng với `DiskWritable` |

| eventCode | Khi nào | `error` thường gặp |
|---|---|---|
| `DiskReadOnly` | Mount bị gắn lại `ro` (ví dụ ext4 `errors=remount-ro` sau lỗi đĩa) hoặc ghi trả lỗi read-only | `Read-only file system` |
| `DiskWriteFailed` | Ghi lỗi vì lý do khác | `Input/output error`, `Permission denied`, `No space left on device`, `Stale file handle` |
| `DiskWritable` | Ghi lại được sau một trong hai lỗi trên | rỗng |

Một thư mục ghi được ngay từ lúc MediaServer khởi động thì không có sự kiện nào. Thư mục hỏng ngay từ lúc
khởi động thì được báo.

`DiskReadOnly` hoặc `DiskWriteFailed` với `isMainStorage=true` nghĩa là MediaServer **đang không ghi được hình**.
Nên xếp vào mức nghiêm trọng cao nhất.

## Gợi ý xử lý phía API

- **Trả 2xx cho mọi `eventCode`, kể cả mã không nhận ra.** Mã khác 2xx chỉ khiến MediaServer gửi lại (nếu có
  bật retry) chứ không giúp gì.
- **Giữ một cảnh báo đang mở cho mỗi khoá** (`mediaServerId` + nhóm + khoá nhận diện):
  - sự kiện mở hoặc nâng mức: tạo cảnh báo nếu chưa có, nếu có rồi thì cập nhật mức, `currentValue` và
    thời điểm thấy gần nhất. Bản nhắc lại mỗi 300 s cũng xử lý như vậy, **không tạo cảnh báo mới**;
  - sự kiện hồi phục (`*Recovered`, `DiskWritable`): đóng cảnh báo cùng khoá;
  - bỏ qua sự kiện có `eventTime` cũ hơn sự kiện mới nhất đã nhận cho cùng khoá (thứ tự đến không đảm bảo).
- **Tự đóng cảnh báo cũ.** Cảnh báo còn hiệu lực sẽ được nhắc lại mỗi 300 s. Nếu quá khoảng 15 phút không
  nhận được nhắc lại thì có thể coi là đã hết. Cách này bù cho các trường hợp không có sự kiện hồi phục:
  nhóm dung lượng ổ, MediaServer khởi động lại, hoặc sự kiện bị mất vì không retry.
- Với nhóm nghẽn IO, nên coi `mountPoint` rỗng là cảnh báo của cả máy chủ, hiển thị riêng với cảnh báo của
  từng ổ.

## Cấu hình phía MediaServer

Ngưỡng CPU, RAM, dung lượng ổ và giới hạn người xem vẫn lấy từ API quản lý như trước. Các khoá dưới đây chỉ
đặt trong config của MediaServer, API quản lý không đổi được:

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `globalmonitor.system_alert_sustain_samples` | 3 | Số mẫu CPU/RAM liên tiếp trước khi báo |
| `globalmonitor.system_alert_repeat_sec` | 300 | Chu kỳ nhắc lại CPU, RAM, người xem (≤ 0: không nhắc) |
| `globalmonitor.hdd_alert_repeat_sec` | 300 | Chu kỳ nhắc lại dung lượng ổ và không ghi được ổ |
| `globalmonitor.disk_io_sample_interval_sec` | 5 | Chu kỳ đo IO |
| `globalmonitor.disk_io_util_warning_threshold` / `_critical_threshold` | 80 / 95 | Ngưỡng `util` |
| `globalmonitor.disk_io_await_warning_ms` / `disk_io_await_critical_ms` | 50 / 200 | Ngưỡng `await` |
| `globalmonitor.iowait_warning_threshold` / `iowait_critical_threshold` | 20 / 40 | Ngưỡng `iowait` |
| `globalmonitor.disk_io_psi_full_warning_threshold` / `_critical_threshold` | 10 / 30 | Ngưỡng `psi` |
| `globalmonitor.disk_io_sustain_samples` | 3 | Số mẫu IO liên tiếp trước khi báo |
| `globalmonitor.disk_io_alert_repeat_sec` | 300 | Chu kỳ nhắc lại nghẽn IO |
| `globalmonitor.mount_refresh_interval_seconds` | 10 | Chu kỳ đo dung lượng ổ và kiểm tra ghi |

## Thay đổi so với bản trước

| Thay đổi | Ảnh hưởng tới phía API |
|---|---|
| CPU, RAM không còn gửi mỗi 10 s khi còn vượt ngưỡng; chỉ gửi khi đổi mức, nhắc lại mỗi 300 s, và phải vượt 3 mẫu (~30 s) mới báo | Ít sự kiện hơn hẳn; cảnh báo đến chậm hơn khoảng 30 s |
| Người xem (tổng và theo camera) không còn gửi mỗi khi có người mở/đóng player | Ít sự kiện hơn hẳn |
| Dung lượng ổ không còn gửi mỗi 10 s cho mọi mount vượt ngưỡng; body thêm `mountPoint`, `device` | `eventCode` giữ nguyên; nên dùng `mountPoint` làm khoá |
| Mã mới: `CpuRecovered`, `RamRecovered`, `LiveStreamLimitRecovered`, `CameraStreamLimitRecovered` | Cần nhận để đóng cảnh báo |
| Mã mới: `DiskIoWarning`, `DiskIoCritical`, `DiskIoRecovered` | Nhóm cảnh báo mới |
| Mã mới: `DiskReadOnly`, `DiskWriteFailed`, `DiskWritable` | Nhóm cảnh báo mới, mức nghiêm trọng cao khi `isMainStorage=true` |
