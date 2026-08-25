# Luồng Chức Năng Lưu Trữ Đa Tầng

Tài liệu này mô tả chi tiết luồng lưu trữ đa tầng đang được triển khai trong S3MediaKit, từ cấu hình storage pool/policy, ghi nhận metadata segment, chuyển tầng, retention, playback đến restore dữ liệu COLD.

Tài liệu API chi tiết nằm ở [api-storage-tiering.md](api-storage-tiering.md).

> Tính năng chỉ được biên dịch khi CMake bật `-DENABLE_TIER_STORAGE=ON`; mặc định hiện là `OFF`. Khi tắt, entity/manager tiering bị loại khỏi build, `TierStorageManager` không start và route `/media/mserver/storage/...` không được đăng ký. Migration SQL của MediaServer vẫn quét thư mục migration độc lập với macro này.

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

Khi trả về client, `access_key` và `secret_key` phải được mask bằng `***********`.

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

Với mô hình mỗi camera ghi 1 segment MP4/phút, 300 camera trong 6 tháng tạo khoảng 77 triệu bản ghi nếu lưu từng segment lâu dài. Vì vậy backend dùng metadata dạng range compact:

- `segment_tier_ranges`: bảng chính, lưu trạng thái tier theo khoảng thời gian liên tục.
- `segment_tier_records`: bảng chi tiết ngắn hạn, dùng hỗ trợ thao tác file-level khi cần; không còn là nguồn chọn camera/tiering chính.

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
   - Backend tạo `SegmentTierRange` tier `HOT`, đúng `pool_id`, status `AVAILABLE`, rồi gọi `mergeOrInsert(...)` để gộp các segment liền kề thành range compact.
   - Nếu không match được HOT pool, segment không được ghi vào `segment_tier_ranges`.
   - Timefile vẫn được ghi dưới `Protocol::kMP4SavePath`; `file_path` trong `TimeBlock` trỏ tới segment MP4 thật, có thể nằm ở HOT pool khác.

2. **Luồng move tier thành công**
   - Khi `tiering_jobs` move thành công, backend gọi `updateTierByRangeId(...)` để đổi tier/pool/status của đúng range đã move.
   - Move LOCAL/NAS và object storage đều cập nhật range theo cùng nguyên tắc này.

3. **Luồng expire/delete**
   - `MARK_EXPIRED_WAIT_APPROVAL`, camera maximum retention và một số pressure path chuyển range sang `EXPIRED`.
   - Automatic delete/`DELETE_OLDEST` xóa file/object best-effort rồi chuyển range sang `DELETED`.
   - Cả hai nhóm dùng `updateStatusByRangeId(...)` và có thể phát rebuild-timefile notification.

4. **Luồng restore COLD**
   - Khi truy cập segment COLD, backend dùng `splitWindowStatus(...)` để tách phần segment cần restore ra khỏi range lớn và đặt status `RESTORING`.
   - Khi restore xong về cache, backend gọi `updateStatusByExactWindow(...)` trả status về `AVAILABLE`; tier/pool vẫn là COLD vì file restore chỉ nằm trong cache tạm.

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
- `segmentExists(pool_id, s3_key)`
- `makeS3Key(base_path, camera_id, stream_id, segment_path)`

Đây là lớp dùng chung cho cả upload khi chuyển sang COLD và download khi restore segment.

### 2.8 Job Tables

Các bảng job/trạng thái hỗ trợ:

- `tiering_jobs`: di chuyển segment giữa tier.
- `restore_jobs`: theo dõi restore segment từ COLD về restore cache tạm.
- `storage_alerts`: cảnh báo storage.
- `protected_videos`: đoạn video được bảo vệ/evidence/locked.

`tiering_jobs` lưu `job_id`, camera/stream/range, source/target tier, source/target pool, status, segment time window, `bytes_total`, `bytes_moved`, `error_message`, `created_at`, `updated_at`.

`restore_jobs` lưu `job_id`, `camera_id`, source/target tier, status, restore time window, `total_bytes`, `processed_bytes`, `reason`, `error_message`, `created_at`, `updated_at`. Job auto-restore dùng `reason=AUTO_RECORD_ACCESS`.

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

Type/tier còn phải khớp ma trận `pool/options`; `S3` và `ARCHIVE` hiện bị validation từ chối.

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

Policy này trỏ vào HOT pool mặc định có id `pool-default-hot`. Nếu chưa có HOT pool, backend tạo HOT pool mặc định từ `{Protocol::kMP4SavePath}/{Record::kAppName}` trước, sau đó tạo policy default.

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

Định kỳ, `TierStorageManager::backfillHotRangesFromTimeFiles()` đọc timefile bằng `TimeQuery`, decode `TimeBlock.file_path`, resolve HOT pool và merge các HOT range bị thiếu. Backfill bị giới hạn bởi `storage.tier_range_backfill_enabled`, `storage.tier_range_backfill_days`, `storage.tier_range_backfill_chunk_seconds` và `storage.tier_range_backfill_max_cameras_per_cycle`.

## 5. Luồng Chuyển Tầng

`TierStorageManager::runTieringCycle()` chạy định kỳ bằng timer của `TierStorageManager` (hiện 300 giây/lần). Hàm này dispatch async sang `WorkThreadPool`.

### 5.1 Các bước của một cycle

Luồng hiện tại theo code:

1. Reset ticker và ghi log bắt đầu.
2. Refresh `_pool_cache` từ `storage_pools`.
3. Gọi `ensureSystemDefaultPolicy()` để đảm bảo policy `policy-system-default` tồn tại.
4. Gọi `backfillHotRangesFromTimeFiles()` để bù HOT range còn thiếu từ timefile.
5. Query `tiering_jobs` trạng thái `PENDING` và execute trước.
6. Lấy danh sách camera có dữ liệu `AVAILABLE`:
   - Từ `segment_tier_ranges.findDistinctAvailableCameras()`.
7. Với từng camera:
   - `getEffectivePolicy(camera_id)`.
   - Nếu có policy hiệu lực, chạy `processCameraTiering(camera_id, policy)`.
   - Chạy `processCameraPressureTiering(camera_id, policy)`.
   - Chạy `enforcePolicyDeleteRetention(camera_id, policy)`.
   - Chạy `enforceCameraArchiveRetention(camera_id, policy)`.
8. Ghi log thời gian hoàn tất.

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
```

5. Query `segment_tier_ranges` theo source tier/source pool với `start_time < move_threshold`.
6. Nếu range vượt quá ngưỡng nhưng còn phần mới chưa đủ tuổi, backend split range tại ranh giới segment gần `move_threshold`.
7. Với range thuộc camera hiện tại, tạo job bằng `queueTierMoveJob(...)`.

`queueTierMoveJob()` yêu cầu `source_pool_id` và `target_pool_id` đều không rỗng. Source pool thực tế ưu tiên lấy từ `SegmentTierRange.pool_id`; policy source pool là fallback cấu hình nhưng cũng phải có giá trị.

`queueTierMoveJob()` chống tạo trùng job bằng cách kiểm tra job `PENDING/RUNNING` cùng camera/source/target/window. Sau khi insert job `PENDING` thành công, hàm gọi `executePendingJob(job)` ngay; cycle sau chỉ nhặt lại các job `PENDING` còn sót từ lần chạy trước.

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

Luồng này cũng tạo `tiering_jobs` qua `queueTierMoveJob(..., pressure=true)`. Mục tiêu reclaim xấp xỉ tới dưới high watermark 5%.

### 5.4 Retention Theo CameraOption

`enforceCameraArchiveRetention()` giữ ý nghĩa hiện tại của cấu hình từng camera:

- `keepArchivedMaxFor`: nếu không auto và > 0, dữ liệu cũ hơn mốc này được expire kể cả khi storage còn dung lượng.
- `keepArchivedMinFor`: dữ liệu mới hơn mốc này được bảo vệ khỏi xóa khi pressure; pressure move vẫn được phép vì không làm mất dữ liệu.

Nếu HOT/WARM pool critical, backend expire các range đủ cũ và không còn trong vùng protected minimum.

Expire yêu cầu `SegmentTierRange.pool_id` trỏ tới pool còn tồn tại trong cache. Nếu range thiếu pool hoặc pool không tồn tại, backend return ngay và không fallback sang `kMP4SavePath`.

Ngoài `CameraOption`, `enforcePolicyDeleteRetention()` xử lý `delete_policy`:

- `delete_after_days <= 0`: không chạy policy delete retention.
- `MARK_EXPIRED_WAIT_APPROVAL` hoặc `require_approval_before_delete=true`: chỉ đặt range sang `EXPIRED`.
- `MOVE_TO_EXTERNAL_STORAGE`: queue tiering job tới `external_pool_id` nếu pool tồn tại và khác pool hiện tại.
- `DELETE_AUTOMATICALLY`: xóa segment qua backend của pool, update range `DELETED` và rebuild timefile.
- `skip_protected_video=true`: bỏ qua range overlap bất kỳ row `protected_videos`. Field `skip_evidence_video` được parse nhưng chưa có check riêng trong hàm này.

### 5.5 Move LOCAL/NAS

Nếu target pool là local/NAS:

```text
resolve backend theo pool.type:
  LOCAL_DISK -> TierLocalDiskStorage
  NAS        -> TierNASStorage
require source_pool_id/target_pool_id non-empty và tồn tại
copy file sang mount_path/network_path qua file .tmp
  -> verify copy
  -> remove source
  -> update SegmentTierRange.tier/pool_id/status cho window đã move
  -> update tiering job DONE/FAILED
```

### 5.6 Move Sang Object Storage

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

Backend aggregate theo tier từ `segment_tier_ranges`.

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
  -> query segment_tier_ranges
  -> filter stream_id và overlap
  -> kiểm tra EXPIRED/DELETED/MISSING
  -> resolve pool type của từng range
  -> query restore_jobs overlap
  -> trả READY / RESTORE_REQUIRED / RESTORING / NOT_FOUND / EXPIRED
```

Trạng thái và mã lỗi:

| Trạng thái | Điều kiện | API code / HTTP |
|------------|-----------|-----------------|
| `READY` | Không có object-storage range cần restore, hoặc có job `DONE` phủ toàn window | `0` / 200 |
| `RESTORE_REQUIRED` | Có object-storage range, chưa có job `DONE` phủ window | `914009` / 404 |
| `RESTORING` | Có job `PENDING` hoặc `RUNNING` overlap | `914009` / 404 |
| `NOT_FOUND` | Không có range hoặc range `MISSING` | `914016` / 404 |
| `EXPIRED` | Có range `EXPIRED` | `914017` / 404 |
| `DELETED` | Có range `DELETED` | `914016` / 404 |

API này không tạo `playback_url`. Field `expires_at` trong response `READY` chỉ là thời điểm hiện tại cộng `storage.restore_ttl_seconds`; URL phát vẫn do luồng playback hiện hữu của hệ thống cung cấp.

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

Lưu ý: một job `PENDING` do API `restoreJob/create` tạo cũng thỏa điều kiện này, dù API đó chưa dispatch worker. Đây là giới hạn quan trọng của code hiện tại.

### 8.4 Tạo Job Và Split Range

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

Backend gọi:

```cpp
range_imp.splitWindowStatus(
    cold_range,
    segment_start,
    segment_start + 60,
    SegmentStatus::RESTORING);
```

Range lớn được tách thành `before / requested window / after`; chỉ window 60 giây cần tải được đặt `RESTORING`.

### 8.5 Thread Và Lifetime

Restore không chạy trên EventPoller/HTTP thread. Backend capture `weak_ptr<TierStorageManager>` rồi dispatch sang:

```cpp
WorkThreadPool::Instance().getPoller()->async(...);
```

Worker lock weak pointer trước khi gọi `executeRestoreSegment(...)`, tránh giữ manager sống cưỡng bức hoặc capture raw `this`.

### 8.6 Restore Worker

Luồng thực thi:

```text
update RestoreJob RUNNING
  -> đảm bảo source object pool đã register
  -> key = makeS3Key(base_path, camera_id, stream_id, segment_path)
  -> restore_path = buildRestoreSegmentPath(...)
  -> tạo restore directory
  -> nếu restore_path đã tồn tại:
       update DONE bằng file size
       range RESTORING -> AVAILABLE
       return
  -> download object vào <restore_path>.restore
  -> rename atomic sang <restore_path>
  -> stat file lấy processed_bytes
  -> update RestoreJob DONE
  -> range RESTORING -> AVAILABLE
```

Khi register/download/mkdir/rename lỗi:

- Job chuyển `FAILED` và ghi `error_message`.
- Exact range window được trả về `AVAILABLE` để có thể retry.
- Partial file `.restore` được xóa ở các nhánh download/rename thất bại.
- Code hiện không chuyển range sang `MISSING` khi object download thất bại.

### 8.7 Object Key Và Restore Path

Object key:

```text
{base_path}/{camera_id}/{stream_id}/{segment_path}.mp4
```

Restore cache:

```text
{storage.restore_save_path}/{camera_id}/{stream_id}/{segment_path}.mp4
```

File tạm:

```text
{restore_path}.restore
```

Range sau restore vẫn giữ `tier=COLD` và `pool_id` của object pool. Chỉ status trở về `AVAILABLE`; file local là cache tạm, không phải dữ liệu HOT chính thức.

### 8.8 Cleanup Cache

Mỗi timer tick, `cleanupRestoreTempFiles()`:

1. Lấy job `DONE` có `updated_at <= now - storage.restore_ttl_seconds`.
2. Thu thập segment trong job window; nếu metadata không đủ thì scan restore root.
3. Xóa file cache và file `.restore`.
4. Chuyển job sang `EXPIRED` để không xử lý lặp.
5. Scan và xóa partial `.restore` bị bỏ dở quá TTL.

`EXPIRED` là status runtime thực tế của restore job sau cleanup, dù enum `JobStatus` dùng chung chưa khai báo giá trị này.

## 9. Luồng Restore Tích Hợp Với API

### 9.1 API Resolve Và Quan Sát Job

Luồng đang hoạt động:

```text
FE -> playback/resolve
BE -> READY hoặc RESTORE_REQUIRED/RESTORING

Luồng replay nội bộ -> resolveRecordedBlockPath
BE -> handleColdAccessByPath
BE -> tạo job + dispatch worker nếu auto_restore bật

FE -> restoreJob/list hoặc restoreJob/detail
BE -> PENDING/RUNNING/DONE/FAILED/EXPIRED

FE/consumer -> retry playback/replay
BE -> resolve cache path khi job DONE và file cache còn TTL
```

Các endpoint restore job yêu cầu đồng thời permission playback `1003` và read MediaServer `150301`.

### 9.2 Giới Hạn Của `restoreJob/create`

`POST /media/mserver/storage/restoreJob/create` hiện:

- Validate camera/time window.
- Tổng hợp `total_bytes` từ range.
- Insert row `PENDING`.
- Trả `job_id`.

Endpoint chưa:

- Chọn từng object segment.
- Split range sang `RESTORING`.
- Dispatch `executeRestoreSegment()`.
- Có scheduler riêng để consume restore job `PENDING`.

Vì vậy FE không nên dùng endpoint này như lệnh download production. Job `PENDING` do API tạo còn có thể chặn auto-restore tạo/dispatch job mới cho cùng window.

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
RESTORING  : đang restore từ COLD
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
storage.default_hot_high_watermark_percent=85
storage.default_hot_critical_watermark_percent=95
storage.segment_record_ttl_seconds=604800
storage.tier_range_backfill_enabled=1
storage.tier_range_backfill_days=180
storage.tier_range_backfill_chunk_seconds=86400
storage.tier_range_backfill_max_cameras_per_cycle=20
```

| Biến | Nơi dùng | Ghi chú |
|------|----------|---------|
| `auto_restore_on_record_access` | `handleColdAccessByPath()` | Phải bật mới tạo auto-restore job |
| `auto_restore_max_concurrent` | Chưa được worker đọc | Đã khai báo nhưng chưa enforce concurrency |
| `restore_save_path` | `buildRestoreSegmentPath()` | Root cache restore tạm |
| `restore_ttl_seconds` | API resolve và cleanup | Tính `expires_at`, xóa cache/job `DONE` cũ |
| `default_hot_*_watermark_percent` | `ensureDefaultHotPool()` | Watermark cho `pool-default-hot` |
| `segment_record_ttl_seconds` | `pruneOldMetrics()` | Chỉ prune `segment_tier_records`, không prune range chính |
| `tier_range_backfill_enabled` | `backfillHotRangesFromTimeFiles()` | Bật/tắt backfill |
| `tier_range_backfill_days` | Backfill | Cửa sổ lịch sử |
| `tier_range_backfill_chunk_seconds` | Backfill | Khoảng thời gian xử lý mỗi camera/cycle |
| `tier_range_backfill_max_cameras_per_cycle` | Backfill | Giới hạn camera mỗi cycle |

`segment_tier_ranges` là bảng metadata chính của luồng tiering nên không bị cleanup bằng TTL ngắn hạn. Chỉ các range `DELETED/EXPIRED` nên được cleanup bằng retention riêng sau khi không còn cần cho timeline, thống kê hoặc audit.

`storage.restore_save_path` là thư mục restore cache tạm. `storage.restore_ttl_seconds` được áp dụng thông qua `restore_jobs`: job `DONE` quá TTL sẽ được đánh dấu `EXPIRED` sau khi xóa file cache tương ứng.

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
  -> if status RESTORING: handled=true, return existing job
  -> create restore job
  -> enqueue restore worker
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
    restore_imp.updateStatus(job_id, "RUNNING");

    pool = source_pool_from_cold_range.pool_id
    key = TierObjectStorage::makeS3Key(pool.base_path, camera_id, stream_id, segment_path)
    restore_path = storage.restore_save_path + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4"
    local_tmp = restore_path + ".restore"

    ok = objectStorage.downloadSegment(pool.id, key, local_tmp)
    if (!ok) fail

    rename(local_tmp, restore_path)
    restore_imp.updateStatus(job_id, "DONE", restored_bytes)
    range_imp.updateStatusByExactWindow(camera_id, stream_id,
                                        range_start, range_end,
                                        "AVAILABLE")
}
```

## 13. Rủi Ro Và Lưu Ý

- Download đã được dispatch sang `WorkThreadPool`, không block EventPoller; tuy nhiên `auto_restore_max_concurrent` chưa được enforce nên vẫn có rủi ro bão I/O.
- Auto-restore đã chống job `PENDING/RUNNING` overlap, nhưng job `PENDING` do API tạo có thể chặn worker mà không bao giờ được consume.
- Restore cache phải được cleanup theo `restore_jobs.updated_at + storage.restore_ttl_seconds`; job đã cleanup chuyển sang `EXPIRED`.
- Worker dùng kết quả download/rename và file size; chưa verify checksum nội dung.
- Direct HTTP static-file miss, byte-range và HLS restore chưa được tích hợp.
- Khi object MinIO mất, job chuyển `FAILED` nhưng range được trả về `AVAILABLE`, chưa chuyển `MISSING`.
- Parser auto-restore phụ thuộc layout record root/tên segment; custom record root có thể không parse được.
- Nếu DB có HOT pool custom nhưng thiếu `pool-default-hot`, system default policy mới có thể không được tạo.

## 14. Phần Còn Thiếu Theo Code Hiện Tại

1. Nối `restoreJob/create` với worker hoặc bổ sung scheduler consume job `PENDING`.
2. Enforce `storage.auto_restore_max_concurrent` và bổ sung queue/backpressure.
3. Quyết định có hỗ trợ direct HTTP static-file restore hay chỉ giữ resolver `TimeBlock`; nếu hỗ trợ cần contract HTTP mới.
4. Bổ sung checksum/size verification và chuyển range sang `MISSING` khi object thực sự không còn.
5. Sửa parser record path để hỗ trợ camera record root do policy điều phối.
6. Sửa bootstrap default policy khi chỉ có HOT pool custom.
7. Hoàn thiện semantics `expiredSegment/approve`/`extend`: xóa file vật lý và lưu `extend_until` nếu đó là yêu cầu sản phẩm.
8. Bổ sung metrics/alert cho restore throughput, failure và queue depth; các metric dashboard placeholder hiện vẫn là `0`.
