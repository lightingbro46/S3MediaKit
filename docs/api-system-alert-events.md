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
| Throughput pool tier | `PoolThroughputLow` **MỚI** | `PoolThroughputRecovered` **MỚI** | `poolId` | Chỉ khi bật tier storage |
| Dung lượng giữ bản ghi | `RetentionCapacityLow` **MỚI** | `RetentionCapacityRecovered` **MỚI** | `scope` + `poolId` | Thư mục record (tắt tier) hoặc pool HOT (bật tier); mức trong `level` |

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
| Throughput pool tier | mỗi chu kỳ tiering (300 s) | 3 lần | ~15 phút |
| Dung lượng giữ bản ghi | 300 s (chu kỳ dọn record hoặc chu kỳ tiering) | 3 lần | ~15 phút, sau ≥ 15 phút đo tốc độ ghi |

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

### Throughput pool tier — MỚI

So tốc độ MediaServer chuyển bản ghi vào một pool tier với tốc độ dữ liệu phải chảy vào pool đó. Nếu chuyển
chậm hơn thì hàng đợi chuyển tầng lớn dần, pool nguồn (thường là HOT) đầy lên và cuối cùng phải xoá bản ghi
hoặc dừng ghi hình.

```json
{"eventCode": "PoolThroughputLow", "eventTime": 1759300000, "currentValue": "8.40", "threshold": "14.40",
 "demandMbps": "12.00", "poolId": "pool-warm-01", "poolName": "NAS WARM", "tier": "WARM",
 "backlogBytes": 52428800000, "backlogEtaSec": 5952}

{"eventCode": "PoolThroughputRecovered", "eventTime": 1759304500, "currentValue": "21.30", "threshold": "14.40",
 "demandMbps": "12.00", "poolId": "pool-warm-01", "poolName": "NAS WARM", "tier": "WARM",
 "backlogBytes": 0, "backlogEtaSec": 0}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `poolId` | string | Pool đích của việc chuyển tầng (khoá nhận diện) |
| `poolName`, `tier` | string | Tên và tầng (`WARM`/`COLD`) của pool |
| `currentValue` | string | Tốc độ chuyển vào pool đo được, MiB/s |
| `threshold` | string | Tốc độ cần có = `demandMbps` × hệ số an toàn (mặc định 1,2), MiB/s |
| `demandMbps` | string | Tổng tốc độ ghi hình của các camera có policy chuyển dữ liệu vào pool, MiB/s |
| `backlogBytes` | number | Byte của các job chuyển tầng vào pool còn chờ/đang chạy từ chu kỳ trước (đo trước khi chu kỳ này queue thêm) |
| `backlogEtaSec` | number | Ước lượng số giây để chuyển hết `backlogBytes` với tốc độ hiện tại; `-1` nếu chưa đo được |

Cách đo (cửa sổ trượt 1 giờ):

- **Tốc độ chuyển** = byte đã copy/upload vào pool ÷ thời gian có ít nhất một lượt copy/upload vào pool đang
  chạy. Nhiều job chạy song song chỉ tính thời gian một lần; lúc pool rảnh không làm giảm tốc độ. Lượt chuyển
  lỗi vẫn tính thời gian nhưng không tính byte. Cần ít nhất 60 s chuyển trong cửa sổ mới có số đo.
- **Nhu cầu** lấy từ dung lượng segment mới ghi của từng camera, cộng cho mọi pool mà policy của camera chuyển
  dữ liệu tới (chuỗi HOT → WARM → COLD tính camera cho cả WARM và COLD). Cần ít nhất 15 phút kể từ khi
  MediaServer khởi động.
- Chỉ báo `PoolThroughputLow` khi việc của chu kỳ trước (5 phút) vẫn chưa chuyển xong. Pool chậm nhưng đã
  chuyển hết việc được giao là bình thường.

Khi pool còn backlog nhưng không có lượt chuyển nào trong cửa sổ (pool offline, tạm dừng vì gần đầy...) thì
MediaServer không kết luận được, nên giữ nguyên trạng thái và **không nhắc lại**. Trường hợp đó đã có các
cảnh báo pool khác.

Áp dụng cho mọi loại pool đích (LOCAL_DISK, NAS, MinIO). Tốc độ đo được gồm cả đọc từ pool nguồn, nên pool
nguồn chậm cũng làm giảm `currentValue`. Khi nhận cảnh báo, hướng xử lý thường là tăng
`storage.tiering_transfer_max_concurrent_per_pool`, nâng băng thông/hiệu năng pool đích hoặc giảm số camera
dùng pool đó.

### Dung lượng giữ bản ghi — MỚI

Với tốc độ ghi hình hiện tại, vùng lưu bản ghi mới (thư mục record khi **tắt** tier storage, từng pool HOT khi
**bật**) có đủ chỗ cho thời gian lưu đã cấu hình hay không. Không đủ thì bản ghi bị xoá (hoặc chuyển tầng) sớm
hơn cấu hình.

```json
{"eventCode": "RetentionCapacityLow", "eventTime": 1759300000, "level": "CRITICAL",
 "scope": "record_dir", "poolId": "", "poolName": "",
 "path": "/opt/media/www/record", "mountPoint": "/opt/media",
 "currentValue": "4.20", "threshold": "7.00", "ingestMbps": "12.50",
 "usableBytes": 4535485464576, "requiredBytes": 7558557450240, "cameras": 24, "overflowAction": ""}

{"eventCode": "RetentionCapacityLow", "eventTime": 1759300000, "level": "WARNING",
 "scope": "hot_pool", "poolId": "pool-hot-01", "poolName": "SSD HOT",
 "path": "/data/hot", "mountPoint": "",
 "currentValue": "2.10", "threshold": "3.00", "ingestMbps": "40.00",
 "usableBytes": 7610000000000, "requiredBytes": 10871635968000, "cameras": 120,
 "overflowAction": "MOVE_TO_NEXT_TIER"}
```

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| `scope` | string | `record_dir` (tắt tier) hoặc `hot_pool` (bật tier). Khoá nhận diện = `scope` + `poolId` |
| `level` | string | `WARNING`, `CRITICAL`; `NORMAL` ở sự kiện hồi phục |
| `poolId`, `poolName` | string | Pool HOT; rỗng khi `scope = record_dir` |
| `path` | string | Thư mục record, hoặc `mount_path` của pool HOT |
| `mountPoint` | string | Mount chứa thư mục record; rỗng với pool HOT |
| `currentValue` | string | Số ngày ghi hình vùng lưu chứa được ở tốc độ hiện tại = dung lượng dùng được ÷ dung lượng ghi mỗi ngày |
| `threshold` | string | Số ngày cần = `requiredBytes` ÷ dung lượng ghi mỗi ngày |
| `ingestMbps` | string | Tốc độ ghi hình vào vùng lưu, MiB/s |
| `usableBytes` | number | Dung lượng dùng được: tổng × `storage.limitPercentUsage` (thư mục record) hoặc × `high_watermark_percent` của pool (HOT) |
| `requiredBytes` | number | Σ theo camera: tốc độ ghi × thời gian phải giữ |
| `cameras` | number | Số camera đang ghi có yêu cầu thời gian lưu |
| `overflowAction` | string | Pool HOT: hành động khi đầy của policy (`MOVE_TO_NEXT_TIER`, `DELETE_OLDEST`, `STOP_RECORDING_AND_ALERT`) |

Báo khi `requiredBytes × storage.retention_forecast_safety_factor` (mặc định 1,1) > `usableBytes`.

- **Thời gian phải giữ:** thư mục record lấy `keepArchivedMinFor` của camera (bỏ camera đặt tối thiểu tự động);
  pool HOT lấy `retain_until_days` của tầng HOT trong policy hiệu lực của camera (bỏ giá trị 0).
- **Mức:** thư mục record luôn `CRITICAL` (sẽ xoá xuống dưới thời gian tối thiểu đã cam kết). Pool HOT là
  `WARNING` khi mọi camera dùng `MOVE_TO_NEXT_TIER` (dữ liệu chuyển tầng sớm, không mất), `CRITICAL` khi có
  camera dùng hành động xoá/dừng ghi.
- **Tốc độ ghi** lấy từ dung lượng các segment ghi xong trong cửa sổ `storage.pool_throughput_window_seconds`
  (mặc định 1 giờ), cần ít nhất 15 phút kể từ khi MediaServer khởi động. Chưa đủ, hoặc không biết dung lượng
  (pool object không quota, mount không tìm thấy) thì giữ nguyên trạng thái.
- **Nhắc lại** mỗi `storage.retention_forecast_alert_repeat_seconds` (mặc định 6 giờ), không phải 300 s như các
  nhóm khác, vì tình trạng này đổi chậm. Phía API **không** được tự đóng cảnh báo nhóm này sau 15 phút không
  nhận nhắc lại; nên dùng ngưỡng lớn hơn chu kỳ nhắc lại (ví dụ 7 giờ).
- Pool HOT còn có thêm một dòng `storage_alerts` loại `TIER_RETENTION_CAPACITY_LOW` (tối đa một dòng/giờ/pool cho cùng mức; đổi mức thì ghi ngay).

Số liệu hiện tại đọc qua `GET /media/mserver/systemStatistic` (và `/media/api/systemStatistic`), object
`data.record_write`:

```jsonc
// Tắt tier storage: thư mục record
"record_write": {
  "mode": "record_dir",
  "path": "/opt/media/www/record", "mount_point": "/opt/media",
  "ingest_mbps": "12.50",          // tốc độ ghi hình lúc gọi, MiB/s; null khi chưa đủ 15 phút đo
  "recording_cameras": 24,         // camera có segment trong cửa sổ đo
  "cameras": 20,                   // camera có thời gian lưu tối thiểu (tính ở lần đánh giá gần nhất)
  "usable_bytes": 0, "used_bytes": 0, "required_bytes": 0,
  "retention_days_estimate": 21.4, "estimated_remaining_days": 3.2, "required_days": 7.0,
  "state": "normal"                // "low" khi đang có RetentionCapacityLow
}
// Bật tier storage: tầng HOT
"record_write": {
  "mode": "hot_tier",
  "ingest_mbps": "40.00", "recording_cameras": 120, "state": "normal",
  "pools": [{"pool_id": "pool-hot-01", "pool_name": "SSD HOT", "ingest_mbps": "40.00",
             "recording_cameras": 120, "retention_forecast": { /* như dashboard/detail */ }}]
}
```

Các trường ngày/byte là kết quả lần đánh giá gần nhất (mỗi 300 s), `null` khi chưa có; `ingest_mbps` tính lúc gọi.

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
  nhóm dung lượng ổ, MediaServer khởi động lại, hoặc sự kiện bị mất vì không retry. Ngoại lệ: nhóm dung lượng
  giữ bản ghi nhắc lại mỗi 6 giờ, nên ngưỡng tự đóng của nhóm này phải lớn hơn 6 giờ.
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
| `storage.pool_throughput_alert_enabled` | 1 | Bật cảnh báo throughput pool tier (0: vẫn đo, không báo) |
| `storage.pool_throughput_window_seconds` | 3600 | Cửa sổ đo tốc độ chuyển và nhu cầu |
| `storage.pool_throughput_safety_factor` | 1.2 | Tốc độ chuyển phải ≥ nhu cầu × hệ số này |
| `storage.pool_throughput_min_busy_seconds` | 60 | Thời gian chuyển tối thiểu trong cửa sổ trước khi có số đo |
| `storage.pool_throughput_sustain_samples` | 3 | Số chu kỳ tiering liên tiếp trước khi báo |
| `storage.pool_throughput_alert_repeat_seconds` | 300 | Chu kỳ nhắc lại throughput pool (≤ 0: không nhắc) |
| `storage.retention_forecast_alert_enabled` | 1 | Bật cảnh báo dung lượng giữ bản ghi (0: vẫn tính, không báo) |
| `storage.retention_forecast_safety_factor` | 1.1 | Dung lượng cần × hệ số này phải ≤ dung lượng dùng được |
| `storage.retention_forecast_sustain_samples` | 3 | Số lần đánh giá (mỗi 300 s) liên tiếp trước khi báo |
| `storage.retention_forecast_alert_repeat_seconds` | 21600 | Chu kỳ nhắc lại dung lượng giữ bản ghi (≤ 0: không nhắc) |

## Thay đổi so với bản trước

| Thay đổi | Ảnh hưởng tới phía API |
|---|---|
| CPU, RAM không còn gửi mỗi 10 s khi còn vượt ngưỡng; chỉ gửi khi đổi mức, nhắc lại mỗi 300 s, và phải vượt 3 mẫu (~30 s) mới báo | Ít sự kiện hơn hẳn; cảnh báo đến chậm hơn khoảng 30 s |
| Người xem (tổng và theo camera) không còn gửi mỗi khi có người mở/đóng player | Ít sự kiện hơn hẳn |
| Dung lượng ổ không còn gửi mỗi 10 s cho mọi mount vượt ngưỡng; body thêm `mountPoint`, `device` | `eventCode` giữ nguyên; nên dùng `mountPoint` làm khoá |
| Mã mới: `CpuRecovered`, `RamRecovered`, `LiveStreamLimitRecovered`, `CameraStreamLimitRecovered` | Cần nhận để đóng cảnh báo |
| Mã mới: `DiskIoWarning`, `DiskIoCritical`, `DiskIoRecovered` | Nhóm cảnh báo mới |
| Mã mới: `DiskReadOnly`, `DiskWriteFailed`, `DiskWritable` | Nhóm cảnh báo mới, mức nghiêm trọng cao khi `isMainStorage=true` |
| Mã mới: `PoolThroughputLow`, `PoolThroughputRecovered` | Nhóm cảnh báo mới, khoá `poolId` |
| Mã mới: `RetentionCapacityLow`, `RetentionCapacityRecovered` | Nhóm cảnh báo mới, khoá `scope` + `poolId`, nhắc lại mỗi 6 giờ |
