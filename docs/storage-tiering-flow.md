# Luồng Chức Năng Lưu Trữ Đa Tầng

Tài liệu này mô tả chi tiết luồng lưu trữ đa tầng đang được triển khai trong S3MediaKit, từ cấu hình storage pool/policy, ghi nhận metadata segment, chuyển tầng, retention, playback đến restore dữ liệu COLD.

Tài liệu API chi tiết nằm ở [api-storage-tiering.md](api-storage-tiering.md).

> Tính năng chỉ được biên dịch khi CMake bật `-DENABLE_TIER_STORAGE=ON`; mặc định hiện là `ON` (`CMakeLists.txt`). Khi tắt, entity/manager tiering bị loại khỏi build, `TierStorageManager` không start và route `/media/mserver/storage/...` không được đăng ký. Migration SQL của MediaServer vẫn quét thư mục migration độc lập với macro này.
>
> **Cấu hình `OFF` không được build tự động ở đâu cả** (repo chưa có CI). Mọi thay đổi trong `manager/Local/Tier*` hoặc code có `#ifdef ENABLE_TIER_STORAGE` phải tự kiểm trước khi phát hành — xem [§16](#16-kiểm-cấu-hình-tier-off).

## 1. Mục Tiêu

Tính năng lưu trữ đa tầng cho phép hệ thống quản lý dữ liệu ghi hình theo vòng đời:

```text
Camera recording
  -> HOT local disk
  -> WARM local/NAS
  -> COLD NAS/MinIO
  -> EXPIRED / DELETED
```

Mục tiêu chính:

- Giữ dữ liệu mới ở HOT để phát nhanh.
- Tự động chuyển dữ liệu cũ hơn sang WARM/COLD theo policy.
- Lưu dài hạn trên COLD NAS hoặc MinIO; S3/ARCHIVE chưa được capability API cho phép.
- Khi client phát lại dữ liệu COLD, hệ thống có thể tạo restore job hoặc tự động restore segment cần thiết.
- Không để EventPoller/replay caller bị block bởi tác vụ tải file từ object storage.

## 2. Các Thành Phần Chính

### 2.1 StoragePool

`storage_pools` định nghĩa backend lưu trữ:

- `LOCAL_DISK`: đĩa local cho HOT hoặc WARM.
- `NAS`: mount/network path cho WARM.
- `MINIO`, `S3`, `ARCHIVE`: các type object storage có trong model/backend.

Capability API hiện cho phép HOT/WARM dùng `LOCAL_DISK` hoặc `NAS`; COLD dùng `NAS` hoặc `MINIO`. `S3`/`ARCHIVE` đã có enum/backend nhưng `pool/options` trả `false` và validation API chưa cho tạo pool bằng hai type này.

Các trường quan trọng:

- `id`, `name`, `type`, `tier`
- `mount_path`, `network_path`
- `endpoint`, `bucket`, `base_path`, `access_key`, `secret_key_enc`
- `high_watermark_percent`, `critical_watermark_percent`
- `enabled`, `health_check_enabled`
- `capacity_source` (`METRICS` | `QUOTA`, mặc định `METRICS`) và `capacity_quota_bytes` (nullable), thêm ở migration `17_storage_pool_capacity_quota.sql`

Khi trả về client, `access_key` và `secret_key` phải được mask bằng `***********`.

Dung lượng pool object storage có hai nguồn:

- `METRICS`: dùng metric cluster của MinIO (`/minio/v2/metrics/cluster`). Số này là raw capacity của cả cluster: gồm parity erasure coding, mọi bucket khác và có độ trễ, nên không phản ánh đúng phần dung lượng của pool.
- `QUOTA`: usage = tổng `size_bytes` của các range `AVAILABLE`/`RESTORING`/`EXPIRED` trên pool (`SegmentTierRangeRepository::sumStoredBytesByPool()`), total = `capacity_quota_bytes`. HeadBucket vẫn quyết định pool online hay không. Nếu metric raw cũng đọc được và raw usage ≥ `critical_watermark_percent` thì lấy giá trị lớn hơn, để metric raw vẫn là lưới an toàn khi chính cluster sắp đầy. Tổng byte được cache `storage.tiering_quota_usage_cache_seconds` (mặc định 30 giây) vì probe dung lượng chạy nhiều lần mỗi cycle.

Khuyến nghị khi dùng `QUOTA`: đặt hard quota cho bucket trên MinIO (`mc quota set <alias>/<bucket> --size ...`) cao hơn `capacity_quota_bytes` khoảng 5%, để MinIO chặn ghi nếu metadata và dữ liệu thực tế lệch nhau (ví dụ object được ghi ngoài hệ thống).

### 2.2 StoragePolicy

`storage_policies` định nghĩa cách giữ và chuyển dữ liệu:

- HOT giữ bao nhiêu ngày.
- WARM giữ bao nhiêu ngày.
- COLD giữ bao nhiêu ngày.
- Pool nào gắn với từng tier.
- Khi hết hạn thì xóa tự động hay đánh dấu chờ duyệt.

Policy có thể được gán ở mức camera qua `camera_policy_assignments`.

Các biến cấu hình chính:

| Nhóm | Biến | Ý nghĩa/hiện trạng |
|------|------|-------------------|
| Policy | `id`, `name`, `description`, `enabled` | Identity và trạng thái policy |
| Policy | `total_retention_days` | Metadata tổng retention; engine delete dùng `delete_after_days` |
| Policy | `allow_camera_override` | Trả cho FE/effective policy; assignment handler chưa chặn theo cờ này |
| Policy | `protect_event_video` | Được lưu/trả API; retention dùng `protected_videos` và delete-policy flags |
| Tier | `tier`, `enabled`, `pool_id` | Tier và pool đích thực |
| Tier | `retain_until_days` | Mốc move theo tuổi của source tier |
| Tier | `overflow_action` | `MOVE_TO_NEXT_TIER`, `DELETE_OLDEST`, `STOP_RECORDING_AND_ALERT` |
| Tier | `high_watermark_percent`, `critical_watermark_percent` | Validation/API; pressure engine lấy watermark runtime từ pool |
| Tier | `data_mode`, `priority` | Được parse/lưu/trả API, chưa thay đổi thuật toán chọn range |
| Delete | `delete_after_days`, `delete_mode` | Mốc và cách xử lý retention cuối |
| Delete | `skip_protected_video`, `skip_evidence_video` | Protected flag được dùng; evidence chưa có nhánh riêng |
| Delete | `require_approval_before_delete`, `external_pool_id` | Mark `EXPIRED` hoặc move tới pool ngoài |
| Advanced | `enable_early_move_when_pool_high` | Bật pressure tiering |
| Advanced | `min_segment_age_minutes_before_move` | Tuổi ổn định tối thiểu |
| Advanced | `skip_move_if_pool_offline` | Bỏ qua source/destination không có capacity runtime |
| Advanced | `prefer_move_no_event_video_first`, `prefer_keep_event_video_longer`, `alert_when_pool_critical` | Đã parse; chưa chi phối engine/alert hiện tại |

Thứ tự ưu tiên policy:

```text
Camera override
  -> Group
  -> Project
  -> System default
```

Hiện backend chỉ resolve camera override rồi fallback về system default; group/project được giữ trong enum/thiết kế nhưng chưa có route assignment.

### 2.3 SegmentTierRange

Với mô hình 300 camera, 600 stream ghi 1 segment MP4/phút, 6 tháng tạo khoảng 155 triệu segment. Vì vậy backend dùng metadata dạng range compact:

- `segment_tier_ranges`: nguồn metadata tier duy nhất, lưu trạng thái theo khoảng thời gian liên tục.
- TimeFile là nguồn file-level để resolve, move, restore và xóa từng MP4 trong range.

Range được giới hạn theo ngày local của `start_time`, với đầu ngày được tính bằng `StampUtils::getStartOfDay()` theo timezone của process. Segment liền kề chỉ merge khi cùng camera, stream, tier, pool, status và cùng ngày local; một lần split/transfer vì thế không phải đọc toàn bộ cửa sổ retention nhiều tháng. Ngày local có thể dài 23, 24 hoặc 25 giờ ở timezone có DST; thay đổi timezone sau khi đã ghi metadata có thể làm quy tắc compact của dữ liệu mới không đồng nhất với range cũ.
Ở trạng thái ổn định, 600 stream trong 180 ngày cần khoảng 108.000 daily range thay vì hàng triệu row file-level.

**Quy tắc sở hữu segment:** một range sở hữu các segment (TimeBlock) có `start_time` nằm trong khoảng nửa mở `[range.start_time, range.end_time)` của cùng camera/stream. File MP4 liền kề có thể chồng nhau vài giây ở biên, nhưng mỗi segment chỉ thuộc đúng một range. Mọi luồng liệt kê file (split, move, delete, cleanup metadata), playback resolver và backfill đều dùng quy tắc này. Không tạo range mới chiếm quyền sở hữu của range đã có, và khi gộp range không được vắt qua range khác tier/pool/status.

`segment_tier_ranges` lưu:

- `range_id`
- `camera_id`
- `stream_id`
- `tier`: `HOT`, `WARM`, `COLD`
- `pool_id`: bắt buộc, phải là id của một pool thật trong `storage_pools`
- `status`: `AVAILABLE`, `RESTORING`, `EXPIRED`, `DELETED`, `MISSING`
- `start_time`, `end_time`
- `segment_count`, `size_bytes`

Các luồng tiering, timeline, summary, playback và restore phải lấy `segment_tier_ranges` làm nguồn dữ liệu chính. `pool_id`, `source_pool_id`, `target_pool_id` không được rỗng; nếu không resolve được pool thật thì backend bỏ qua metadata/job hoặc fail ngay.

### 2.4 Dữ Liệu `segment_tier_ranges` Được Lấy Từ Đâu

`segment_tier_ranges` được tạo/cập nhật từ các nguồn sau:

1. **Luồng ghi hình local/HOT**
   - `MP4Recorder` emit `Broadcast::kBroadcastRecordMP4` sau khi file `.tmp` đã close và rename sang `.mp4`.
   - `TierStorageManager` nhận `RecordInfo`, lấy `camera_id`, `stream_id`, `start_time`, `time_len`, `file_size`, `file_path`.
   - Backend resolve HOT pool bằng longest-prefix match từ `file_path` với `mount_path/network_path` của các HOT pool enabled.
   - Backend tạo `SegmentTierRange` tier `HOT`, đúng `pool_id`, status `AVAILABLE`, rồi gọi `registerIfUncovered(...)`: nếu `start_time` của segment đã thuộc một range thì bỏ qua (idempotent), ngược lại gộp với range liền kề (khoảng trống ≤ 10 giây — `kSegmentMergeGapSeconds`, cùng ngày local) thành range compact.
   - Nếu không match được HOT pool, segment không được ghi vào `segment_tier_ranges`.
   - Timefile vẫn được ghi dưới `Protocol::kMP4SavePath`; `file_path` trong `TimeBlock` trỏ tới segment MP4 thật, có thể nằm ở HOT pool khác.

2. **Luồng move tier thành công**
   - Khi `tiering_jobs` move thành công, backend gọi `updateTierBySnapshotAt(...)`: chỉ đổi tier/pool/status nếu range vẫn khớp đúng snapshot (id, camera, stream, window, tier, pool, status, `segment_count`, `size_bytes`) đã dùng để liệt kê file. Nếu range đã đổi (split, merge, bị xóa) thì job fail với `STALE_RANGE_SNAPSHOT` và rollback file đã copy.
   - Move LOCAL/NAS và object storage đều cập nhật range theo cùng nguyên tắc này.
   - Segment mất ở cả nguồn lẫn đích được retry có backoff; hết số lần thử thì bị loại khỏi range (xem 5.5).

3. **Luồng expire/delete**
   - `MARK_EXPIRED_WAIT_APPROVAL` chuyển range sang `EXPIRED` nhưng chưa xóa file.
   - Automatic delete/`DELETE_OLDEST` chỉ chuyển range sang `DELETED` khi mọi file/object đã xóa thành công.
   - Mọi đường xóa bỏ qua range đang có `tiering_jobs` `PENDING/RUNNING`, đọc lại range ngay trước khi xóa file, và đánh dấu `DELETED` + tạo cleanup job trong cùng transaction chỉ khi range vẫn khớp snapshot.
   - Sau đó `tier_metadata_cleanup_jobs` xóa đúng các `TimeBlock` tương ứng; move giữa tier không xóa timefile.
   - `tier_metadata_cleanup_jobs` xử lý tuần tự toàn bộ metadata của range: xác minh cleanup TimeFile, cleanup motion trên các ngày bị ảnh hưởng, rồi cleanup bookmark trong range và chỉ chuyển `DONE` khi mọi bước hoàn tất. Camera không có TimeFile được hiểu là không có dữ liệu lưu trữ và tiếp tục cleanup; lỗi mở/đọc/parse TimeFile (báo qua `TimeQuery::queryBlocks()`) giữ job ở `PENDING` kèm `error_message`.
   - `protected_videos` và `segment_retention_overrides` được kiểm tra tập trung trước mọi đường xóa.

4. **Luồng restore COLD**
   - `restore_jobs` là parent summary; `restore_job_segments` lưu từng segment cần tải và trạng thái bền vững.
   - Trạng thái restore không sửa `segment_tier_ranges`; tier/pool vẫn là COLD vì file restore chỉ nằm trong cache tạm.

### 2.5 TierStorageManager

`TierStorageManager` chịu trách nhiệm:

- CRUD pool/policy.
- Gán policy cho camera.
- Kiểm tra health/capacity pool.
- Tạo HOT pool mặc định từ `Protocol::kMP4SavePath` nếu chưa có HOT pool.
- Tạo/duy trì system default policy cố định `policy-system-default`.
- Đọc metadata chính từ `segment_tier_ranges`.
- Tạo và thực thi tiering job.
- Upload segment lên object storage qua `TierObjectStorage` (API hiện cho phép MinIO).
- Restore segment COLD về restore cache tạm theo `storage.restore_save_path`.
- Ghi nhận HOT range từ event `kBroadcastRecordMP4` và backfill định kỳ từ timefile.

### 2.6 File Storage Backend

Các pool dạng filesystem được tách theo backend:

- `TierFileStorageBase`: helper chung register pool, test path, resolve path, copy qua file `.tmp`, rename atomic, delete, `statvfs`.
- `TierLocalDiskStorage`: dùng cho `LOCAL_DISK`.
- `TierNASStorage`: dùng cho `NAS`, hiện yêu cầu `network_path`/`mount_path` đã mount và writable; đây là hook để bổ sung stale-mount/retry/timeout.

`TierStorageManager` chọn backend theo `StoragePool.type`; nghiệp vụ tiering không xử lý trực tiếp chi tiết copy/delete của từng backend.

### 2.7 TierObjectStorage

`TierObjectStorage` quản lý client MinIO/S3/ARCHIVE theo `pool_id`. Trong capability API hiện tại, chỉ `MINIO` được phép làm object-storage pool COLD.

Các hàm quan trọng:

- `registerPool(...)`
- `uploadSegment(pool_id, local_path, s3_key)`
- `downloadSegment(pool_id, s3_key, local_path)`
- `deleteSegment(pool_id, s3_key)`
- `deleteSegmentsBatch(pool_id, s3_keys)`: `DeleteObjects` ở chế độ quiet, tối đa 1 000 key/request; trả `false` khi request lỗi hoặc có bất kỳ key nào nằm trong `Errors`.
- `segmentExists(pool_id, s3_key)`
- `headSegment(pool_id, s3_key)`: `HeadObject` trả `PRESENT`, `ABSENT` (chỉ khi HTTP 404) hoặc `UNKNOWN` (mọi lỗi khác). `segmentExists` gộp mọi lỗi thành `false` nên không dùng được để chứng minh object đã mất.
- `listObjects(pool_id, prefix, out, error)`: `ListObjectsV2` theo continuation token, trả key đầy đủ kèm size và `LastModified`. Lỗi ở bất kỳ trang nào trả `false` và để `out` rỗng, không bao giờ trả danh sách thiếu.
- `listCommonPrefixes(pool_id, prefix, out, error)`: như trên với delimiter `/`, trả các "thư mục" con ngay dưới `prefix`.
- `bucketVersioningStatus(pool_id, status)`: đọc `GetBucketVersioning` (`Enabled`, `Suspended` hoặc rỗng).
- `makeS3Key(base_path, camera_id, stream_id, segment_path)`

Đây là lớp dùng chung cho cả upload khi chuyển sang COLD và download khi restore segment.

Bucket bật versioning không giải phóng dung lượng khi xóa: `DeleteObjects` chỉ tạo delete marker, bản cũ thành noncurrent version và vẫn chiếm chỗ. `TierStorageManager::refreshPoolVersioning()` đọc trạng thái versioning ngay khi đăng ký pool và tối đa một lần/giờ trong probe dung lượng (health cycle và maintenance). Khi `Enabled`, backend ghi cảnh báo `POOL_VERSIONING_BLOCKS_RECLAIM` (tối đa một lần/ngày/pool) khuyến nghị thêm lifecycle rule `NoncurrentVersionExpiration` (kèm dọn expired delete marker) hoặc tắt versioning, và thêm ghi chú vào `capacity_message` của health. Backend không chặn thao tác nào vì trạng thái này.

### 2.8 Job Tables

Các bảng job/trạng thái hỗ trợ:

- `tiering_jobs`: di chuyển segment giữa tier.
- `restore_jobs`: theo dõi restore segment từ COLD về restore cache tạm.
- `restore_job_segments`: hàng đợi restore từng segment, có startup recovery và bounded concurrency.
- `tier_metadata_cleanup_jobs`: outbox cleanup timefile sau khi xóa vật lý.
- `segment_retention_overrides`: thời hạn gia hạn retention do operator đặt.
- `storage_alerts`: cảnh báo storage.
- `protected_videos`: đoạn video được bảo vệ/evidence/locked.

`tiering_jobs` lưu `job_id`, camera/stream/range, source/target tier, source/target pool, status, segment time window, `bytes_total`, `bytes_moved`, `error_message`, `created_at`, `updated_at`. Ba field `job_trigger`, `policy_id`, `policy_updated_at` ghi lại nguyên nhân và policy context tại thời điểm tạo job để kiểm tra lại khi policy camera thay đổi. Row cũ được migrate với `LEGACY`; job pressure cũ có prefix `ptj-` được nhận diện thành `PRESSURE`.

`restore_jobs` lưu summary parent. Mỗi child trong `restore_job_segments` giữ stream, range, segment path, source pool, bytes và lỗi riêng. Job auto-restore dùng `reason=AUTO_RECORD_ACCESS`.

## 3. Luồng Cấu Hình

### 3.1 Tạo Pool

FE gọi:

```text
POST /media/mserver/storage/pool/create
```

Backend kiểm tra:

- `name`, `type`, `tier` bắt buộc.
- `LOCAL_DISK` cần `mount_path` và được xử lý qua `TierLocalDiskStorage`.
- `NAS` cần `mount_path` hoặc `network_path`, path phải được mount/writable và được xử lý qua `TierNASStorage`.
- `MINIO` cần `endpoint`, `bucket`, `base_path`, `access_key`, `secret_key`.
- `high_watermark_percent < critical_watermark_percent`.
- `capacity_source` là `METRICS` hoặc `QUOTA` (không phân biệt hoa thường); `QUOTA` chỉ dành cho object storage và bắt buộc `capacity_quota_bytes` là số nguyên dương. `pool/update` với `capacity_quota_bytes: null` xóa quota, bỏ trống field thì giữ nguyên.

`mount_path`/`network_path` của filesystem pool lưu logical recorder root, không
chứa `Record::kAppName`. `TierStorageManager::resolveFilePoolDataRoot()` nối app
directory trước khi register backend, kiểm tra kết nối, tiering hoặc đo dung
lượng. Nhờ đó bind mount trực tiếp vào `.../record` vẫn có thể cấu hình pool bằng
root cha mà không làm lệch đường ghi của Recorder.

Type/tier còn phải khớp ma trận `pool/options`; object storage chỉ được chấp nhận khi build có `ENABLE_AWS_SDK`.

Sau khi tạo pool, `TierStorageManager` register pool vào backend tương ứng:

- Object storage: `TierObjectStorage`.
- Local disk: `TierLocalDiskStorage`.
- NAS: `TierNASStorage`.

### 3.2 Test Connection

FE gọi:

```text
POST /media/mserver/storage/pool/testConnection
```

Với pool MinIO, backend dùng object-storage probe để xác nhận kết nối. Với filesystem pool, backend kiểm tra path/mount và quyền truy cập.

Khi chỉnh sửa pool, FE gửi `id` cùng các thông số mới. Backend merge request
với cấu hình đã lưu rồi test bằng client/path tạm thời, vì vậy endpoint hoặc
credential mới được kiểm tra trước khi update mà không thay đổi pool đang chạy.

### 3.3 Tạo Policy

FE gọi:

```text
POST /media/mserver/storage/policy/create
```

Backend validate:

- `total_retention_days` bắt buộc phải có trong request; handler hiện chưa check riêng giá trị `> 0`
- `HOT` phải enabled
- WARM optional; nếu WARM disabled và COLD enabled thì HOT có thể move trực tiếp sang COLD
- `retain_until_days` tăng dần giữa các tier enabled theo thứ tự `HOT -> WARM -> COLD`
- Tier enabled phía sau yêu cầu tier enabled liền trước có `overflow_action = MOVE_TO_NEXT_TIER`
- Tier enabled cuối cùng không được dùng `MOVE_TO_NEXT_TIER` (không có tier đích nên dữ liệu sẽ không bao giờ bị xóa); lưu ý `overflow_action` mặc định là `MOVE_TO_NEXT_TIER` khi request không truyền
- Tier dùng `DELETE_OLDEST` phải có `retain_until_days > 0`
- `delete_after_days >= retain_until_days` lớn nhất
- `pool_id` tồn tại và enabled
- `pool_id` phải đúng tier đang cấu hình và type phải được tier đó hỗ trợ
- watermark hợp lệ

### 3.4 Assign Policy

FE gọi:

```text
POST /media/mserver/storage/policy/assignCamera
```

Từ thời điểm này, camera dùng policy được gán để quyết định chuyển tầng.

### 3.5 System Default Policy

Khi start, `TierStorageManager` đảm bảo tồn tại policy mặc định cố định:

```text
policy_id = policy-system-default
name      = System Default
source    = SYSTEM_DEFAULT
```

Policy này trỏ vào HOT pool mặc định có id `pool-default-hot`. Nếu chưa có HOT
pool, backend lưu `Protocol::kMP4SavePath` làm logical root; effective data root
là `{Protocol::kMP4SavePath}/{Record::kAppName}`. Sau đó backend tạo policy
default.

Khi gọi `/media/mserver/storage/policy/removeCamera`, backend chỉ xóa camera-level override trong `camera_policy_assignments`. Camera không được assign cứng về default; `getEffectivePolicy(camera_id)` tự fallback về `policy-system-default`.

Lưu ý theo code hiện tại: `ensureDefaultHotPool()` chỉ tạo `pool-default-hot` khi chưa có HOT pool nào. `ensureSystemDefaultPolicy()` lại cần `pool-default-hot` để tạo policy mặc định mới. Nếu DB đã có HOT pool custom nhưng chưa có `pool-default-hot`, cần tạo system default policy qua migration/repair hoặc chỉnh code chọn HOT pool enabled hiện có.

`Manager::enforceStoragePolicy()` luôn start `StorageManager`; khi có `ENABLE_TIER_STORAGE` nó start thêm `TierStorageManager`. Timer của `StorageManager` vẫn cleanup user session/file tạm nhưng không chạy policy xóa archive cũ; retention archive khi đó do `TierStorageManager` điều phối.

### 3.6 Xác Thực, Permission Và Mã Lỗi

Tất cả route `/media/mserver/storage/...` đều yêu cầu JWT. Các permission chính:

- Playback: `1003`.
- Read MediaServer: `150301`.
- Modify MediaServer: `150302`.

Khi handler truyền nhiều permission vào `CHECK_USER_PERMISSION(...)`, các check được kết hợp bằng AND. Ví dụ, restore job detail/list cần cả playback và read MediaServer.

Storage error dùng nhóm `914xxx`; các mã quan trọng trong luồng này:

| Code | HTTP | Ý nghĩa |
|------|------|---------|
| `914009` | 404 | Cần restore |
| `914016` | 404 | Không tìm thấy segment |
| `914017` | 404 | Segment hết hạn |
| `914018` | 404 | Restore job không tồn tại |
| `914019` | 500 | Tạo restore job thất bại |
| `914020..914022` | 404/500 | Tiering job not found/update/cancel failed |

Danh sách đầy đủ nằm trong [api-storage-tiering.md](api-storage-tiering.md).

## 4. Luồng Ghi Hình Và Ghi Nhận Segment

Luồng ghi hình hiện tại tạo file MP4/HLS trong root cấu hình bởi:

- `Protocol::kMP4SavePath`
- `Record::kAppName`

Timefile vẫn nằm dưới `{Protocol::kMP4SavePath}/{Record::kAppName}/{camera_id}`. Segment MP4 thật có thể nằm ở HOT pool khác; `TimeBlock.file_path` lưu absolute path thật của segment.

Để tiering hoạt động đúng, mỗi segment mới cần được gom vào `segment_tier_ranges` với `pool_id` thật.

Luồng cập nhật `segment_tier_ranges`:

```text
MP4Recorder close/rename segment
  -> emit kBroadcastRecordMP4
  -> TierStorageManager resolve HOT pool từ RecordInfo.file_path
  -> nếu không resolve được HOT pool: log và bỏ qua
  -> merge/upsert SegmentTierRange
      tier = HOT
      pool_id = matched HOT pool id
      status = AVAILABLE
      segment_count += 1
      size_bytes += file_size
```

Backfill lịch sử chạy bằng dispatcher riêng của `TierStorageManager`, tách khỏi tiering cycle. Mỗi camera có một dòng durable trong `tier_backfill_tasks`; worker claim một task đến hạn, đọc đúng một chunk timefile bằng `TimeQuery::queryBlocks()`, decode `TimeBlock.file_path`, resolve HOT pool rồi dựng range. Range compact và cursor task được commit trong cùng transaction, nên restart tiếp tục từ cursor đã commit và replay không nhân đôi metadata.

Quy tắc dựng range của một chunk:

- Chunk sở hữu các block có `start_time` trong `[cursor, chunk_end)`; block đã thuộc một range (bất kỳ tier/status) thì bỏ qua.
- Block được nhóm theo stream và HOT pool, sắp theo thời gian; range mới được tách khi sang ngày local khác, khi khoảng trống giữa hai block lớn hơn 10 giây, hoặc khi giữa hai block có `start_time` của một range đã tồn tại. `end_time` của range mới bị chặn tại `start_time` của range kế tiếp.
- Lúc commit, nếu range mới chiếm quyền sở hữu của range khác (metadata vừa thay đổi) thì rollback và chunk được retry.
- Block orphan (không có stream/file path hoặc path không thuộc HOT pool nào) được đếm vào `orphan_blocks`, ghi cảnh báo `TIER_BACKFILL_ORPHAN` và **không** chặn cursor.
- Block đã kết thúc quá `tier_reconcile_min_age_seconds` mà file MP4 không còn trên HOT pool (pool qua health gate và thư mục dữ liệu tồn tại) không được đăng ký thành range; sau khi chunk commit, TimeBlock của nó bị gỡ khỏi timefile và ghi cảnh báo `TIER_BACKFILL_MISSING_FILE`. Nếu pool không kiểm chứng được là đọc được, chunk retry thay vì kết luận file đã mất. Trường hợp điển hình là file đã bị `StorageManager` cũ xóa nhưng việc rebuild timefile chưa hoàn tất trước khi nâng cấp.
- Lỗi mở/đọc/parse timefile làm chunk retry, cursor không tiến.

Task chuyển qua `PENDING -> RUNNING -> PENDING/COMPLETED`. Lỗi chuyển sang `RETRY` với exponential backoff; quá số lần thử chuyển sang `FAILED`. Khi MediaServer khởi động, task còn `RUNNING` được đưa về `PENDING`. Mỗi phút, việc đồng bộ task chỉ trượt `window_start`/`cutoff_time` tới; task `FAILED`/`RETRY` giữ nguyên số lần thử, backoff và lỗi, task `COMPLETED` được mở lại `PENDING` khi cutoff mới vượt cursor. Chỉ khi đổi `timeline_path` hoặc mở rộng cửa sổ thì cursor mới được reset.

## 5. Luồng Chuyển Tầng

`TierStorageManager::runTieringCycle()` chạy định kỳ bằng timer của `TierStorageManager` (hiện 300 giây/lần). Hàm này dispatch async sang `WorkThreadPool`.

Health monitoring không còn phụ thuộc chu kỳ này. `TierStorageManager` dispatch
một health cycle riêng theo `storage.pool_health_check_interval_seconds`, probe
theo số pool và cập nhật runtime cache. `pool_metrics` chỉ được ghi khi trạng
thái thay đổi hoặc đến chu kỳ snapshot để tránh tăng tải SQLite.

### 5.1 Các bước của một cycle

Luồng hiện tại theo code:

1. Reset ticker và ghi log bắt đầu.
2. Refresh `_pool_cache` từ `storage_pools`.
3. Gọi `ensureSystemDefaultPolicy()` để đảm bảo policy `policy-system-default` tồn tại.
4. Kiểm tra effective policy hiện tại rồi dispatch `tiering_jobs` trạng thái `PENDING` sang worker queue có giới hạn tổng và giới hạn trên từng pool. Job không còn đúng route, target pool, retention hoặc trigger được chuyển `PENDING -> CANCELLED` với nguyên nhân `POLICY_CHANGED`; lỗi lookup policy giữ job ở `PENDING`. Copy/upload không chặn maintenance scan; worker kiểm tra policy thêm lần nữa ngay trước I/O và tự dispatch phần backlog khi hoàn tất.
5. Lấy danh sách camera có dữ liệu `AVAILABLE`:
   - Từ `segment_tier_ranges.findDistinctAvailableCameras()`.
6. Với từng camera:
   - `getEffectivePolicy(camera_id)`.
   - Nếu có policy hiệu lực, chạy `processCameraTiering(camera_id, policy)`.
   - Chạy `processCameraPressureTiering(camera_id, policy)`.
   - Chạy `enforcePolicyDeleteRetention(camera_id, policy)`.
   - Chạy `enforceCameraArchiveRetention(camera_id, policy)` (chỉ `keepArchivedMaxFor`).
7. Chạy `enforcePoolPressureRetention()` một lần cho mọi pool enabled, gồm HOT/WARM/COLD và object storage (xem 5.4).
8. Chạy `runRangeFileReconcile()` (xem 5.6).
9. Xử lý metadata cleanup, restore job, prune metric/range, dọn cache restore và MP4 orphan, rồi dispatch job lần nữa.
10. Ghi log thời gian hoàn tất.

### 5.2 Move Theo Tuổi Dữ Liệu

`processCameraTiering()` xử lý move theo `retain_until_days`.

Backend sort tier theo thứ tự `HOT -> WARM -> COLD`, nhưng tier đích là **tier enabled kế tiếp** chứ không bắt buộc là tier liền kề vật lý. Vì vậy WARM có thể disabled; khi HOT có `overflow_action = MOVE_TO_NEXT_TIER`, dữ liệu HOT sẽ move trực tiếp sang COLD.

1. Bỏ qua source tier disabled.
2. Tìm destination là tier enabled kế tiếp.
3. Bỏ qua nếu source hoặc destination `pool_id` rỗng.
4. Tính:

```text
move_threshold = now - source_tier.retain_until_days * 86400
move_threshold không được mới hơn now - min_segment_age_minutes_before_move
move_threshold được làm tròn xuống theo tiering_split_granularity_seconds (mặc định 3600)
```

Làm tròn giúp mỗi stream chỉ bị split, tạo job và range DELETED tối đa một lần mỗi giờ thay vì mỗi cycle 5 phút; dữ liệu vì thế có thể chuyển/xóa trễ tối đa một khoảng bằng độ mịn. Mốc retention theo tuổi (`delete_after_days`, `keepArchivedMaxFor`) cũng được làm tròn như vậy; xóa khi vượt ngưỡng không bị ảnh hưởng.

5. Query `segment_tier_ranges` theo source tier/source pool với `start_time < move_threshold`.
6. Bỏ qua range đã có job `PENDING/RUNNING` (không đọc timefile lại).
7. Đọc timefile để liệt kê segment thuộc range, đồng thời làm mới `segment_count`/`size_bytes` theo timefile. Nếu range còn phần mới chưa đủ tuổi, split tại `start_time` của segment đầu tiên chưa đủ tuổi (các segment cùng `start_time` được xét như một nhóm). Split/refresh dùng `replaceRangeWithPiecesIfUnchanged`, piece mới kế thừa `segment_retention_overrides` của range gốc.
8. Với range thuộc camera hiện tại, tạo job bằng `queueTierMoveJob(...)`.

`queueTierMoveJob()` yêu cầu `source_pool_id` và `target_pool_id` đều không rỗng. Source pool thực tế ưu tiên lấy từ `SegmentTierRange.pool_id`; policy source pool là fallback cấu hình nhưng cũng phải có giá trị. Job ghi `job_trigger=RETENTION` cùng `policy_id`/`policy_updated_at` hiện tại.

`queueTierMoveJob()` chống tạo trùng job bằng cách kiểm tra job `PENDING/RUNNING` cùng `range_id` (hoặc cùng camera/stream/source/target/window với job legacy). Range có job trước đó fail vì `SOURCE_AND_TARGET_NOT_FOUND` chỉ được queue lại sau backoff `tiering_missing_segment_retry_base_seconds << (số lần fail - 1)`, tối đa 1 ngày. Khi chạy, job chỉ xử lý nếu range vẫn khớp đúng window và `bytes_total` lúc tạo job. Dispatcher cache effective policy theo camera trong mỗi lượt và batch-load range để tránh N+1 query. Dispatcher chỉ claim job khi policy vẫn cho phép, source cho phép đọc và destination cho phép ghi. Pool `UNKNOWN`, `OFFLINE`, có snapshot stale hoặc bị disable làm job tiếp tục ở `PENDING`; health cycle sẽ gọi lại dispatcher sau khi probe để job tự resume khi pool phục hồi.

Trước khi tạo job (mọi trigger: `RETENTION`, `PRESSURE`, `DELETE_POLICY_EXTERNAL`), `queueTierMoveJob()` probe dung lượng destination pool qua `isDestinationPoolCritical()`: nếu usage ≥ `critical_watermark_percent` thì không tạo job, range giữ nguyên ở tier nguồn và ghi cảnh báo `TIER_DEST_POOL_CRITICAL` (tối đa một lần/giờ/pool). Kết quả probe được cache trong một maintenance cycle nên mỗi pool chỉ bị probe (statvfs/metric MinIO) một lần mỗi cycle; pool không đo được dung lượng (`total_bytes = 0`) không bị coi là critical. `processCameraTiering()` kiểm tra điều kiện này trước khi split range để không ghi lại range khi chắc chắn không move được.

Health gate cho phép source được đọc/xóa ở trạng thái `OK`, `WARNING`, `HIGH` hoặc `CRITICAL`, nhưng không ghi thêm dữ liệu vào destination `CRITICAL`. Pool tắt `health_check_enabled` giữ hành vi tương thích cũ, miễn là pool vẫn enabled. Worker kiểm tra lại health sau khi claim; nếu trạng thái thay đổi, job được trả từ `RUNNING` về `PENDING` thay vì đánh dấu `FAILED`.

### 5.3 Move Sớm Khi Pool HOT/WARM Đầy

`processCameraPressureTiering()` xử lý trường hợp pool source vượt `high_watermark_percent` trước khi dữ liệu đạt `move_threshold`.

Điều kiện chính:

- Source/destination tier enabled.
- Source/destination `pool_id` không rỗng.
- Destination là tier enabled kế tiếp, nên HOT có thể move trực tiếp sang COLD khi WARM disabled.
- Destination pool tồn tại và chưa vượt `critical_watermark_percent`.
- Source pool thực tế từ `SegmentTierRange.pool_id` tồn tại, enabled và usage >= watermark của pool.
- Range source đang `AVAILABLE`.
- Range đủ ổn định theo `min_segment_age_minutes_before_move`, tối thiểu 60 giây.

Luồng này cũng tạo `tiering_jobs` với `job_trigger=PRESSURE`. Mục tiêu reclaim xấp xỉ tới dưới high watermark 5%.

Cách chọn range theo `storage.tiering_pressure_delete_mode` (dùng chung cho move sớm và xóa theo áp lực):

- `OLDEST`: mỗi camera tự lấy tối đa `tiering_max_ranges_per_camera_cycle` range cũ nhất đã ổn định và queue move cho tới khi riêng phần của camera đó đủ kéo pool xuống mục tiêu; nhiều camera cùng pool vì vậy có thể move vượt nhu cầu.
- `BALANCED` (mặc định): trước vòng camera, `planBalancedPressureMoves()` gom các camera có policy bật move sớm ra khỏi tier của từng pool (destination không critical; không offline nếu `skip_move_if_pool_offline`). Với mỗi pool vượt high watermark, lượng cần move = `used - total * (high - 5)% - byte job đang PENDING/RUNNING từ pool`; nếu còn dương thì tìm `kp` giống xóa cân bằng trên cửa sổ `[lo, now - max(min_segment_age, 60s))` của từng camera (range có override vẫn được move vì move không làm mất dữ liệu). `processCameraPressureTiering()` sau đó queue move các range bắt đầu trước mốc cắt của camera (split tại mốc), tối đa `tiering_max_ranges_per_camera_cycle` range mỗi camera mỗi cycle. Job đã queue được trừ vào nhu cầu ở cycle sau nên không queue lặp.

### 5.4 Retention Theo CameraOption

`enforceCameraArchiveRetention()` giữ ý nghĩa hiện tại của cấu hình từng camera:

- `keepArchivedMaxFor`: nếu không auto và > 0, dữ liệu cũ hơn mốc này được expire kể cả khi storage còn dung lượng.
- `keepArchivedMinFor`: dữ liệu mới hơn mốc này được bảo vệ khỏi xóa khi pressure (áp dụng trong `enforcePoolPressureRetention()`); pressure move vẫn được phép vì không làm mất dữ liệu. Camera không có statistic recorder không có vùng bảo vệ này.

Retention theo tuổi (`keepArchivedMaxFor` và `delete_after_days`) dùng chung `collectAgeRetentionPieces()`:

- Lấy range `AVAILABLE` có `start_time < cutoff` theo phân trang keyset `(start_time, range_id)`, tối đa 16 trang mỗi lượt; range có job `PENDING/RUNNING` hoặc override còn hạn bị loại ngay trong SQL nên không chiếm chỗ của range khác.
- Range vượt qua cutoff được split; chỉ phần cũ bị xử lý, phần mới được lượt sau xử lý khi đủ tuổi.
- Phần đã split được cắt tiếp quanh video được bảo vệ (`carveDeletablePieces()`): segment chạm video bảo vệ nằm trong piece riêng giữ `AVAILABLE`, các piece còn lại được thu hẹp sát segment để không chạm video bảo vệ và được xử lý bình thường.

Xóa khi vượt ngưỡng chạy trong `enforcePoolPressureRetention()`, một lần mỗi cycle cho từng pool enabled ở mọi tier (HOT/WARM/COLD), kể cả object storage:

- Chỉ chạy khi usage thực tế ≥ `critical_watermark_percent` và pool đọc/xóa được. Pool không đo được dung lượng (`total_bytes = 0`, ví dụ object storage `METRICS` khi metric không đọc được) bị bỏ qua.
- Tier cuối và object storage không có job move đi nên `pending_move_bytes = 0` và nhánh hoãn xóa không áp dụng.
- Pool object storage `METRICS`: metric MinIO trễ so với thao tác xóa, nên sau một lượt đã xóa được dữ liệu, pool bị tạm dừng xóa theo áp lực trong `storage.tiering_object_pressure_cooldown_seconds` (mặc định 900 giây) để không xóa quá tay dựa trên số liệu cũ. Pool `QUOTA` tính usage từ DB nên không cần cooldown; cache tổng byte của pool bị xóa ngay sau lượt xóa.
- Ưu tiên chuyển tầng: nếu pool còn job move `PENDING/RUNNING` và usage < `critical + tiering_pressure_delete_margin_percent` thì hoãn xóa; nếu vẫn xóa thì trừ số byte đang chờ move khỏi lượng cần giải phóng.
- Lượng cần giải phóng = `used - total * (high - 5)%`. Cách chọn dữ liệu theo `storage.tiering_pressure_delete_mode`:
  - `OLDEST`: range được lấy theo `end_time` tăng dần trên toàn pool (bỏ range có job hoặc override còn hạn), bỏ range mới hơn `keepArchivedMinFor` của camera, cắt quanh video được bảo vệ như trên rồi xóa các piece không được bảo vệ, và dừng ngay khi đủ dung lượng. Camera có retention dài chịu toàn bộ phần xóa vì dữ liệu của nó cũ nhất.
  - `BALANCED` (mặc định): mỗi camera có dữ liệu trên pool được xét trên cửa sổ `[lo, hi)`, với `lo` là `start_time` cũ nhất của camera trên pool và `hi = now - keepArchivedMinFor` (`now` khi auto). Với tỉ lệ giữ `kp`, mốc cắt của camera là `hi - (hi - lo) * kp / 100`. Backend tìm `kp` lớn nhất (bước 1%) sao cho tổng byte các range deletable kết thúc trước mốc cắt ≥ lượng cần giải phóng (`sumDeletableBytesByCameraPoolEndedBy()`, dò từ 99 xuống với bước gấp đôi rồi chia đôi), sau đó xóa luân phiên từng range của mỗi camera tới mốc cắt của nó (split tại mốc, cắt quanh video bảo vệ) và dừng khi đủ. Mọi camera vì vậy mất cùng tỉ lệ cửa sổ lưu trữ thay vì camera retention dài mất hết. Camera không còn statistic recorder (đã bị gỡ khỏi hệ thống) được xử lý trước như một nhóm riêng: chỉ khi toàn bộ dữ liệu deletable của nhóm này không đủ thì mới cắt vào cửa sổ của camera đang ghi. Ước lượng bỏ qua range vắt qua mốc cắt nên luôn thiên thấp; phần thiếu được bù ở vòng luân phiên. Trên DB tổng hợp 1,14 triệu range (600 camera), mỗi bước `kp` tốn khoảng 20–60 ms khi `kp` ≥ 90 và khoảng 220 ms ở `kp = 50`.
- Nếu không giải phóng đủ, ghi cảnh báo `POOL_CRITICAL_NO_DELETABLE` (tối đa một lần/giờ/pool).

Với pool object storage, `expireRangeBestEffort()` gom mọi key của range rồi gọi `deleteSegmentsBatch()` một lần thay vì một `DeleteObject` cho mỗi segment. Range chỉ chuyển `DELETED` khi cả batch thành công; một key lỗi giữ range `AVAILABLE`, ghi `TIER_DELETE_FAILED`, và lượt sau gửi lại toàn bộ key (xóa key đã mất là idempotent).

Expire yêu cầu `SegmentTierRange.pool_id` trỏ tới pool còn tồn tại và sẵn sàng cho thao tác xóa. Nếu range thiếu pool, pool không tồn tại hoặc health gate chưa cho phép, backend return mà không đổi trạng thái range và không fallback sang `kMP4SavePath`.

Ngoài `CameraOption`, `enforcePolicyDeleteRetention()` xử lý `delete_policy`:

- `delete_after_days <= 0`: không chạy policy delete retention.
- `MARK_EXPIRED_WAIT_APPROVAL` hoặc `require_approval_before_delete=true`: chỉ đặt range sang `EXPIRED`.
- `MOVE_TO_EXTERNAL_STORAGE`: queue tiering job tới `external_pool_id` nếu pool tồn tại và khác pool hiện tại.
- `DELETE_AUTOMATICALLY`: xóa segment qua backend của pool, update range `DELETED` và rebuild timefile.
- `MOVE_TO_EXTERNAL_STORAGE` bỏ qua range đã nằm ở `external_pool_id` ngay khi lấy ứng viên.
- `skip_protected_video=true`: bỏ qua range overlap bất kỳ row `protected_videos`. Field `skip_evidence_video` được parse nhưng chưa có check riêng trong hàm này.

Sau khi physical deletion thành công, cleanup metadata chạy theo thứ tự:

```text
segment_tier_ranges = DELETED
  -> tier_metadata_cleanup_jobs xóa đúng TimeBlock
  -> đợi và xác minh TimeFile đã swap
  -> cùng metadata job xác định các ngày giao với range
  -> TimeQuery toàn camera xác minh ngày không còn TimeBlock
  -> gọi MotionBlockManager xóa .mblk/.idx của các ngày rỗng
  -> xóa bookmark của camera có start_time nằm trong range
  -> mọi metadata step thành công mới đánh dấu job DONE
```

Mỗi cycle lấy tối đa 2 000 cleanup job `PENDING`, gom TimeBlock cần gỡ theo camera và rebuild timefile một lần cho mỗi camera (chỉ các file ngày chứa block cần gỡ). Liệt kê segment của một range chỉ mở các file ngày trong khung của range (`timelineFilesForWindow()`), không mở toàn bộ timefile của camera.

Pipeline không lưu phase: mỗi lần retry chạy lại từ bước TimeFile và dựa trên trạng thái dữ liệu thực tế làm checkpoint. Vì vậy mọi metadata step phải idempotent. Motion hiện có độ phân giải theo ngày: `TierStorageManager` lọc các ngày không còn TimeBlock rồi chuyển danh sách đó cho API hiện có của `MotionBlockManager`. Nếu stream/range khác vẫn còn TimeBlock trong ngày, motion ngày đó được giữ lại. Bookmark được cleanup bằng `BookmarkImp::removeByTimeRange()` sau bước motion; bookmark ngoài range hoặc thuộc camera khác được giữ lại. Move HOT/WARM/COLD không chạy metadata cleanup vì video vẫn còn được lưu giữ.
Range terminal và cleanup job `PENDING` không bị TTL cleanup để giữ đủ `start_time/end_time` cho retry sau restart. Chỉ range `DELETED`/`MISSING` được prune, và chỉ khi không còn cleanup job `PENDING`; range `EXPIRED` vẫn còn file chờ duyệt nên không bao giờ bị prune. Cleanup job `DONE` được prune cùng thời hạn range terminal. Một range được kích hoạt lại rồi xóa lần nữa sẽ ghi đè cleanup job cũ.

### 5.5 Move LOCAL/NAS

Nếu target pool là local/NAS:

```text
resolve backend theo pool.type:
  LOCAL_DISK -> TierLocalDiskStorage
  NAS        -> TierNASStorage
require source_pool_id/target_pool_id non-empty và tồn tại
liệt kê segment của range (lỗi đọc timefile -> job FAILED)
copy từng file sang mount_path/network_path qua file .tmp
  -> fsync + close file tạm (lỗi -> rollback)
  -> rename, so kích thước đích với nguồn, fsync thư mục cha
  -> update SegmentTierRange theo snapshot (lệch -> rollback file đã copy)
  -> remove source, xóa thư mục ngày rỗng
  -> update tiering job DONE/FAILED
```

Xử lý segment thiếu nguồn:

- Nguồn mất nhưng đích đã có file/object: dùng lại đích (trường hợp retry sau crash hoặc dữ liệu cũ bị lỗi 2 range).
- Nguồn và đích đều mất: job fail với prefix `SOURCE_AND_TARGET_NOT_FOUND` và được retry có backoff. Khi số lần fail đạt `tiering_missing_segment_max_attempts` **và** pool nguồn đang đọc được (health gate + thư mục gốc tồn tại), segment bị loại khỏi range: range đích giảm `segment_count`/`size_bytes`, TimeBlock của segment bị xóa, ghi cảnh báo `TIER_SEGMENT_MISSING`. Nếu mọi segment của range đều mất, range chuyển `MISSING`.

### 5.6 Đối Soát Range Với File Thực Tế

`runRangeFileReconcile()` quét range `AVAILABLE` từ mới về cũ trong giới hạn `tier_reconcile_work_budget_ms` mỗi cycle, bằng con trỏ `(start_time, range_id)` giữ trong bộ nhớ (khởi động lại thì quét lại từ range mới nhất). Chỉ xét range thuộc pool filesystem đọc được (health gate + thư mục dữ liệu tồn tại) và không có job `PENDING/RUNNING`.

Với mỗi range, `reconcileRangeFiles()` liệt kê segment theo timefile và `stat` các segment đã kết thúc quá `tier_reconcile_min_age_seconds`. Segment mất file (ENOENT) bị bỏ:

- Range được dựng lại bằng các segment còn file: tách tại đoạn mất, thu hẹp khung sát segment, piece đầu giữ `range_id` (qua `replaceRangeWithPiecesIfUnchanged`, override được kế thừa). Không còn segment nào thì range chuyển `MISSING`.
- TimeBlock của segment mất bị gỡ khỏi timefile và ghi cảnh báo `TIER_SEGMENT_MISSING`.
- Lỗi `stat` khác ENOENT hoặc lỗi đọc timefile thì bỏ qua range ở lượt này.

Khoảng trống ≤ 10 giây (`kSegmentMergeGapSeconds`) vẫn được coi là liền mạch như mọi luồng compact khác; đoạn mất file dài hơn luôn tách range.

#### Đối soát range trên object storage

Pool object storage không `stat` được, nên `runObjectRangeReconcile()` (chạy ngay sau `runRangeFileReconcile()`) đối soát bằng `ListObjectsV2`, theo `storage.tier_object_reconcile_mode`:

- `FIX` (mặc định): bỏ segment mà object đã mất, giống đối soát file (tách/thu hẹp range hoặc chuyển `MISSING`, gỡ TimeBlock, cảnh báo `TIER_SEGMENT_MISSING` với "missing from object storage") và xoá cache usage `QUOTA` của pool.
- `REPORT`: chỉ ghi `TIER_OBJECT_MISSING` (một lần/range/tiến trình), không đổi range.
- `OFF`: không chạy. Giá trị không hợp lệ được hiểu là `REPORT`.

Range do restore xếp hàng (mục 8.6) được đối soát trước, rồi mới tới lượt quét. Mỗi pool object (enabled, qua health gate, đăng ký được client) có con trỏ riêng `(end_time, range_id)` giảm dần trên index `(pool_id, tier, status, end_time)`; các pool chia chung `tier_object_reconcile_work_budget_ms` (mặc định 2 000 ms), pool bắt đầu được xoay vòng mỗi cycle. Bỏ qua range bắt đầu gần hơn `tier_reconcile_min_age_seconds` và range có job `PENDING/RUNNING` (kiểm lại ngay trước khi sửa).

Với mỗi range, `checkObjectRange()`:

1. Đọc segment từ timefile, dựng key bằng `makeS3Key(base_path, ...)`.
2. List các day prefix `base_path/camera/stream/YYYY-MM-DD/` của segment đã kết thúc quá `tier_reconcile_min_age_seconds` (range tách ở nửa đêm địa phương nên thường 1–2 prefix). List lỗi (bất kỳ trang nào) thì dừng pool ở lượt này, không đổi gì và không tiến con trỏ.
3. Key không có trong listing chỉ bị coi là mất khi `headSegment()` trả `ABSENT` (HTTP 404); `PRESENT` thì giữ (listing trễ), lỗi khác thì dừng như trên.
4. Object có size khác `file_size` của TimeBlock chỉ ghi `TIER_OBJECT_SIZE_MISMATCH` (một lần/range/tiến trình), không đổi range.

Chặn mất hàng loạt (mass-loss guard): range mất toàn bộ object không bị sửa ngay mà chờ trong bộ nhớ đến khi pool chứng minh cấu hình đúng, tức là gặp một range còn ít nhất một object (lúc đó các range chờ được áp dụng), hoặc hết lượt quét mà đã từng gặp object. Nếu `tier_object_reconcile_mass_loss_threshold` (mặc định 20) range liên tiếp mất toàn bộ, hoặc hết lượt quét mà pool không có object nào, backend coi là cấu hình sai (đổi `base_path`, sai bucket/credential, bucket bị reset): không đổi gì, ghi `POOL_OBJECTS_MISSING_SUSPECT` (ERROR) và dừng đối soát pool đó `tier_object_reconcile_pause_seconds` (mặc định 3 600 s). Đổi endpoint/bucket/`base_path` của pool xoá toàn bộ trạng thái đang chờ. Hệ quả: pool chỉ có vài range và đã mất hết object luôn bị giữ nguyên kèm cảnh báo, cần người vận hành xác nhận.

Bucket bật versioning: object đã xoá (delete marker) không có trong `ListObjectsV2` nên được coi là mất đúng như mong muốn.

#### Báo cáo object mồ côi

Sau lượt đối soát range, mỗi pool object còn dùng được (không bị pause) chạy `runObjectOrphanSweep()` trong `tier_object_orphan_work_budget_ms` (mặc định 1 000 ms, `0` tắt), tối đa một lượt mỗi `tier_object_orphan_sweep_interval_seconds` (mặc định 1 ngày):

1. Duyệt `base_path/` bằng `listCommonPrefixes` xuống `camera/stream/YYYY-MM-DD/`; thư mục cấp ngày không đúng dạng ngày bị bỏ qua. Trạng thái lượt quét giữ trong bộ nhớ, list lỗi thì thử lại prefix đó ở cycle sau.
2. Với mỗi day prefix: list object, lấy các range `AVAILABLE/RESTORING/EXPIRED` của pool cho camera/stream đó chồng lên ngày (±1 giờ), đọc segment từ timefile để dựng tập key sở hữu. Timefile đọc lỗi thì coi cả ngày là có chủ.
3. Object không thuộc tập đó và có `LastModified` cũ hơn `tier_object_orphan_min_age_seconds` (mặc định 1 ngày, vì upload ghi object trước khi range chuyển sang pool) được đếm là mồ côi.
4. Hết lượt: nếu có, ghi một cảnh báo `POOL_ORPHAN_OBJECTS` (WARNING) với số object, tổng byte và key đầu tiên.

Báo cáo không bao giờ xoá object. Object mồ côi thường đến từ lỗi xoá giữa chừng, upload bị huỷ sau khi đã ghi object, hoặc dữ liệu ghi ngoài hệ thống. Người vận hành tự xác nhận rồi xoá.

### 5.7 Move Sang Object Storage

Nếu target pool là object storage (API hiện cho phép `MINIO`):

```text
resolve local_path
  -> require source_pool_id non-empty và source pool có mount_path/network_path
  -> s3_key = base_path/camera_id/stream_id/segment_path.mp4
  -> uploadSegment(pool_id, local_path, s3_key)
  -> update SegmentTierRange:
       tier = COLD
       pool_id = cold_pool_id
       status = AVAILABLE
  -> unlink local file sau khi upload thành công
  -> update tiering job
```

Nếu không resolve được source pool hoặc local path từ `pool_id`, job fail ngay; backend không đoán lại path bằng `kMP4SavePath`.

Quy ước object key:

```text
{base_path}/{camera_id}/{stream_id}/{segment_path}.mp4
```

Ví dụ:

```text
traffic/cam-003/main/2026-06-27/10-00-00.mp4
```

## 6. Luồng Timeline Và Summary

### 6.1 Timeline

FE gọi:

```text
GET/POST /media/mserver/storage/camera/timeline
```

Backend ưu tiên đọc `segment_tier_ranges` và trả:

- `tier`
- `pool_id`
- `status`
- `restore_required = true` nếu `pool_id` trỏ tới object-storage pool; COLD trên NAS không cần restore
- `size_bytes`
- `segment_count`

FE dùng response này để vẽ timeline HOT/WARM/COLD.

Nếu không có metadata trong `segment_tier_ranges`, timeline trả `ranges=[]`; `playback/resolve` trả `914016` (`CODE_STORAGE_SEGMENTS_NOT_FOUND`). Backend không tạo range giả `HOT` với `pool_id` rỗng.

### 6.2 Camera Summary

FE gọi:

```text
GET/POST /media/mserver/storage/camera/summary
```

Backend aggregate theo cặp tier/pool từ `segment_tier_ranges`, bỏ `DELETED` và `MISSING`.

## 7. Luồng Playback Hiện Tại

Playback có hai lớp resolve khác nhau:

1. API `/media/mserver/storage/playback/resolve` trả trạng thái tier/restore cho FE.
2. `Manager::resolveRecordedBlockPath()` resolve đường dẫn MP4 thực tế từ `TimeBlock` cho luồng replay/extract nội bộ.

### 7.1 API Resolve Trạng Thái

FE gọi:

```text
POST /media/mserver/storage/playback/resolve
```

Các input được handler đọc:

- `camera_id`, `start_time`, `end_time`: bắt buộc.
- `stream_id`: optional, dùng để lọc range.
- `protocol`: optional, mặc định `HTTP_MP4`.
- `quality`: hiện không được handler đọc.

Luồng xử lý:

```text
camera/time window
  -> kiểm tra quyền truy cập camera
  -> query segment_tier_ranges
  -> filter stream_id và overlap
  -> kiểm tra EXPIRED/DELETED/MISSING
  -> resolve pool type của từng range
  -> query restore_jobs/restore_job_segments đúng stream và overlap
  -> xác minh child DONE, cache file tồn tại và TTL còn hiệu lực
  -> trả READY / RESTORE_REQUIRED / RESTORING / NOT_FOUND / EXPIRED
```

Trạng thái và mã lỗi:

| Trạng thái | Điều kiện | API code / HTTP |
|------------|-----------|-----------------|
| `READY` | Không có object-storage range cần restore, hoặc có job `DONE` phủ toàn window với đủ child/cache đúng stream và TTL còn hiệu lực | `0` / 200 |
| `RESTORE_REQUIRED` | Có object-storage range, chưa có job `DONE` phủ window | `914009` / 404 |
| `RESTORING` | Có job `PENDING` hoặc `RUNNING` overlap | `914009` / 404 |
| `NOT_FOUND` | Không có range hoặc range `MISSING` | `914016` / 404 |
| `EXPIRED` | Có range `EXPIRED` | `914017` / 404 |
| `DELETED` | Có range `DELETED` | `914016` / 404 |

API này không tạo `playback_url`. Với restore cache, `expires_at` là `restore_job.updated_at + storage.restore_ttl_seconds`; HOT/WARM hoặc TTL bị vô hiệu hóa trả `expires_at=0`. URL phát vẫn do luồng playback hiện hữu của hệ thống cung cấp.

### 7.2 Resolver Đường Dẫn MP4 Thực Tế

`server/Manager.cpp` có helper:

```cpp
static std::string resolveRecordedBlockPath(const TimeBlock &block);
```

Helper được dùng tại:

- Listener `Broadcast::kBroadcastMediaSeeked2`: dựng map file cho seek/replay MP4.
- Listener `Broadcast::kBroadcastGetRecordedMP4`: dựng map file cho consumer nội bộ như extraction.

Luồng resolve:

```text
TimeBlock.file_path
  -> decodeBase64
  -> resolvePlaybackSegmentPath(
       block.app,
       block.stream,
       block.start_time,
       timefile_path)
```

`TierStorageManager::resolvePlaybackSegmentPath(...)` xử lý:

1. Nếu camera/stream/timestamp không hợp lệ:
   - `fallback=true`.
   - Trả lại `timefile_path` nếu path không rỗng.
2. Nếu không derive được `segment_path` hoặc không tìm thấy range:
   - Fallback về `timefile_path`.
3. Nếu range là `DELETED`, `EXPIRED` hoặc `MISSING`:
   - Không chọn range đó; có thể fallback path nếu không còn match hợp lệ.
4. Nếu `pool_id` không có trong cache hoặc backend không resolve được:
   - Fallback về `timefile_path`.
5. Nếu pool là `LOCAL_DISK`/`NAS`:
   - Dựng storage key `camera_id/stream_id/segment_path.mp4`.
   - Resolve path theo root đã register của pool.
6. Nếu pool là object storage:
   - Kiểm tra file trong restore cache.
   - Có cache: `ready=true`, trả cache path.
   - Chưa có cache: `restore_required=true`, `ready=false`, không trả read path.

Khi một block chưa ready, `Manager` không block chờ download; block đó không được đưa vào map file trả cho invoker. Client/consumer phải retry sau khi restore hoàn tất.

## 8. Luồng Auto-Restore COLD Đã Triển Khai

Restore hiện không hook `HttpFileManager::accessFile` và không trả HTTP `202` khi static-file miss. Điểm kích hoạt thực tế nằm trong `Manager::resolveRecordedBlockPath()`: khi resolver báo `restore_required`, Manager gọi:

```cpp
TierStorageManager::Instance().handleColdAccessByPath(timefile_path);
```

### 8.1 Điều Kiện Kích Hoạt

`handleColdAccessByPath()` chỉ xử lý nếu:

- Build có `ENABLE_TIER_STORAGE`.
- `storage.auto_restore_on_record_access=1`.
- `file_path` parse được theo layout record thành camera, stream, segment path và timestamp.
- Tìm thấy range COLD overlap segment.
- Range không ở trạng thái `DELETED`/`EXPIRED`.
- Range có `pool_id` và pool là object storage.
- Pool nguồn có đủ cấu hình để worker đăng ký object-storage client và download segment.

Nếu các điều kiện trước bước đăng ký pool không thỏa, helper trả `handled=false` và không tạo job. Riêng đăng ký object storage được thử trước khi enqueue nhưng kết quả chưa được kiểm tra tại đây; job vẫn có thể được tạo rồi chuyển `FAILED` trong worker nếu đăng ký lại không thành công.

### 8.2 Parse Segment Identity

Input là absolute path gốc lưu trong `TimeBlock.file_path`. Parser lấy:

```text
camera_id
stream_id
segment_path       # không gồm .mp4
segment_start      # parse từ tên segment
```

Parser hiện yêu cầu path nằm dưới record root cấu hình bởi `Protocol::kMP4SavePath` và `Record::kAppName`. Nếu camera record root nằm ngoài layout này hoặc tên segment không parse được timestamp, auto-restore không được kích hoạt.

### 8.3 Chống Tạo Trùng Job

Trước khi tạo job, backend query:

```text
restore_jobs
  where camera_id = ?
    and status in (PENDING, RUNNING)
    and job.start_time <= segment_start
    and job.end_time >= segment_start
```

Nếu có job:

- Không tạo job mới.
- Trả lại `job_id` và status hiện tại.
- Không dispatch thêm worker.

Auto-restore tái sử dụng child đang active của cùng segment. `restoreJob/create` tái sử dụng job có cùng camera, stream, window, target tier và tập segment/source pool; request khác thông số vẫn tạo job độc lập. Cả hai luồng dùng chung durable child queue và giới hạn concurrency.

### 8.4 Tạo Parent/Child Job

Nếu chưa có job overlap:

```text
job_id       = UUID prefix rj
source_tier  = COLD
target_tier  = HOT
status       = PENDING
start_time   = segment_start
end_time     = segment_start + 60
reason       = AUTO_RECORD_ACCESS
```

Backend insert atomic một `restore_jobs` parent và một hay nhiều `restore_job_segments` child. Range COLD không bị split hoặc đổi status.

### 8.5 Thread Và Lifetime

Restore không chạy trên EventPoller/HTTP thread. Backend capture `weak_ptr<TierStorageManager>` rồi dispatch sang:

```cpp
WorkThreadPool::Instance().getExecutor()->async(...);
```

Worker lock weak pointer trước khi gọi `executeRestoreSegment(...)`, tránh giữ manager sống cưỡng bức hoặc capture raw `this`.

### 8.6 Restore Worker

Luồng thực thi:

```text
claim RestoreJobSegment PENDING -> RUNNING
  -> đảm bảo source object pool đã register
  -> key = makeS3Key(base_path, camera_id, stream_id, segment_path)
  -> restore_path = buildRestoreSegmentPath(...)
  -> tạo restore directory
  -> nếu restore_path đã tồn tại:
       update child DONE bằng file size
       tổng hợp trạng thái/progress parent
       return
  -> download object vào <restore_path>.part
  -> rename atomic sang <restore_path>
  -> stat file lấy processed_bytes
  -> update child DONE
  -> tổng hợp trạng thái/progress parent
```

Khi register/download/mkdir/rename lỗi:

- Child chuyển `FAILED`, parent được tổng hợp thành `FAILED` và ghi `error_message`.
- Range COLD vẫn giữ `AVAILABLE` vì trạng thái restore thuộc durable child queue.
- Partial file `.part` trong thư mục job được xóa ở các nhánh download/rename thất bại.
- Download lỗi thì worker `HeadObject` lại key: chỉ khi HTTP 404 (`ABSENT`) child ghi lỗi "Object is missing from object storage" và range được xếp vào hàng ưu tiên của đối soát object (`requestObjectRangeReconcile`, tối đa 32 range/cycle, chạy trước lượt quét ở cycle kế tiếp, xem 5.6). Restore không tự đổi range; mọi thay đổi vẫn đi qua đối soát và mass-loss guard.

### 8.7 Object Key Và Restore Path

Object key:

```text
{base_path}/{camera_id}/{stream_id}/{segment_path}.mp4
```

Restore cache:

```text
{storage.restore_save_path}/{job_id}/{camera_id}/{stream_id}/{segment_path}.mp4
```

File tạm:

```text
{restore_path}.part
```

Mỗi job có marker `.restore_job` trong thư mục gốc của nó. Hai job có thể chứa bản sao riêng của cùng một segment. Range sau restore vẫn giữ `tier=COLD` và `pool_id` của object pool. Chỉ status trở về `AVAILABLE`; file local là cache tạm, không phải dữ liệu HOT chính thức.

### 8.8 Cleanup Cache

Mỗi timer tick, `cleanupRestoreTempFiles()`:

1. Lấy tối đa 100 job `DONE`/`FAILED`/`CANCELLED` có `updated_at <= now - storage.restore_ttl_seconds`.
2. Xóa toàn bộ thư mục `{storage.restore_save_path}/{job_id}` của đúng job đã hết TTL, gồm cache, `.part` và marker.
3. Chỉ chuyển job sang `EXPIRED` khi thư mục job đã xóa hoặc không còn tồn tại. Lỗi xóa giữ nguyên status/`updated_at` để retry ở cycle sau.
4. Với job tạo trước khi áp dụng layout theo `job_id`, cleanup vẫn xóa exact child cache và partial `.restore.<job_id>` theo layout cũ.
5. Scan và xóa partial `.restore.*` legacy bị bỏ dở quá TTL; lỗi xóa cũng được retry ở lần scan sau.

`EXPIRED` là status runtime thực tế của restore job sau cleanup, dù enum `JobStatus` dùng chung chưa khai báo giá trị này.

### 8.9 Cleanup MP4 Không Có Trong TimeFile

`MP4Recorder` ghi segment đang mở bằng tên ẩn `.{filename}.mp4` và chỉ rename sang tên chính thức sau khi đóng file thành công. Nếu tiến trình dừng bất thường, file tạm có thể còn lại nhưng không xuất hiện trong TimeFile, vì vậy backfill và retention dựa trên tier metadata không nhìn thấy file này.

Mỗi maintenance cycle, `cleanupOrphanTemporarySegments()` scan trực tiếp các file-backed HOT pool đang bật để dọn cả file tạm và MP4 hoàn chỉnh không có TimeBlock:

```text
{pool-data-root}/{camera}/{stream}/{YYYY-MM-DD}/[.]{segment}.mp4
```

Quy tắc an toàn:

1. Chỉ xét regular file kết thúc bằng `.mp4` và parse được timestamp segment; tên bắt đầu bằng `.` được phân loại là file tạm.
2. Không đi theo symlink directory và không xóa symlink file.
3. Chỉ scan HOT pool đang sẵn sàng cho thao tác xóa; pool disabled, `UNKNOWN`, `OFFLINE` hoặc có health snapshot stale được bỏ qua toàn bộ.
4. Ngưỡng cleanup của camera là thời điểm hiện tại trừ retention nhỏ hơn giữa `HOT.retain_until_days` và `keepArchivedMaxFor` khi camera cấu hình giá trị này thủ công (`keepArchivedMaxForAuto=false`, giá trị lớn hơn 0). Khi không có statistic camera, chỉ dùng retention HOT của policy hiệu lực. Policy bị tắt hoặc không có HOT tier hợp lệ thì bỏ qua camera.
5. Quy đổi ngưỡng cleanup về đầu ngày và chỉ scan folder có ngày nhỏ hơn ngày ngưỡng. Folder của chính ngày biên được bỏ qua toàn bộ, tránh phải duyệt và so sánh từng file trong folder này.
6. File tạm chỉ bị xóa khi hai maintenance pass liên tiếp thấy cùng device, inode, size và mtime. Quan sát này chỉ nằm trong bộ nhớ, nên restart luôn bắt đầu lại từ pass đầu tiên.
7. MP4 hoàn chỉnh không cần hai lần quan sát. Cleanup dùng `TimeQuery` ở cấp camera để truy vấn một khoảng gộp cho nhóm camera/stream/ngày trên toàn bộ TimeFile còn tồn tại, rồi giữ file có TimeBlock khớp chính xác đường dẫn hoặc định danh segment. Việc thiếu riêng TimeFile của ngày chứa MP4 không làm nhóm bị bỏ qua.
8. Nếu camera không có TimeFile, `TimeQuery` trả timeline rỗng; MP4 hoàn chỉnh không có TimeBlock khớp được xem là orphan và tiếp tục cleanup. Lỗi mở/đọc TimeFile khác vẫn được xử lý theo cơ chế an toàn hiện tại và không bị đồng nhất với trạng thái camera không có dữ liệu.
9. Ngay trước `unlink`, cleanup kiểm tra lại MP4 bằng `lstat` và xác nhận device, inode, size và mtime chưa thay đổi.
10. Điểm bắt đầu pool và camera được xoay vòng giữa các pass để camera ở cuối danh sách không bị bỏ đói khi hết budget.
11. File được cleanup không được thêm vào `segment_tier_ranges`, không move và không upload sang tier khác. MP4 có TimeBlock được giữ lại để `runTieringCycle()` move hoặc xóa theo policy.

Các cấu hình:

| Key | Mặc định | Ý nghĩa |
|---|---:|---|
| `storage.tier_tmp_cleanup_enabled` | `1` | Bật pass phát hiện MP4 tạm trong HOT filesystem pool |
| `storage.tier_tmp_cleanup_dry_run` | `1` | Chỉ đếm/log `would_delete`, chưa xóa file |
| `storage.tier_tmp_cleanup_max_files_per_cycle` | `1000` | Số regular file tối đa được kiểm tra trong một pass |
| `storage.tier_tmp_cleanup_work_budget_ms` | `500` | Ngân sách thời gian tối đa của một pass |

Rollout nên giữ `dry_run=1` ít nhất hai maintenance cycle để kiểm tra counter `would_delete_files`, sau đó đặt `dry_run=0` để xóa thật. Có thể rollback ngay bằng `tier_tmp_cleanup_enabled=0`; cơ chế này không có schema hoặc metadata cần rollback.

## 9. Luồng Restore Tích Hợp Với API

### 9.1 API Resolve Và Quan Sát Job

Luồng đang hoạt động:

```text
FE -> playback/resolve
BE -> READY hoặc RESTORE_REQUIRED/RESTORING

FE đã xác định range COLD từ timeline -> restoreJob/create
BE -> tái sử dụng job tương đương hoặc tạo job mới và bắt đầu download ngay
FE -> poll restoreJob/detail
BE -> RESTORING/READY/NOT_READY/FAILED/CANCELLED/EXPIRED
FE nhận READY -> gọi API playback hiện hữu

Luồng replay nội bộ -> resolveRecordedBlockPath
BE -> handleColdAccessByPath
BE -> tạo job + dispatch worker nếu auto_restore bật

FE -> restoreJob/list hoặc restoreJob/detail
BE -> PENDING/RUNNING/DONE/FAILED/EXPIRED

FE/consumer -> retry playback/replay
BE -> resolve cache path khi job DONE và file cache còn TTL
```

Các endpoint restore job yêu cầu đồng thời permission playback `1003` và read MediaServer `150301`.

### 9.2 Hành Vi Của `restoreJob/create`

`POST /media/mserver/storage/restoreJob/create` hiện:

- Validate camera/time window.
- Resolve các segment COLD/AVAILABLE chính xác từ timefile.
- Insert atomic parent `restore_jobs` và các child `restore_job_segments`.
- Dispatch các child theo giới hạn `storage.auto_restore_max_concurrent`.
- Trả ngay snapshot của job gồm `job_id`, trạng thái playback và tiến độ theo số segment; không trả estimate thời gian restore.

Manual restore và auto-restore dùng chung durable queue. Manual request tương đương tái sử dụng job `PENDING`/`RUNNING`; job `DONE` có cache đầy đủ được cập nhật `updated_at` để gia hạn TTL. Job terminal lỗi, đã hết hạn hoặc thiếu cache không được tái sử dụng. Request khác thông số vẫn có thể tải cùng object vào folder job khác để vòng đời cache, TTL và cleanup độc lập.

### 9.3 Không Có HTTP Static-File Hook

Code hiện không intercept `HttpFileManager::accessFile` hoặc `HttpSession::checkLiveStreamByApp` cho tiering. Do đó:

- Static request tới file đã move khỏi local vẫn theo behavior file-not-found hiện hữu.
- Backend không trả `202 + Retry-After` cho direct URL.
- Auto-restore được kích hoạt khi luồng replay/extraction resolve `TimeBlock`, không phải khi HTTP static file bị miss.

Nếu bổ sung direct-file restore sau này, cần thiết kế mapping URL -> camera/stream/timestamp, response contract, chống retry storm và Range request riêng; đây chưa phải behavior hiện tại.

## 10. Trạng Thái Segment Và Job

### 10.1 Segment Status

```text
AVAILABLE  : có thể phát hoặc restore
RESTORING  : giá trị legacy; restore mới được biểu diễn ở `restore_job_segments`
EXPIRED    : hết hạn, chờ duyệt xóa
DELETED    : đã xóa
MISSING    : metadata còn nhưng file/object mất
```

### 10.2 Restore Job Status

```text
PENDING    : đã tạo, chưa chạy
RUNNING    : worker đang download
DONE       : cache đã sẵn sàng
FAILED     : restore lỗi, có error_message
CANCELLED  : trạng thái trong model dùng chung
EXPIRED    : cache của job DONE đã bị cleanup theo TTL
```

Progress:

```text
status == DONE                         -> 100
total_bytes <= 0 && processed_bytes>0  -> 100
total_bytes <= 0                       -> 0
ngược lại                            -> clamp(processed_bytes * 100 / total_bytes, 0, 100)
```

Khi `DONE`:

```text
playback_ready = true
```

## 11. Dashboard Và Quan Sát Hệ Thống

Dashboard lấy các API đã đăng ký:

- `/media/mserver/storage/dashboard/summary`
- `/media/mserver/storage/dashboard/detail`
- `/media/mserver/storage/pool/list`
- `/media/mserver/storage/camera/summary/list`
- `/media/mserver/storage/tieringJob/list`
- `/media/mserver/storage/restoreJob/list`
- `/media/mserver/storage/alert/list`
- `/media/mserver/storage/expiredSegment/list`
- `/media/mserver/storage/protected/list`

Các thông tin cần hiển thị:

- Tổng dung lượng, used/free/percent.
- Tình trạng từng tier.
- Active/failed tiering jobs.
- Active restore jobs.
- Cảnh báo pool high/critical/offline.
- Expired segments chờ duyệt.
- Protected/evidence video ranges.

`dashboard/summary` dùng response gọn cho FE. Các field `write_mbps`, `estimated_remaining_days`, `camera_count` hiện là placeholder `0`. `dashboard/detail` trả model raw gồm summary, pools, policies, configured camera assignments, job counts và 20 tiering job gần nhất.

## 12. Cấu Hình Và Luồng Gọi Nội Bộ

### 12.1 Backend Config

Các cấu hình và giá trị mặc định trong `src/Common/config.cpp`:

```ini
storage.auto_restore_on_record_access=0
storage.auto_restore_max_concurrent=5
storage.restore_save_path=/dev/shm/restore       # Linux
# storage.restore_save_path=./www/restore        # Nền tảng khác
storage.restore_ttl_seconds=3600
storage.object_read_mode=RESTORE
storage.presigned_url_ttl_seconds=3600
storage.presigned_extract_max_segments=0
storage.presigned_extract_max_bytes=0
storage.default_hot_high_watermark_percent=85
storage.default_hot_critical_watermark_percent=95
storage.tiering_max_ranges_per_camera_cycle=64
storage.tiering_missing_segment_max_attempts=3
storage.tiering_missing_segment_retry_base_seconds=3600
storage.tiering_pressure_delete_margin_percent=3
storage.tiering_quota_usage_cache_seconds=30
storage.tiering_object_pressure_cooldown_seconds=900
storage.tiering_pressure_delete_mode=BALANCED
storage.tier_reconcile_enabled=1
storage.tier_reconcile_work_budget_ms=2000
storage.tier_reconcile_min_age_seconds=600
storage.tier_object_reconcile_mode=FIX
storage.tier_object_reconcile_work_budget_ms=2000
storage.tier_object_reconcile_mass_loss_threshold=20
storage.tier_object_reconcile_pause_seconds=3600
storage.tier_object_orphan_work_budget_ms=1000
storage.tier_object_orphan_min_age_seconds=86400
storage.tier_object_orphan_sweep_interval_seconds=86400
storage.tiering_split_granularity_seconds=3600
storage.tiering_transfer_max_concurrent=4
storage.tiering_transfer_max_concurrent_per_pool=2
storage.tiering_job_retention_days=30
storage.tiering_terminal_range_retention_days=30
storage.tier_range_backfill_enabled=1
storage.tier_range_backfill_days=180
storage.tier_range_backfill_chunk_seconds=86400
storage.tier_range_backfill_max_cameras_per_cycle=20
storage.tier_range_backfill_dispatch_interval_seconds=1
storage.tier_range_backfill_work_budget_ms=2000
storage.tier_range_backfill_max_attempts=5
storage.tier_range_backfill_retry_base_seconds=30
storage.pool_health_check_interval_seconds=30
storage.pool_health_failure_threshold=2
storage.pool_health_recovery_threshold=2
storage.pool_health_stale_after_seconds=90
storage.pool_health_metric_snapshot_interval_seconds=300
globalmonitor.mount_refresh_interval_seconds=10
globalmonitor.mount_snapshot_stale_after_seconds=30
```

| Biến | Nơi dùng | Ghi chú |
|------|----------|---------|
| `auto_restore_on_record_access` | `handleColdAccessByPath()` | Phải bật mới tạo auto-restore job |
| `auto_restore_max_concurrent` | `processRestoreJobs()` | Giới hạn số child restore chạy đồng thời |
| `restore_save_path` | `buildRestoreSegmentPath()` | Root cache restore tạm |
| `restore_ttl_seconds` | API resolve và cleanup | Tính `expires_at`, xóa exact child cache của job terminal cũ |
| `object_read_mode` | Thumbnail và persistent extraction | `RESTORE`, `PRESIGNED` hoặc `HYBRID`; mặc định giữ cơ chế restore cũ |
| `presigned_url_ttl_seconds` | Object read resolver | TTL URL tạm, tối đa 7 ngày và phải đủ dài cho extraction |
| `presigned_extract_max_segments` | HYBRID extraction | Số object tối đa cho direct read; `0` tắt direct extraction |
| `presigned_extract_max_bytes` | HYBRID extraction | Tổng byte tối đa cho direct read; `0` tắt direct extraction |
| `default_hot_*_watermark_percent` | `ensureDefaultHotPool()` | Watermark cho `pool-default-hot` |
| `tiering_max_ranges_per_camera_cycle` | Tiering/retention | Giới hạn số range eligible xử lý cho mỗi camera/tier/cycle |
| `tiering_missing_segment_max_attempts` | Tier transfer | Số lần job fail vì segment mất ở cả nguồn và đích trước khi segment bị loại khỏi range |
| `tiering_missing_segment_retry_base_seconds` | `queueTierMoveJob()` | Backoff cơ sở cho range có segment mất, nhân đôi mỗi lần, tối đa 1 ngày |
| `tiering_pressure_delete_margin_percent` | `enforcePoolPressureRetention()` | Phần trăm vượt critical cho phép xóa dù pool còn job move đang chờ |
| `tiering_quota_usage_cache_seconds` | `quotaUsedBytes()` | Thời gian dùng lại tổng byte range của pool `QUOTA`; `0` tắt cache |
| `tiering_object_pressure_cooldown_seconds` | `enforcePoolPressureRetention()` | Thời gian chờ giữa hai lượt xóa theo áp lực trên pool object storage `METRICS`; `0` tắt cooldown |
| `tiering_pressure_delete_mode` | `enforcePoolPressureRetention()`, `planBalancedPressureMoves()` | `BALANCED` (mặc định): khi xóa và khi move sớm, mọi camera chịu cùng tỉ lệ cửa sổ lưu trữ (khi xóa, camera không còn recorder trước); `OLDEST`: hành vi cũ, lấy range cũ nhất |
| `tier_reconcile_enabled` | Backfill, `runRangeFileReconcile()` | Bật/tắt việc bỏ segment/TimeBlock không còn file MP4 |
| `tier_reconcile_work_budget_ms` | `runRangeFileReconcile()` | Thời gian tối đa cho đối soát mỗi cycle |
| `tier_reconcile_min_age_seconds` | Backfill, `runRangeFileReconcile()`, `runObjectRangeReconcile()` | Segment kết thúc gần hơn ngưỡng này không bị coi là mất file/object |
| `tier_object_reconcile_mode` | `runObjectRangeReconcile()` | `FIX` (mặc định) bỏ segment có object mất; `REPORT` chỉ cảnh báo `TIER_OBJECT_MISSING`; `OFF` tắt |
| `tier_object_reconcile_work_budget_ms` | `runObjectRangeReconcile()` | Thời gian tối đa đối soát object storage mỗi cycle (chung cho mọi pool object) |
| `tier_object_reconcile_mass_loss_threshold` | `runObjectRangeReconcile()` | Số range liên tiếp mất toàn bộ object bị coi là cấu hình sai (`POOL_OBJECTS_MISSING_SUSPECT`) |
| `tier_object_reconcile_pause_seconds` | `runObjectRangeReconcile()` | Thời gian dừng đối soát pool sau khi mass-loss guard kích hoạt |
| `tier_object_orphan_work_budget_ms` | `runObjectOrphanSweep()` | Thời gian tối đa cho báo cáo object mồ côi mỗi cycle; `0` tắt |
| `tier_object_orphan_min_age_seconds` | `runObjectOrphanSweep()` | Object mới hơn ngưỡng này không bị coi là mồ côi |
| `tier_object_orphan_sweep_interval_seconds` | `runObjectOrphanSweep()` | Khoảng cách giữa hai lượt báo cáo của cùng một pool |
| `tiering_split_granularity_seconds` | Move theo tuổi, retention theo tuổi | Độ mịn làm tròn mốc; `<= 1` tắt làm tròn |
| `tiering_transfer_max_concurrent` | Transfer dispatcher | Giới hạn tổng job copy/upload chạy đồng thời |
| `tiering_transfer_max_concurrent_per_pool` | Transfer dispatcher | Giới hạn job đồng thời cùng chạm một source/target pool |
| `tiering_job_retention_days` | `pruneOldMetrics()` | Xóa lịch sử job `DONE`/`FAILED`/`CANCELLED` cũ |
| `tiering_terminal_range_retention_days` | `pruneOldMetrics()` | Xóa range `DELETED`/`MISSING` cũ (không xóa `EXPIRED`) |
| `tier_range_backfill_enabled` | Backfill dispatcher | Bật/tắt durable backfill |
| `tier_range_backfill_days` | Đồng bộ task | Cửa sổ lịch sử cố định khi tạo/reset task |
| `tier_range_backfill_chunk_seconds` | Backfill worker | Khoảng timeline commit nguyên tử mỗi chunk |
| `tier_range_backfill_max_cameras_per_cycle` | Backfill dispatcher | Số task tối đa mỗi lượt dispatch |
| `tier_range_backfill_dispatch_interval_seconds` | Backfill timer | Chu kỳ đánh thức dispatcher |
| `tier_range_backfill_work_budget_ms` | Backfill dispatcher | Time budget tối đa mỗi lượt |
| `tier_range_backfill_max_attempts` | Backfill retry | Số lỗi tối đa trước khi chuyển `FAILED`; trạng thái `FAILED` giữ nguyên khi cửa sổ trượt |
| `tier_range_backfill_retry_base_seconds` | Backfill retry | Backoff cơ sở, tăng theo lũy thừa hai |
| `pool_health_check_interval_seconds` | Health timer | Chu kỳ probe nhẹ, độc lập với tiering cycle |
| `pool_health_failure_threshold` | Health state machine | Số lỗi liên tiếp trước khi chuyển `OFFLINE` |
| `pool_health_recovery_threshold` | Health state machine | Số lần thành công liên tiếp để phục hồi từ `OFFLINE` |
| `pool_health_stale_after_seconds` | Playback health gate | Snapshot cũ hơn ngưỡng được coi là `UNKNOWN` |
| `pool_health_metric_snapshot_interval_seconds` | `pool_metrics` | Chu kỳ ghi snapshot khi trạng thái không đổi |
| `globalmonitor.mount_refresh_interval_seconds` | `HddMonitor` | Chu kỳ quét lại toàn bộ mount inventory để phát hiện mount/unmount |
| `globalmonitor.mount_snapshot_stale_after_seconds` | Mount lookup | Snapshot quá ngưỡng sẽ được refresh trực tiếp trước khi probe NAS |

`tier_backfill_tasks` lưu cursor và các counter `scanned_blocks`, `registered_blocks`, `orphan_blocks`, `indexed_bytes`. Khi task `FAILED`, có thể đưa lại về hàng đợi qua `TierBackfillTaskImp::retryFailed(camera_id)` sau khi đã xử lý nguyên nhân lỗi mở/đọc timefile. Block orphan không làm task fail; xem cảnh báo `TIER_BACKFILL_ORPHAN`.

`segment_tier_ranges` là bảng metadata chính của luồng tiering nên không bị cleanup bằng TTL ngắn hạn. Chỉ các range `DELETED/MISSING` được cleanup bằng retention riêng sau khi không còn cần cho timeline, thống kê hoặc audit.

`storage.restore_save_path` là thư mục restore cache tạm. `storage.restore_ttl_seconds` được áp dụng thông qua `restore_jobs`: job terminal quá TTL sẽ được đánh dấu `EXPIRED` sau khi xóa exact child cache tương ứng.

### 12.2 TierStorageManager API Nội Bộ

Hook nội bộ hiện có/phục vụ luồng autorestore:

```cpp
struct ColdAccessRestoreResult {
    bool handled = false;
    std::string job_id;
    std::string status;
    std::string message;
    int estimated_restore_seconds = 120;
};

ColdAccessRestoreResult TierStorageManager::handleColdAccessByPath(
    const std::string &file_path);
```

Logic:

```text
file_path -> segment identity
  -> query SegmentTierRange by camera_id, stream_id, timestamp
  -> if not found: handled=false
  -> if tier != COLD: handled=false
  -> nếu đã có child PENDING/RUNNING cùng segment: return existing job
  -> atomic create parent + child
  -> scheduler claim child theo concurrency limit
  -> handled=true
```

### 12.3 Playback Resolver Pseudocode

```cpp
auto resolved = TierStorageManager::Instance().resolvePlaybackSegmentPath(
    block.app(), block.stream(), block.start_time(), decoded_timefile_path);

if (resolved.ready && !resolved.read_path.empty()) {
    return resolved.read_path;
}

if (resolved.restore_required) {
    // Chỉ queue thực sự nếu auto_restore_on_record_access=1.
    auto restore = TierStorageManager::Instance()
        .handleColdAccessByPath(decoded_timefile_path);
    log(restore.job_id, restore.status);
}

// Block chưa ready bị bỏ khỏi file map; consumer retry sau.
return "";
```

### 12.4 Restore Worker Pseudocode

```cpp
void TierStorageManager::executeRestoreSegment(...) {
    child_imp.claimPending(job_id, stream_id, segment_path);

    pool = source_pool_from_cold_range.pool_id
    key = TierObjectStorage::makeS3Key(pool.base_path, camera_id, stream_id, segment_path)
    restore_path = storage.restore_save_path + "/" + job_id + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4"
    local_tmp = restore_path + ".part"

    ok = objectStorage.downloadSegment(pool.id, key, local_tmp)
    if (!ok) fail

    rename(local_tmp, restore_path)
    child_imp.updateResult(job_id, stream_id, segment_path,
                           "DONE", restored_bytes)
    aggregateParentRestoreJob(job_id)
}
```

## 13. Rủi Ro Và Lưu Ý

- Download được dispatch sang `WorkThreadPool`, không block EventPoller và được giới hạn bởi `storage.auto_restore_max_concurrent`.
- Manual restore và auto-restore đều được scheduler consume; manual request tương đương được deduplicate, còn request khác thông số có thể restore cùng segment độc lập và chịu chung giới hạn concurrency.
- Restore cache phải được cleanup theo `restore_jobs.updated_at + storage.restore_ttl_seconds`; cleanup chỉ xóa thư mục của job hết TTL rồi chuyển job đó sang `EXPIRED`.
- Worker dùng kết quả download/rename và file size; chưa verify checksum nội dung.
- Direct HTTP static-file miss, byte-range và HLS restore chưa được tích hợp.
- Object MinIO mất (xoá tay, ILM, reset bucket) được đối soát theo lượt quét (mục 5.6): quét hết ~90 nghìn range COLD (600 stream × 150 ngày, ~1–2 LIST/range) mất khoảng 1–3 ngày với budget 2 s mỗi cycle 300 s; range có restore thất bại vì object trả 404 được đối soát ngay ở cycle kế tiếp.
- Không đổi `base_path`/bucket của pool đang chứa dữ liệu: mọi key cũ sẽ lệch. Mass-loss guard chặn việc chuyển hàng loạt range sang `MISSING` trong trường hợp này, nhưng dữ liệu cũ vẫn không đọc được cho tới khi trả lại cấu hình.
- Pool `QUOTA` chỉ biết dữ liệu có range trong DB: object ghi ngoài hệ thống, upload đang chạy (range chưa chuyển sang pool) hoặc object còn sót sau lỗi xóa không được tính. Object còn sót quá 1 ngày được báo qua `POOL_ORPHAN_OBJECTS` (mục 5.6) nhưng không được tự xoá. Hard quota MinIO cao hơn quota logic khoảng 5% là lớp chặn cuối.
- Bucket MinIO bật versioning mà không có lifecycle `NoncurrentVersionExpiration` sẽ không bao giờ lấy lại dung lượng sau khi xóa; backend chỉ cảnh báo (`POOL_VERSIONING_BLOCKS_RECLAIM`), không tự sửa cấu hình bucket.
- Parser auto-restore phụ thuộc layout record root/tên segment; custom record root có thể không parse được.
- Bootstrap chọn HOT filesystem pool hợp lệ theo thứ tự xác định; chỉ tạo pool mặc định khi chưa có pool phù hợp.
- Demuxer TimeFile hiện coi lỗi `read()` như hết file; chỉ lỗi parse block được báo qua `TimeQuery::queryBlocks()`. Lỗi I/O giữa file vẫn có thể làm danh sách segment bị thiếu.
- Retention theo tuổi và xóa khi vượt ngưỡng quét tối đa 16 trang × `tiering_max_ranges_per_camera_cycle` range mỗi lượt; piece chứa video được bảo vệ vẫn nằm trong trang quét nên số lượng rất lớn có thể làm chậm việc xóa range mới hơn.
- Segment có cùng `start_time` không tách được theo quyền sở hữu; nếu một trong số đó chạm video bảo vệ thì cả nhóm được giữ.
- Rebuild timefile ghi ra file tạm rồi swap trên một luồng riêng ("timefile swap") khi file hết reader; swap thay file bằng `rename()` nguyên tử. Với file ngày cũ, nếu có block mới được ghi trong lúc chờ swap (ví dụ nhập từ thẻ SD), swap bị hủy để không mất block và file vẫn dirty cho lần rebuild sau. Dấu dirty chỉ được xóa sau khi swap thành công. File ngày hiện tại đang ghi dùng cơ chế mirror block mới sang file tạm.
- Gỡ TimeBlock là bất đồng bộ (swap khi file hết reader) và không bền qua restart. Block bỏ ở backfill mà chưa swap kịp sẽ còn trong timefile nhưng không có range; block trong range sẽ được vòng đối soát xử lý lại.
- `StorageManager` cũ (bản không có tier) xóa file trước rồi mới rebuild timefile; nếu tiến trình dừng giữa chừng, timefile giữ block của file đã xóa. Sau khi nâng cấp, backfill và vòng đối soát dọn các block này.

## 14. Phần Còn Thiếu Theo Code Hiện Tại

1. Quyết định có hỗ trợ direct HTTP static-file restore hay chỉ giữ resolver `TimeBlock`; nếu hỗ trợ cần contract HTTP mới.
2. Bổ sung checksum nội dung object; hiện chỉ so size với TimeBlock (`TIER_OBJECT_SIZE_MISMATCH`, không tự sửa).
3. Sửa parser record path để hỗ trợ camera record root do policy điều phối.
4. Bổ sung metrics/alert cho restore throughput, failure và queue depth; các metric dashboard placeholder hiện vẫn là `0`.

## 15. Quy Mô Tham Chiếu

Ước lượng cho 600 stream, segment MP4 1 phút, lưu 6 tháng, `tiering_split_granularity_seconds=3600`:

| Hạng mục | Khối lượng |
|---|---|
| Segment MP4 | ~155 triệu |
| Range `AVAILABLE` | ~108 nghìn (1 range/stream/ngày, cộng piece do split) |
| Job chuyển tầng | ~14 400/ngày (1/stream/giờ) |
| Range `DELETED` | ~14 400/ngày, giữ 30 ngày (~430 nghìn dòng) |
| Timefile | 1 file/camera/ngày, ~2 880 block (~400 KB) |

Migration `16_tier_performance_indexes.sql` thêm index cho các truy vấn nóng. Đo trên SQLite với 1,14 triệu range: `hasActiveJobForRange` 3,3 ms → 0,012 ms, `findDeletableByPoolAfter` 2 053 ms/trang → 2 ms, `findRetentionCandidatesAfter` 67 ms → 0,06 ms, `findReconcileCandidatesBefore` 15,6 ms → 0,5 ms.

## 16. Kiểm Cấu Hình Tier-Off

Repo **chưa có CI**, nên cấu hình `-DENABLE_TIER_STORAGE=OFF` không được build ở đâu tự động.
Nó đã từng hỏng đúng vì vậy: `manager/Local/TierFileStorageBase.cpp` thiếu guard
`#ifdef ENABLE_TIER_STORAGE` nên cả file được biên dịch khi tier tắt và không compile được.

Cách kiểm:

```bash
cmake -S . -B build-tier-off -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TIER_STORAGE=OFF -DENABLE_AWS_SDK=OFF
cmake --build build-tier-off --target MediaServer -j2
```

### Cạm bẫy: thư mục output dùng chung

**Mọi** build dir (`build/`, `build-test/`, `build-tier-off/`, `build-sync-off/`) đều ghi thư viện
ra cùng một chỗ: `release/linux/Debug/`. Hệ quả cụ thể, đã mất thời gian vì nó:

Build cấu hình A xong rồi build cấu hình B, thì B thấy `libs3managerkit.a` **mới hơn** các `.o` của
chính nó, nên coi archive là đã cập nhật và **bỏ qua bước archive** — rồi link thẳng archive chứa
object của A. Triệu chứng khi kiểm tier-off ngay sau khi build `build/`:

```
undefined reference to `vtable for Aws::S3::Model::HeadObjectRequest'
```

Đây **không** phải lỗi mã nguồn tier-off. Cách nhận ra: object của chính build-tier-off không tham
chiếu AWS nào cả, còn object cùng tên của `build/` thì có 41 symbol:

```bash
nm -C build-tier-off/manager/CMakeFiles/s3managerkit.dir/Local/TierObjectStorage.cpp.o | grep -c ' U .*[Aa]ws'   # 0
nm -C build/manager/CMakeFiles/s3managerkit.dir/Local/TierObjectStorage.cpp.o           | grep -c ' U .*[Aa]ws'   # 41
```

Cách xử lý: buộc cấu hình đang kiểm archive lại. Không cần biên dịch lại, chỉ cần các `.o` của nó
mới hơn archive:

```bash
find build-tier-off -name '*.o' -exec touch {} +
cmake --build build-tier-off --target MediaServer -j2
```

Xong rồi thì làm điều tương tự cho build dir bạn dùng thường ngày trước khi chạy binary của nó,
nếu không sẽ gặp đúng vấn đề theo chiều ngược lại.

### Cạm bẫy: `file(GLOB)` trong `manager/CMakeLists.txt`

Danh sách nguồn của `manager/` lấy bằng `file(GLOB)`, chỉ mở rộng lúc **configure**. Một build dir
cấu hình từ trước khi có file `.cpp` mới sẽ không biên dịch file đó, và lỗi hiện ra ở bước link:

```
undefined reference to `managerkit::clampMaxToDisk(...)'
undefined reference to `managerkit::keepTimeThreshold(...)'
```

Vì `build-tier-off/` hiếm khi được dùng nên nó là chỗ dễ bị nhất. Configure lại trước khi kiểm:

```bash
cmake -S . -B build-tier-off -DCMAKE_BUILD_TYPE=Debug -DENABLE_TIER_STORAGE=OFF -DENABLE_AWS_SDK=OFF
```

Cả hai cạm bẫy trên đều là thứ một job CI sẽ tự bắt được. Chừng nào chưa có CI, hai bước
(configure lại + `touch` các `.o`) là phần bắt buộc của việc kiểm.
