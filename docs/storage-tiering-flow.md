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

### 2.3 SegmentTierRange Và SegmentTierRecord

Với mô hình mỗi camera ghi 1 segment MP4/phút, 300 camera trong 6 tháng tạo khoảng 77 triệu bản ghi nếu lưu từng segment lâu dài. Vì vậy backend dùng hai mức metadata:

- `segment_tier_ranges`: bảng chính, lưu trạng thái tier theo khoảng thời gian liên tục.
- `segment_tier_records`: bảng chi tiết/compatibility ngắn hạn, tự xóa theo TTL cấu hình.

`segment_tier_ranges` lưu:

- `range_id`
- `camera_id`
- `stream_id`
- `tier`: `HOT`, `WARM`, `COLD`
- `pool_id`
- `status`: `AVAILABLE`, `RESTORING`, `EXPIRED`, `DELETED`, `MISSING`
- `start_time`, `end_time`
- `segment_count`, `size_bytes`
- `path_pattern`

`segment_tier_records` chỉ lưu từng segment trong TTL ngắn:

- `camera_id`
- `stream_id`
- `segment_path`
- `tier`: `HOT`, `WARM`, `COLD`
- `pool_id`
- `status`: `AVAILABLE`, `RESTORING`, `EXPIRED`, `DELETED`, `MISSING`
- `start_time`, `end_time`
- `file_size`

Mọi API timeline/playback/restore/tiering nên ưu tiên `segment_tier_ranges`. Chỉ dùng `segment_tier_records` để tương thích API cũ, debug ngắn hạn hoặc xử lý chi tiết trong khoảng còn TTL.

### 2.4 TierStorageManager

`TierStorageManager` chịu trách nhiệm:

- CRUD pool/policy.
- Gán policy cho camera.
- Kiểm tra health/capacity pool.
- Sync metadata segment vào `segment_tier_ranges`; ghi `segment_tier_records` ngắn hạn để tương thích/debug.
- Tạo và thực thi tiering job.
- Upload segment lên MinIO/S3 qua `TierObjectStorage`.

### 2.5 TierObjectStorage

`TierObjectStorage` quản lý client MinIO/S3 theo `pool_id`.

Các hàm quan trọng:

- `registerPool(...)`
- `uploadSegment(pool_id, local_path, s3_key)`
- `downloadSegment(pool_id, s3_key, local_path)`
- `deleteSegment(pool_id, s3_key)`
- `segmentExists(pool_id, s3_key)`
- `makeS3Key(base_path, camera_id, stream_id, segment_path)`

Đây là lớp nên được tái sử dụng cho restore segment từ COLD.

### 2.6 Job Tables

Các bảng job/trạng thái hỗ trợ:

- `tiering_jobs`: di chuyển segment giữa tier.
- `restore_jobs`: khôi phục segment từ COLD về HOT/WARM để phát lại.
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
- `LOCAL_DISK` cần `mount_path`.
- `NAS` cần `mount_path` hoặc `network_path`.
- `MINIO/S3` cần `endpoint`, `bucket`, `base_path`, `access_key`, `secret_key`.
- `high_watermark_percent < critical_watermark_percent`.

Sau khi tạo pool object storage, `TierStorageManager` cần register pool với `TierObjectStorage` để upload/download hoạt động.

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

## 4. Luồng Ghi Hình Và Ghi Nhận Segment

Luồng ghi hình hiện tại tạo file MP4/HLS trong root cấu hình bởi:

- `Protocol::kMP4SavePath`
- `Record::kAppName`

Để tiering hoạt động đúng, mỗi segment mới cần được gom vào `segment_tier_ranges`. Backend có thể ghi thêm `segment_tier_records`, nhưng bảng này chỉ giữ ngắn hạn.

Luồng đề xuất:

```text
Recorder tạo segment local
  -> phát sinh segment_path, start_time, end_time, file_size
  -> merge/upsert SegmentTierRange
      tier = HOT
      pool_id = HOT pool id
      status = AVAILABLE
      segment_count += 1
      size_bytes += file_size
  -> optional upsert SegmentTierRecord để debug/compatibility
```

Nếu chưa có event realtime từ recorder, `TierStorageManager::syncSegmentRecords(...)` scan thư mục record theo chu kỳ và merge metadata vào range. Scanner phải kiểm tra range đã bao phủ segment trước khi cộng `segment_count/size_bytes` để không cộng lặp khi scan lại.

## 5. Luồng Chuyển Tầng

`TierStorageManager::runTieringCycle()` chạy định kỳ.

### 5.1 Chọn Segment Cần Move

Với mỗi camera:

1. Resolve effective policy.
2. Sắp xếp tier theo thứ tự `HOT -> WARM -> COLD`.
3. Query `segment_tier_ranges` theo tier/status/end_time.
4. Với từng cặp tier liền kề:
   - Tính `move_threshold = now - retain_until_days`.
   - Query range `AVAILABLE` có `end_time <= threshold`.
   - Bỏ qua range đang có job `PENDING/RUNNING`.
   - Tạo `tiering_jobs`.

### 5.2 Move LOCAL/NAS

Nếu target pool là local/NAS:

```text
copy file sang mount_path/network_path
  -> verify copy
  -> remove source
  -> update SegmentTierRecord.tier/pool_id/status
  -> update SegmentTierRange.tier/pool_id/status cho window đã move
  -> update tiering job DONE/FAILED
```

### 5.3 Move MINIO/S3

Nếu target pool là MinIO/S3:

```text
resolve local_path
  -> s3_key = base_path/camera_id/stream_id/segment_path.mp4
  -> uploadSegment(pool_id, local_path, s3_key)
  -> update SegmentTierRecord:
       tier = COLD
       pool_id = cold_pool_id
       status = AVAILABLE
  -> update SegmentTierRange:
       tier = COLD
       pool_id = cold_pool_id
       status = AVAILABLE
  -> unlink local file sau khi upload thành công
  -> update tiering job
```

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

### 8.2 Hàm Helper Đề Xuất

Tạo helper mới, ví dụ:

```text
TierStorageManager::resolveSegmentByLocalPath(file_path)
TierStorageManager::requestRestoreForSegment(segment, target_tier, reason)
TierStorageManager::getOrCreateRestoreJob(camera_id, start_time, end_time, target_tier, reason)
```

Hoặc tạo lớp riêng:

```text
manager/Local/TierRestoreManager.h
manager/Local/TierRestoreManager.cpp
```

Trách nhiệm:

- Parse `file_path` hoặc `MediaInfo` thành segment identity.
- Tìm `SegmentTierRange` chứa timestamp của segment.
- Nếu range `tier != COLD`, không xử lý.
- Nếu range `COLD`:
  - Nếu đang `RESTORING`, trả job hiện có.
  - Nếu chưa có job, tạo restore job.
  - Split range lớn thành `before / requested segment / after` để chỉ segment được truy cập chuyển sang `RESTORING`.
  - Dispatch task tải object từ MinIO/S3 về local restore cache hoặc HOT pool.

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
  -> fsync/rename .tmp -> .mp4
  -> update requested SegmentTierRange tier = HOT, status = AVAILABLE
  -> optional update SegmentTierRecord nếu bản ghi chi tiết còn tồn tại
  -> update RestoreJob DONE
```

Nếu lỗi:

```text
update RestoreJob FAILED
update SegmentTierRange status = AVAILABLE hoặc MISSING tùy lỗi
ghi error_message
```

### 8.5 Restore Target Path

Có hai lựa chọn:

#### Lựa chọn 1: Restore về HOT path gốc

```text
{mp4_save_path}/{Record::kAppName}/{camera_id}/{stream_id}/{segment_path}.mp4
```

Ưu điểm:

- Luồng phát file hiện tại không cần đổi nhiều.
- Sau khi restore xong, request URL cũ có thể đọc được file.

Nhược điểm:

- Có thể làm HOT pool đầy.
- Cần cleanup file restored theo TTL.

#### Lựa chọn 2: Restore về cache riêng

```text
{mp4_save_path}/restore_cache/{camera_id}/{stream_id}/{segment_path}.mp4
```

Ưu điểm:

- Tách dữ liệu restore tạm khỏi dữ liệu HOT thật.
- Dễ cleanup theo TTL.

Nhược điểm:

- Cần map URL/file_path sang cache path trong `HttpFileManager`.

Khuyến nghị giai đoạn đầu: restore về HOT path gốc để tận dụng playback hiện tại, sau đó bổ sung cleanup TTL.

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

## 12. Đề Xuất API/Code Bổ Sung

### 12.1 Backend Config

Thêm cấu hình:

```ini
storage.auto_restore_on_record_access=1
storage.restore_target_tier=HOT
storage.restore_cache_ttl_seconds=3600
storage.restore_max_concurrency=4
storage.restore_retry_count=3
storage.segment_record_ttl_seconds=604800
```

`storage.segment_record_ttl_seconds` điều khiển TTL của `segment_tier_records`. Giá trị mặc định nên ngắn, ví dụ 7 ngày. `segment_tier_ranges` là bảng chính nên không bị xóa theo TTL này; chỉ range `DELETED/EXPIRED` có thể cleanup bằng retention riêng.

### 12.2 TierStorageManager API Nội Bộ

Đề xuất thêm:

```cpp
struct RestoreRequestResult {
    bool handled = false;
    bool ready = false;
    std::string job_id;
    std::string status;
    std::string message;
    int estimated_restore_seconds = 120;
};

RestoreRequestResult TierStorageManager::handleColdAccessByPath(
    const std::string &file_path,
    const MediaInfo &media_info);
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

    for each segment in job window:
        pool = getPool(segment.pool_id)
        key = TierObjectStorage::makeS3Key(pool.base_path, camera_id, stream_id, segment_path)
        local_tmp = local_path + ".restore"

        ok = objectStorage.downloadSegment(pool.id, key, local_tmp)
        if (!ok) fail

        rename(local_tmp, local_path)
        update segment tier/status/pool
        update processed_bytes

    updateJob(job_id, DONE)
}
```

## 13. Rủi Ro Và Lưu Ý

- Không block event poller/HTTP thread khi download từ MinIO/S3.
- Cần giới hạn số restore đồng thời để tránh bão I/O.
- Cần chống job trùng khi player retry liên tục.
- Cần cleanup restored file nếu restore về HOT path gốc.
- Cần verify checksum/size sau download.
- Cần xử lý Range request nếu client phát MP4 bằng byte-range:
  - Nếu file chưa restore xong, trả `202`.
  - Khi restore xong, request Range tiếp theo đọc local file bình thường.
- Với HLS, nên restore cả playlist window trước khi player request từng segment.
- Nếu object trên MinIO mất, update segment `MISSING` và job `FAILED`.

## 14. Thứ Tự Triển Khai Khuyến Nghị

1. Hoàn thiện `restore_jobs` repository và restore worker thực thi thật.
2. Thêm helper resolve segment từ `file_path`/`MediaInfo`.
3. Hook `HttpFileManager::accessFile` khi local file missing.
4. Trả `202 + code 300056 + job_id` khi COLD restore được trigger.
5. Tải object từ MinIO/S3 về HOT path hoặc restore cache.
6. Update `SegmentTierRange` và `RestoreJob` khi restore hoàn tất.
7. Bổ sung cleanup TTL cho restored file.
8. Bổ sung metrics/alert cho restore throughput, failures, queue depth.
