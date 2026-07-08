# Luồng Chức Năng Lưu Trữ Đa Tầng

Tài liệu này mô tả luồng tổng thể của tính năng lưu trữ đa tầng trong S3MediaKit, từ cấu hình storage pool/policy, ghi nhận metadata segment, chuyển tầng, phát lại, restore dữ liệu COLD, đến đề xuất hook trong luồng HTTP media record.

Tài liệu API chi tiết nằm ở [api-storage-tiering.md](api-storage-tiering.md).

## 1. Mục Tiêu

Tính năng lưu trữ đa tầng cho phép hệ thống quản lý dữ liệu ghi hình theo vòng đời:

```text
Camera recording
  -> HOT local disk
  -> WARM local/NAS
  -> COLD MinIO/S3/archive
  -> EXPIRED / DELETED
```

Mục tiêu chính:

- Giữ dữ liệu mới ở HOT để phát nhanh.
- Tự động chuyển dữ liệu cũ hơn sang WARM/COLD theo policy.
- Lưu dài hạn trên object storage như MinIO/S3.
- Khi client phát lại dữ liệu COLD, hệ thống có thể tạo restore job hoặc tự động restore segment cần thiết.
- Không để HTTP thread bị block bởi tác vụ tải file từ MinIO/S3.

## 2. Các Thành Phần Chính

### 2.1 StoragePool

`storage_pools` định nghĩa backend lưu trữ:

- `LOCAL_DISK`: đĩa local cho HOT hoặc WARM.
- `NAS`: mount/network path cho WARM.
- `MINIO`, `S3`, `ARCHIVE`: object storage cho COLD.

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

Thứ tự ưu tiên policy:

```text
Camera override
  -> Group
  -> Project
  -> System default
```

Hiện backend mới triển khai camera override; group/project được giữ trong thiết kế để mở rộng.

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
   - Khi retention hoặc pressure delete cần hết hạn range, backend gọi `updateStatusByRangeId(...)` để chuyển status sang `EXPIRED`.

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
- Upload segment lên MinIO/S3 qua `TierObjectStorage`.
- Restore segment COLD về restore cache tạm theo `storage.restore_save_path`.
- Ghi nhận HOT range từ event `kBroadcastRecordMP4` và reconcile định kỳ từ timefile.

### 2.6 File Storage Backend

Các pool dạng filesystem được tách theo backend:

- `TierFileStorageBase`: helper chung register pool, test path, resolve path, copy qua file `.tmp`, rename atomic, delete, `statvfs`.
- `TierLocalDiskStorage`: dùng cho `LOCAL_DISK`.
- `TierNASStorage`: dùng cho `NAS`, hiện yêu cầu `network_path`/`mount_path` đã mount và writable; đây là hook để bổ sung stale-mount/retry/timeout.

`TierStorageManager` chọn backend theo `StoragePool.type`; nghiệp vụ tiering không xử lý trực tiếp chi tiết copy/delete của từng backend.

### 2.7 TierObjectStorage

`TierObjectStorage` quản lý client MinIO/S3/ARCHIVE theo `pool_id`.

Các hàm quan trọng:

- `registerPool(...)`
- `uploadSegment(pool_id, local_path, s3_key)`
- `downloadSegment(pool_id, s3_key, local_path)`
- `deleteSegment(pool_id, s3_key)`
- `segmentExists(pool_id, s3_key)`
- `makeS3Key(base_path, camera_id, stream_id, segment_path)`

Đây là lớp nên được tái sử dụng cho restore segment từ COLD.

### 2.8 Job Tables

Các bảng job/trạng thái hỗ trợ:

- `tiering_jobs`: di chuyển segment giữa tier.
- `restore_jobs`: theo dõi restore segment từ COLD về restore cache tạm.
- `storage_alerts`: cảnh báo storage.
- `protected_videos`: đoạn video được bảo vệ/evidence/locked.

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
- `MINIO/S3` cần `endpoint`, `bucket`, `base_path`, `access_key`, `secret_key`.
- `high_watermark_percent < critical_watermark_percent`.

Sau khi tạo pool, `TierStorageManager` register pool vào backend tương ứng:

- Object storage: `TierObjectStorage`.
- Local disk: `TierLocalDiskStorage`.
- NAS: `TierNASStorage`.

### 3.2 Test Connection

FE gọi:

```text
POST /media/mserver/storage/pool/testConnection
```

Với pool MinIO/S3, backend dùng `HeadBucket`/probe tương đương để xác nhận kết nối.

### 3.3 Tạo Policy

FE gọi:

```text
POST /media/mserver/storage/policy/create
```

Backend validate:

- `total_retention_days > 0`
- `HOT < WARM < COLD`
- `delete_after_days >= retain_until_days` lớn nhất
- `pool_id` tồn tại và enabled
- `HOT` phải enabled
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

Định kỳ, `TierStorageManager::reconcileHotRangesFromTimeFiles()` đọc timefile ở `kMP4SavePath` bằng `TimeQuery`, query từ `last_hot_range_end - overlap`, decode `TimeBlock.file_path`, rồi merge lại các HOT range bị thiếu.

## 5. Luồng Chuyển Tầng

`TierStorageManager::runTieringCycle()` chạy định kỳ bằng timer của `TierStorageManager` (hiện 300 giây/lần). Hàm này dispatch async sang `WorkThreadPool`.

### 5.1 Các bước của một cycle

Luồng hiện tại theo code:

1. Reset ticker và ghi log bắt đầu.
2. Refresh `_pool_cache` từ `storage_pools`.
3. Gọi `ensureSystemDefaultPolicy()` để đảm bảo policy `policy-system-default` tồn tại.
4. Gọi `reconcileHotRangesFromTimeFiles()` để bù HOT range còn thiếu từ timefile.
5. Query `tiering_jobs` trạng thái `PENDING` và execute trước.
6. Lấy danh sách camera có dữ liệu `AVAILABLE`:
   - Từ `segment_tier_ranges.findDistinctAvailableCameras()`.
7. Với từng camera:
   - `getEffectivePolicy(camera_id)`.
   - Nếu có policy hiệu lực, chạy `processCameraTiering(camera_id, policy)`.
   - Chạy `processCameraPressureTiering(camera_id, policy)`.
   - Chạy `enforceCameraArchiveRetention(camera_id)`.
8. Ghi log thời gian hoàn tất.

### 5.2 Move Theo Tuổi Dữ Liệu

`processCameraTiering()` xử lý move theo `retain_until_days`.

Với từng cặp tier liền kề sau khi sort `HOT -> WARM -> COLD`:

1. Bỏ qua nếu source/destination tier disabled.
2. Bỏ qua nếu source hoặc destination `pool_id` rỗng.
3. Tính:

```text
move_threshold = now - source_tier.retain_until_days * 86400
move_threshold không được mới hơn now - min_segment_age_minutes_before_move
```

4. Query `segment_tier_ranges` theo source tier và age.
5. Với range thuộc camera hiện tại, tạo job bằng `queueTierMoveJob(...)`.

`queueTierMoveJob()` yêu cầu `source_pool_id` và `target_pool_id` đều không rỗng. Source pool thực tế ưu tiên lấy từ `SegmentTierRange.pool_id`; policy source pool là fallback cấu hình nhưng cũng phải có giá trị.

`queueTierMoveJob()` chống tạo trùng job bằng cách kiểm tra job `PENDING/RUNNING` cùng camera/source/target/window.

### 5.3 Move Sớm Khi Pool HOT/WARM Đầy

`processCameraPressureTiering()` xử lý trường hợp pool source vượt `high_watermark_percent` trước khi dữ liệu đạt `move_threshold`.

Điều kiện chính:

- Source/destination tier enabled.
- Source/destination `pool_id` không rỗng.
- Destination pool tồn tại và chưa vượt `critical_watermark_percent`.
- Source pool trong policy tồn tại và usage >= `high_watermark_percent`.
- Range source đang `AVAILABLE`.
- Range đủ ổn định theo `min_segment_age_minutes_before_move`, tối thiểu 60 giây.

Luồng này cũng tạo `tiering_jobs` qua `queueTierMoveJob(..., pressure=true)`. Mục tiêu reclaim xấp xỉ tới dưới high watermark 5%.

### 5.4 Retention Theo CameraOption

`enforceCameraArchiveRetention()` giữ ý nghĩa hiện tại của cấu hình từng camera:

- `keepArchivedMaxFor`: nếu không auto và > 0, dữ liệu cũ hơn mốc này được expire kể cả khi storage còn dung lượng.
- `keepArchivedMinFor`: dữ liệu mới hơn mốc này được bảo vệ khỏi xóa khi pressure; pressure move vẫn được phép vì không làm mất dữ liệu.

Nếu HOT/WARM pool critical, backend expire các range đủ cũ và không còn trong vùng protected minimum.

Expire yêu cầu `SegmentTierRange.pool_id` trỏ tới pool còn tồn tại trong cache. Nếu range thiếu pool hoặc pool không tồn tại, backend return ngay và không fallback sang `kMP4SavePath`.

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

### 5.6 Move MINIO/S3/ARCHIVE

Nếu target pool là MinIO/S3/ARCHIVE:

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
- `restore_required = true` nếu `tier = COLD`
- `size_bytes`
- `segment_count`

FE dùng response này để vẽ timeline HOT/WARM/COLD.

Nếu không có metadata trong `segment_tier_ranges` hoặc detail record hợp lệ, backend trả rỗng/NOT_FOUND; không tạo range giả `HOT` với `pool_id` rỗng.

### 6.2 Camera Summary

FE gọi:

```text
GET/POST /media/mserver/storage/camera/summary
```

Backend aggregate theo tier từ `segment_tier_ranges`.

## 7. Luồng Playback Hiện Tại

Backend đã có API:

```text
POST /media/mserver/storage/playback/resolve
```

Luồng hiện tại:

```text
FE chọn camera + time range
  -> playback/resolve
  -> backend query segment_tier_ranges
      HOT/WARM: trả READY + playback_url
      COLD: trả RESTORE_REQUIRED
      EXPIRED: trả EXPIRED
      không có metadata: trả NOT_FOUND
```

Nếu response là `RESTORE_REQUIRED`, FE gọi:

```text
POST /media/mserver/storage/restoreJob/create
```

Sau đó polling:

```text
GET/POST /media/mserver/storage/restoreJob/detail
```

Khi job `DONE`, FE gọi lại `playback/resolve` để lấy URL phát.

## 8. Đề Xuất Hook Restore Khi Truy Cập `/media/record/...`

Ngoài API `playback/resolve`, cần hỗ trợ trường hợp client truy cập trực tiếp URL record, ví dụ:

```text
/media/record/...
/record/...
/media/mserver/record/...
```

Khi URL này trỏ tới segment đã chuyển sang COLD và local file không còn tồn tại, `HttpFileManager` trigger restore qua hook đăng ký từ `Manager` thay vì trả 404 ngay.

### 8.1 Vị Trí Hook Đã Triển Khai

Có hai điểm có thể intercept:

#### Phương án A: Hook ở `HttpFileManager::accessFile`

File liên quan:

```text
src/Http/HttpFileManager.cpp
```

Hiện luồng file static:

```text
accessFile(...)
  -> nếu !is_hls && !File::fileExist(file_path)
       sendNotFound(cb)
  -> canAccessPath(...)
  -> response_file(...)
```

Hook được chèn trước `sendNotFound(cb)`:

```text
if (!is_hls && !File::fileExist(file_path)) {
    if (tryTriggerColdRestore(parser, media_info, file_path, cb)) {
        return;
    }
    sendNotFound(cb);
    return;
}
```

Ưu điểm:

- Bắt được mọi request file record trực tiếp.
- Không phụ thuộc vào loại protocol playback phía trên.
- Dễ trả `202 Accepted` ngay tại file access.

Nhược điểm:

- Cần parse ngược `file_path` thành `camera_id`, `stream_id`, `segment_path`.
- Nếu URL record có nhiều format, cần chuẩn hóa mapping path.

#### Phương án B: Hook ở `HttpSession::checkLiveStreamByApp`

File liên quan:

```text
src/Http/HttpSession.cpp
```

Điểm nhận diện VOD:

```cpp
GET_CONFIG(string, record_app, Record::kAppName);
auto is_vod = _media_info.app == record_app;
```

Đề xuất:

- Sau khi `_media_info.parse(...)` và xác định `is_vod`.
- Nếu request là VOD record và range truy cập nằm trong COLD, trả `RESTORE_REQUIRED` hoặc tạo restore job.

Ưu điểm:

- Có sẵn `MediaInfo`, app/stream rõ hơn.
- Có thể xử lý theo protocol trước khi chạm file.

Nhược điểm:

- Không phải mọi static file request đều đi qua luồng live stream lookup.
- Với request file MP4 trực tiếp, hook ở `HttpFileManager::accessFile` vẫn chắc chắn hơn.

Hiện tại dùng phương án A làm hook chính. Phương án B vẫn có thể bổ sung sau như pre-check tối ưu cho VOD playback.

### 8.2 Hàm Helper Hiện Tại

`TierStorageManager` hiện có helper:

```text
TierStorageManager::handleColdAccessByPath(file_path)
```

Có thể tách riêng thành `TierRestoreManager` sau nếu luồng restore phức tạp hơn:

```text
manager/Local/TierRestoreManager.h
manager/Local/TierRestoreManager.cpp
```

Trách nhiệm:

- Parse `file_path` thành `camera_id`, `stream_id`, `segment_path`, timestamp.
- Tìm `SegmentTierRange` chứa timestamp của segment.
- Nếu range `tier != COLD`, không xử lý.
- Nếu range `COLD`:
  - Nếu đang `RESTORING`, trả job hiện có.
  - Nếu chưa có job, tạo restore job.
  - Split range lớn thành `before / requested segment / after` để chỉ segment được truy cập chuyển sang `RESTORING`.
  - Dispatch task tải object từ MinIO/S3 về `storage.restore_save_path`.

### 8.3 Response Khi Truy Cập File COLD

Khi client truy cập trực tiếp file COLD, backend nên trả JSON nếu request chấp nhận JSON, hoặc text/plain tối thiểu nếu là player thông thường.

Đề xuất HTTP:

```http
HTTP/1.1 202 Accepted
Content-Type: application/json
Retry-After: 5
```

Body:

```json
{
  "code": 300056,
  "msg": "Recording is in Cold Storage and must be restored before playback",
  "data": {
    "status": "RESTORE_REQUIRED",
    "job_id": "restore-job-000001",
    "tier": "COLD",
    "restore_required": true,
    "estimated_restore_seconds": 120
  }
}
```

Với HLS `.m3u8` hoặc `.ts`:

- `.m3u8`: có thể trả `202` JSON để FE/player wrapper biết cần restore.
- `.ts`/segment file: nếu player tự request segment, nên ưu tiên restore trước ở bước resolve playlist; không nên để từng `.ts` tạo job riêng.

### 8.4 Restore Worker Đã Triển Khai Ở Mức Hook/Job

Restore không được chạy trực tiếp trên HTTP poller thread.

Luồng worker:

```text
HTTP request COLD segment
  -> create/get restore job
  -> split SegmentTierRange quanh segment 1 phút
  -> mark requested segment range status = RESTORING
  -> enqueue task vào WorkThreadPool
  -> HTTP trả 202/300056 ngay

worker:
  -> load pool COLD
  -> register object storage client nếu cần
  -> s3_key = makeS3Key(base_path, camera_id, stream_id, segment_path)
  -> local_path = restore target path
  -> downloadSegment(pool_id, s3_key, local_path.tmp)
  -> rename .restore -> .mp4
  -> update requested SegmentTierRange status = AVAILABLE
  -> update RestoreJob DONE
```

Nếu lỗi:

```text
update RestoreJob FAILED
update SegmentTierRange status = AVAILABLE hoặc MISSING tùy lỗi
ghi error_message
```

### 8.5 Restore Target Path Hiện Tại

Code hiện tại restore segment COLD về cache riêng, không ghi ngược vào HOT pool:

```text
{storage.restore_save_path}/{camera_id}/{stream_id}/{segment_path}.mp4
```

Luồng ghi file:

```text
download object -> {path}.restore
rename atomic  -> {path}.mp4
update RestoreJob DONE
update SegmentTierRange status = AVAILABLE
```

Range được restore vẫn giữ `tier = COLD` và `pool_id = cold_pool_id`. File restore nằm trong cache tạm để phục vụ request retry, không được tính là dữ liệu HOT chính thức.

Ưu điểm:

- Tách dữ liệu restore tạm khỏi dữ liệu HOT thật.
- Không làm HOT pool đầy do dữ liệu restore.
- Có thể cleanup theo TTL độc lập.

Cleanup hiện dựa trên `restore_jobs`: job `DONE` có `updated_at <= now - storage.restore_ttl_seconds` sẽ bị xử lý xóa cache, sau đó update status sang `EXPIRED` để không quét lặp. Đường dẫn file restore được lấy từ metadata của job và layout `{storage.restore_save_path}/{camera_id}/{stream_id}/{segment_path}.mp4`.

### 8.6 Chống Tạo Trùng Job

Trước khi tạo restore job:

```text
query restore_jobs
  where camera_id = ?
    and status in (PENDING, RUNNING)
    and time window overlap requested segment
```

Nếu có job đang chạy:

- Không tạo job mới.
- Trả lại `job_id` hiện có.
- `SegmentTierRange.status` giữ `RESTORING`.

### 8.7 Lock Và Protected Segment

Khi restore hoặc playback:

- Nếu segment `DELETED`: trả `410 Gone` hoặc code `300060`.
- Nếu segment `EXPIRED`: trả `300060`, FE hiển thị dữ liệu đã hết hạn.
- Nếu segment protected/evidence:
  - Không cho delete tự động.
  - Vẫn cho restore/playback nếu user có quyền.
- Nếu segment đang tiering:
  - Trả `300059` hoặc chờ job hoàn tất tùy policy.

## 9. Luồng Restore Tích Hợp Với API

### 9.1 FE Chủ Động Restore

```text
FE -> playback/resolve
BE -> RESTORE_REQUIRED
FE -> restoreJob/create
BE -> job_id PENDING
FE -> restoreJob/detail polling
BE -> DONE
FE -> playback/resolve
BE -> READY + playback_url
```

### 9.2 Player Truy Cập Trực Tiếp URL COLD

```text
Player -> GET /media/record/.../segment.mp4
HttpFileManager -> file local missing
Cold restore hook -> find SegmentTierRange COLD
Cold restore hook -> create/get restore job
BE -> 202 + code 300056 + job_id
Worker -> download from MinIO/S3
Worker -> update metadata DONE
Player/FE -> retry URL
BE -> responseFile local restored file
```

### 9.3 Autorestore Không Cần FE

Có thể cấu hình `auto_restore_on_record_access`:

- `false`: chỉ trả restore required, FE tự tạo job.
- `true`: backend tự tạo job ngay khi truy cập URL COLD.

Khuyến nghị:

- Bật `true` cho console/controlled playback.
- Cẩn thận khi public URL có thể bị crawler/player retry liên tục.

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
PENDING
RUNNING
DONE
FAILED
CANCELLED
```

Progress:

```text
progress_percent = processed_bytes * 100 / total_bytes
```

Khi `DONE`:

```text
playback_ready = true
```

## 11. Dashboard Và Quan Sát Hệ Thống

Dashboard nên lấy:

- `/storage/dashboard/summary`
- `/storage/pool/list`
- `/storage/tieringJob/list`
- `/storage/restoreJob/list`
- `/storage/alert/list`
- `/storage/expiredSegment/list`
- `/storage/protected/list`

Các thông tin cần hiển thị:

- Tổng dung lượng, used/free/percent.
- Tình trạng từng tier.
- Active/failed tiering jobs.
- Active restore jobs.
- Cảnh báo pool high/critical/offline.
- Expired segments chờ duyệt.
- Protected/evidence video ranges.

## 12. Cấu Hình Và Hook Nội Bộ

### 12.1 Backend Config

Các cấu hình liên quan trong codebase hiện tại:

```ini
storage.auto_restore_on_record_access=1
storage.auto_restore_max_concurrent=5
storage.restore_save_path=/dev/shm/restore
storage.restore_ttl_seconds=3600
```

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

### 12.3 HttpFileManager Hook Pseudocode

```cpp
if (!is_hls && !File::fileExist(file_path)) {
    auto ret = TierStorageManager::Instance()
        .handleColdAccessByPath(file_path, media_info);

    if (ret.handled) {
        Json::Value body;
        body["code"] = 300056;
        body["msg"] = ret.message;
        body["data"]["status"] = ret.status;
        body["data"]["job_id"] = ret.job_id;
        body["data"]["tier"] = "COLD";
        body["data"]["restore_required"] = true;
        body["data"]["estimated_restore_seconds"] = ret.estimated_restore_seconds;

        StrCaseMap header;
        header["Content-Type"] = "application/json";
        header["Retry-After"] = "5";
        cb(202, "application/json", header,
           std::make_shared<HttpStringBody>(body.toStyledString()));
        return;
    }

    sendNotFound(cb);
    return;
}
```

### 12.4 Restore Worker Pseudocode

```cpp
void RestoreWorker::execute(RestoreJob job) {
    updateJob(job_id, RUNNING);

    pool = source_pool_from_cold_range.pool_id
    key = TierObjectStorage::makeS3Key(pool.base_path, camera_id, stream_id, segment_path)
    restore_path = storage.restore_save_path + "/" + camera_id + "/" + stream_id + "/" + segment_path + ".mp4"
    local_tmp = restore_path + ".restore"

    ok = objectStorage.downloadSegment(pool.id, key, local_tmp)
    if (!ok) fail

    rename(local_tmp, restore_path)
    update SegmentTierRange status = AVAILABLE
    update processed_bytes

    updateJob(job_id, DONE)
}
```

## 13. Rủi Ro Và Lưu Ý

- Không block event poller/HTTP thread khi download từ MinIO/S3.
- Cần giới hạn số restore đồng thời để tránh bão I/O.
- Cần chống job trùng khi player retry liên tục.
- Restore cache phải được cleanup theo `restore_jobs.updated_at + storage.restore_ttl_seconds`; job đã cleanup chuyển sang `EXPIRED`.
- Cần verify checksum/size sau download.
- Cần xử lý Range request nếu client phát MP4 bằng byte-range:
  - Nếu file chưa restore xong, trả `202`.
  - Khi restore xong, request Range tiếp theo cần map sang restore cache.
- Với HLS, nên restore cả playlist window trước khi player request từng segment.
- Nếu object trên MinIO mất, update segment `MISSING` và job `FAILED`.

## 14. Thứ Tự Triển Khai Khuyến Nghị

1. Hoàn thiện `restore_jobs` repository và restore worker thực thi thật.
2. Hoàn thiện helper resolve segment từ `file_path`.
3. Hook `HttpFileManager::accessFile` khi local file missing.
4. Trả `202 + code 300056 + job_id` khi COLD restore được trigger.
5. Tải object từ MinIO/S3 về `storage.restore_save_path`.
6. Update `SegmentTierRange` và `RestoreJob` khi restore hoàn tất.
7. Cleanup TTL dựa trên `restore_jobs` trạng thái `DONE`.
8. Bổ sung metrics/alert cho restore throughput, failures, queue depth.
