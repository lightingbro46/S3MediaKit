# API Lưu Trữ Phân Tầng

Tài liệu này mô tả các route đang được đăng ký trong `server/WebApiStorage.cpp` và payload thực tế mà backend trả về.

Luồng nội bộ: [storage-tiering-flow.md](storage-tiering-flow.md).

> Cần build với `-DENABLE_TIER_STORAGE=ON`. Option này mặc định `OFF`; khi tắt, nhóm API bên dưới không được đăng ký.

## 1. Quy ước chung

### 1.1 Base path và request

Tất cả route dùng prefix:

```text
/media/mserver/storage
```

Handler `API_ARGS_MAP` nhận query/form params; handler `API_ARGS_JSON` nhận JSON body. Route list/detail được backend đăng ký theo cùng cơ chế `api_regist`, không tách route GET và POST.

Timestamp trong API tiering là Unix timestamp theo giây. Pagination bắt đầu từ `page=0`; các list job/alert/range bảo vệ `size` trong khoảng `1..100`, nếu không hợp lệ thì dùng `20`.

### 1.2 Xác thực và permission

Mọi route đều gọi `CHECK_AUTH_TOKEN()`.

| Permission | Code | Phạm vi storage |
|------------|------|--------------------|
| Playback | `1003` | `playback/resolve`; kết hợp với quyền MediaServer cho restore/protected |
| Read MediaServer | `150301` | Xem pool, policy, camera storage, job, dashboard, alert |
| Modify MediaServer | `150302` | CRUD pool/policy, assignment, retry/cancel, acknowledge, approve/extend |

Khi endpoint truyền nhiều permission vào `CHECK_USER_PERMISSION`, `checkPermissionCode(...)` kết hợp bằng AND; user phải có đủ tất cả permission đó.

### 1.3 Response envelope

```json
{
  "code": 0,
  "msg": "",
  "data": {}
}
```

`code=0` là thành công. HTTP status của lỗi lấy trực tiếp từ `WebApiErrCode.h`; không suy ra từ tên business status trong `data`.

### 1.4 Mã lỗi liên quan

| Code | HTTP | Ý nghĩa |
|------|------|---------|
| `901001` | 401 | Chưa xác thực |
| `901004` | 401 | Không có quyền playback |
| `901006` | 401 | Không có quyền read MediaServer |
| `901007` | 401 | Không có quyền modify MediaServer |
| `902001` | 400 | Tham số không hợp lệ/thiếu |
| `902008` | 404 | Watermark percent không hợp lệ |
| `902009` | 404 | Khoảng thời gian không hợp lệ |
| `905001` | 404 | Camera không tồn tại |
| `914001` | 404 | Pool không tồn tại |
| `914002` | 404 | Policy không tồn tại |
| `914003` | 404 | Pool đang được tham chiếu |
| `914004` | 404 | Pool offline |
| `914005` | 404 | Kiểm tra kết nối pool thất bại |
| `914006` | 404 | Policy đang được camera dùng |
| `914007` | 404 | Policy không hợp lệ |
| `914008` | 404 | Thứ tự tier không hợp lệ |
| `914009` | 404 | Cần restore trước khi playback |
| `914010..914015` | 500 | CRUD pool/policy thất bại |
| `914016` | 404 | Không tìm thấy segment |
| `914017` | 404 | Segment đã hết hạn |
| `914018` | 404 | Restore job không tồn tại |
| `914019` | 500 | Tạo restore job thất bại |
| `914020` | 404 | Tiering job không tồn tại |
| `914021` | 500 | Update/retry tiering job thất bại |
| `914022` | 500 | Cancel tiering job thất bại |
| `914023` | 404 | Alert không tồn tại |
| `914024` | 404 | Protected range không tồn tại |
| `914025` | 500 | Tạo protected range thất bại |
| `914026..914028` | 500 | Clone/assign/unassign policy thất bại |
| `914029` | 400 | Không được xóa default HOT pool |
| `914030` | 400 | Không được xóa system default policy |

### 1.5 Enum và capability

```typescript
type StorageTier = 'HOT' | 'WARM' | 'COLD';
type StoragePoolType = 'LOCAL_DISK' | 'NAS' | 'MINIO' | 'S3' | 'ARCHIVE';
type StorageHealth = 'OK' | 'WARNING' | 'HIGH' | 'CRITICAL' | 'OFFLINE';
type SegmentStatus = 'AVAILABLE' | 'RESTORING' | 'EXPIRED' | 'DELETED' | 'MISSING';
type JobStatus = 'PENDING' | 'RUNNING' | 'DONE' | 'FAILED' | 'CANCELLED';
type PolicySource = 'CAMERA' | 'SYSTEM_DEFAULT';
```

Restore job có thêm status runtime `EXPIRED` sau khi cache quá TTL.

Capability thực tế từ `pool/options`:

| Tier | Type cho phép |
|------|----------------|
| HOT | `LOCAL_DISK`, `NAS` |
| WARM | `LOCAL_DISK`, `NAS` |
| COLD | `NAS`, `MINIO` |

`S3`/`ARCHIVE` có trong enum/backend nhưng validation API hiện không cho tạo pool bằng hai type này.

## 2. Storage pool

### 2.1 List pool

`GET/POST /media/mserver/storage/pool/list` — permission `150301`.

Params optional: `tier`, `type`, `status`, `keyword`.

```json
{
  "code": 0,
  "data": [{
    "id": "pool-default-hot",
    "name": "Default Hot Storage",
    "type": "LOCAL_DISK",
    "tier": "HOT",
    "status": "OK",
    "total_bytes": 1000000000,
    "used_bytes": 400000000,
    "free_bytes": 600000000,
    "used_percent": 40,
    "write_mbps": 0,
    "read_mbps": 0,
    "camera_count": 0,
    "enabled": true,
    "last_health_check": 1787562000
  }]
}
```

`write_mbps`, `read_mbps` và `camera_count` hiện là `0`.

### 2.2 Pool detail

`GET/POST /media/mserver/storage/pool/detail` — permission `150301`; required `id`.

Response gồm toàn bộ field của list và thêm:

```json
{
  "access_key": "***********",
  "secret_key": "***********",
  "endpoint": "http://minio:9000",
  "bucket": "record",
  "base_path": "traffic",
  "mount_path": "",
  "network_path": "",
  "high_watermark_percent": 85,
  "critical_watermark_percent": 90,
  "health_check_enabled": true,
  "created_at": 1787560000,
  "updated_at": 1787561000
}
```

Credential không được trả rõ; chuỗi mask chỉ cho biết field đã có giá trị.

### 2.3 Create pool

`POST /media/mserver/storage/pool/create` — JSON, permission `150302`.

Required chung: `name`, `type`, `tier`.

```json
{
  "name": "Cold MinIO",
  "type": "MINIO",
  "tier": "COLD",
  "endpoint": "http://10.0.0.10:9000",
  "bucket": "record",
  "base_path": "traffic",
  "access_key": "access",
  "secret_key": "secret",
  "enabled": true,
  "health_check_enabled": true,
  "high_watermark_percent": 85,
  "critical_watermark_percent": 90
}
```

Required theo type:

- `LOCAL_DISK`: `mount_path`.
- `NAS`: `mount_path` hoặc `network_path`.
- `MINIO`: `endpoint`, `bucket`, `base_path`, `access_key`, `secret_key`.

Response: `data.id` của pool mới.

### 2.4 Update pool

`POST /media/mserver/storage/pool/update` — JSON, permission `150302`; required `id`.

Endpoint chỉ merge các field: `name`, `enabled`, `health_check_enabled`, `high_watermark_percent`, `critical_watermark_percent`. Type, tier, path và credential không được update qua route này.

### 2.5 Delete pool

`POST /media/mserver/storage/pool/delete` — params, permission `150302`; required `id`.

Khi thất bại, `data` có:

```json
{
  "ref_count": 3,
  "is_default_pool": false
}
```

Pool mặc định hoặc pool còn bị policy/range tham chiếu không thể xóa.

### 2.6 Test connection

`POST /media/mserver/storage/pool/testConnection` — JSON, permission `150302`.

- Pool đã lưu: gửi `pool_id`.
- Pool chưa lưu: gửi trực tiếp các field kết nối như create.

```json
{
  "code": 0,
  "data": {
    "status": "OK",
    "latency_ms": 18,
    "can_read": true,
    "can_write": true,
    "message": "Connection successful"
  }
}
```

### 2.7 Pool options

`GET/POST /media/mserver/storage/pool/options` — permission `150301`, không cần params.

Response `data.poolsTypeSupport` là ma trận boolean cho HOT/WARM/COLD và năm pool type.

### 2.8 Mount point available

`GET/POST /media/mserver/storage/mountpoint/available` — permission `150301`.

Param optional `include` là danh sách storage type backend chuyển cho `getAvailableMountPoints()`.

```json
{
  "data": {
    "mount_point": [{
      "name": "/dev/sdb1",
      "mount": "/data/record",
      "used": 100,
      "total": 1000,
      "used_pct": 10.0
    }]
  }
}
```

## 3. Storage policy

### 3.1 List/detail

- `GET/POST /media/mserver/storage/policy/list` — permission `150301`; optional `keyword`, `enabled`, `page` (default `0`), `size` (default `20`). Response `data={items,page,size,total}`.
- `GET/POST /media/mserver/storage/policy/detail` — permission `150301`; required `id`. Response gồm policy chung, `tiers`, `delete_policy`, `advanced_rules`; mỗi tier có thêm `pool_name`.

Item list gồm:

```json
{
  "id": "policy-system-default",
  "name": "System Default",
  "description": "",
  "enabled": true,
  "total_retention_days": 30,
  "hot_retain_until_days": 30,
  "warm_retain_until_days": 0,
  "cold_retain_until_days": 0,
  "delete_after_days": 0,
  "allow_camera_override": true,
  "protect_event_video": false,
  "applied_camera_count": 0,
  "created_at": 1787560000,
  "updated_at": 1787560000
}
```

### 3.2 Create/update

- `POST /media/mserver/storage/policy/create` — JSON, permission `150302`; required `name`, `total_retention_days`.
- `POST /media/mserver/storage/policy/update` — JSON, permission `150302`; required `id`, `name`, `total_retention_days`.

Payload:

```json
{
  "id": "policy-traffic",
  "name": "Traffic",
  "description": "180-day retention",
  "enabled": true,
  "total_retention_days": 180,
  "allow_camera_override": true,
  "protect_event_video": true,
  "tiers": [{
    "tier": "HOT",
    "enabled": true,
    "pool_id": "pool-default-hot",
    "retain_until_days": 7,
    "data_mode": "FULL_VIDEO",
    "overflow_action": "MOVE_TO_NEXT_TIER",
    "high_watermark_percent": 85,
    "critical_watermark_percent": 90,
    "priority": "NORMAL"
  }],
  "delete_policy": {
    "delete_after_days": 180,
    "delete_mode": "DELETE_AUTOMATICALLY",
    "skip_protected_video": true,
    "skip_evidence_video": true,
    "require_approval_before_delete": false
  },
  "advanced_rules": {
    "enable_early_move_when_pool_high": true,
    "prefer_move_no_event_video_first": true,
    "prefer_keep_event_video_longer": true,
    "min_segment_age_minutes_before_move": 30
  }
}
```

Validation:

- HOT phải enabled.
- `retain_until_days` tăng dần giữa các tier enabled.
- Tier enabled kế tiếp yêu cầu source `overflow_action=MOVE_TO_NEXT_TIER`.
- Pool phải tồn tại, enabled, đúng tier và đúng capability.
- `high_watermark_percent < critical_watermark_percent <= 95`.
- `delete_after_days >= retain_until_days` lớn nhất.

### 3.3 Clone/delete

- `POST /media/mserver/storage/policy/clone` — params, permission `150302`; required `id`, `name`; trả `data.id`.
- `POST /media/mserver/storage/policy/delete` — params, permission `150302`; required `id`.

Delete failure trả `data.camera_count` và `data.is_default_policy`. `policy-system-default` không thể xóa.

## 4. Gán policy và effective policy

Backend hiện chỉ có camera override và system default. Không có route `assignProject`/`assignGroup`.

### 4.1 Assign camera

- `POST /media/mserver/storage/policy/assignCamera` — params, permission `150302`; required `camera_id`, `policy_id`; optional `override_reason`.
- `POST /media/mserver/storage/policy/assignCameras` — JSON, permission `150302`; required `policy_id`, `camera_ids[]`.

Bulk response:

```json
{
  "data": {
    "assigned_count": 2,
    "failed_ids": ["camera-not-found"]
  }
}
```

### 4.2 Remove override

- `POST /media/mserver/storage/policy/removeCamera` — params, permission `150302`; required `camera_id`.
- `POST /media/mserver/storage/policy/removeCameras` — JSON, permission `150302`; required `camera_ids[]`; trả `removed_count`, `failed_ids`.

Remove chỉ xóa row override; camera tự fallback về `policy-system-default`.

### 4.3 Effective policy

- `GET/POST /media/mserver/storage/camera/effectivePolicy` — permission `150301`; required `camera_id`.
- `GET/POST /media/mserver/storage/camera/effectivePolicy/list` — permission `150301`; optional `search` theo tên camera và `policy_id` theo effective id.

List chỉ duyệt camera hiện có trong `DeviceSource`; không union camera chỉ có assignment/range.

```json
{
  "camera_id": "cam-003",
  "camera_name": "Camera 003",
  "policy_id": "",
  "effective_policy_id": "policy-system-default",
  "policy_name": "System Default",
  "source": "SYSTEM_DEFAULT",
  "source_id": "",
  "allow_camera_override": true
}
```

Khi source là `CAMERA`, `policy_id=effective_policy_id` và `source_id=camera_id`.

## 5. Camera storage

### 5.1 Timeline

`GET/POST /media/mserver/storage/camera/timeline` — permission `150301`; required `camera_id`, `start_time`, `end_time`; optional `include_deleted`.

`include_motion` không phải request param được handler đọc. Response luôn có `has_motion`/`has_event` từ metadata range.

```json
{
  "data": {
    "camera_id": "cam-003",
    "camera_name": "Camera 003",
    "policy_id": "policy-system-default",
    "ranges": [{
      "start": 1787560000,
      "end": 1787560060,
      "tier": "COLD",
      "pool_id": "cold-minio",
      "status": "AVAILABLE",
      "segment_count": 1,
      "size_bytes": 52428800,
      "restore_required": true,
      "has_motion": false,
      "has_event": false
    }]
  }
}
```

`restore_required` dựa vào type của pool (`MINIO`/`S3`/`ARCHIVE`), không chỉ dựa vào `tier=COLD`; COLD NAS trả `false`.

### 5.2 Summary

- `GET/POST /media/mserver/storage/camera/summary` — permission `150301`; required `camera_id`.
- `GET/POST /media/mserver/storage/camera/summary/list` — permission `150301`; optional `search` theo tên camera; response là mảng trực tiếp trong `data`.

Mỗi summary:

```json
{
  "camera_id": "cam-003",
  "camera_name": "Camera 003",
  "policy_id": "policy-system-default",
  "policy_name": "System Default",
  "policy_source": "SYSTEM_DEFAULT",
  "total_size_bytes": 52428800,
  "total_segment_count": 1,
  "tier_summary": [{
    "tier": "COLD",
    "from_time": 1787560000,
    "to_time": 1787560060,
    "size_bytes": 52428800,
    "segment_count": 1
  }],
  "last_tiering_job_time": 1787559000,
  "status": "OK"
}
```

## 6. Playback và restore job

### 6.1 Resolve playback state

`POST /media/mserver/storage/playback/resolve` — params, permission `1003`; required `camera_id`, `start_time`, `end_time`; optional `stream_id`, `protocol` (default `HTTP_MP4`).

Handler không đọc `quality` và không tạo `playback_url`.

READY:

```json
{
  "code": 0,
  "data": {
    "status": "READY",
    "tier": "HOT",
    "restore_required": false,
    "job_created": false,
    "protocol": "HTTP_MP4",
    "expires_at": 1787563600
  }
}
```

RESTORE_REQUIRED/RESTORING:

```json
{
  "code": 914009,
  "msg": "Recording is in object storage and must be restored before playback",
  "data": {
    "status": "RESTORE_REQUIRED",
    "tier": "COLD",
    "restore_required": true,
    "job_created": false,
    "job_id": "",
    "restore_job_status": "",
    "protocol": "HTTP_MP4",
    "estimated_restore_seconds": 120
  }
}
```

Nếu có job overlap, `status=RESTORING`, kèm `job_id`/`restore_job_status`. Response này có HTTP 404 theo error map, không phải 202.

NOT_FOUND/EXPIRED/DELETED/MISSING trả lần lượt `914016` hoặc `914017` và `data.status` tương ứng.

### 6.2 Create restore job

`POST /media/mserver/storage/restoreJob/create` — params; cần cả permission `150301` và `1003`; required `camera_id`, `start_time`, `end_time`; optional `target_tier` (default `HOT`), `reason`.

```json
{
  "data": {
    "job_id": "rj-...",
    "status": "PENDING"
  }
}
```

> Giới hạn hiện tại: endpoint chỉ insert row `PENDING`, không dispatch restore worker. Auto-restore trong resolver replay mới là luồng gọi worker; một job API-created `PENDING` overlap còn có thể khiến auto-restore coi job đã được queue. FE chưa nên dùng endpoint này như lệnh download.

### 6.3 Restore job detail/list

- `GET/POST /media/mserver/storage/restoreJob/detail` — cần cả permission `150301` và `1003`; required `job_id`.
- `GET/POST /media/mserver/storage/restoreJob/list` — cần cả permission `150301` và `1003`; optional `camera_id`, `status`, `from_time`, `to_time`, `page`, `size`.

Detail gồm `job_id`, `type=RESTORE`, status/progress, camera/tier, time window, processed/total bytes, `playback_ready`, `created_at`, `updated_at`. Item list bỏ `type`, time window, processed bytes và updated time; thêm `camera_name`.

## 7. Tiering job

- `GET/POST /media/mserver/storage/tieringJob/list` — permission `150301`; optional `camera_id`, `status`, `source_tier`, `target_tier`, `from_time`, `to_time`, `page`, `size`.
- `GET/POST /media/mserver/storage/tieringJob/detail` — permission `150301`; required `job_id`.
- `POST /media/mserver/storage/tieringJob/retry` — permission `150302`; required `job_id`; đặt job về `PENDING`, bytes moved về `0`.
- `POST /media/mserver/storage/tieringJob/cancel` — permission `150302`; required `job_id`; chỉ cancel job chưa `DONE`/`FAILED`/`CANCELLED`.

Item list/detail gồm `job_id`, status/progress, camera/stream/range, source/target tier/pool, bytes và timestamps. `segment_count`/`processed_segment_count` hiện luôn `0`. Detail thêm `error_code` (`TIERING_JOB_FAILED` khi có message), `error_message`, `retryable`.

Tiering job `PENDING` được thực thi ở cycle 300 giây; không có API run-now.

## 8. Dashboard

### 8.1 Summary

`GET/POST /media/mserver/storage/dashboard/summary` — permission `150301`.

Response gồm `total_bytes`, `used_bytes`, `free_bytes`, `used_percent`, `status`, `tier_summary[]`, `active_tiering_jobs`, `failed_tiering_jobs`, `active_restore_jobs`, `alerts[]`.

Trong `tier_summary`, `estimated_remaining_days` và `write_mbps` hiện là `0`.

### 8.2 Detail

`GET/POST /media/mserver/storage/dashboard/detail` — JSON, permission `150301`; không cần body.

```json
{
  "data": {
    "summary": {},
    "pools": [],
    "policies": [],
    "configured_cameras": [],
    "job_counts": {
      "PENDING": 0,
      "RUNNING": 0,
      "DONE": 0,
      "FAILED": 0,
      "CANCELLED": 0
    },
    "recent_jobs": []
  }
}
```

Khác với summary gọn, detail dùng `TierStorageManager::getDashboardDetail()` và trả model raw của pool/policy/job.

## 9. Alert

- `GET/POST /media/mserver/storage/alert/list` — permission `150301`; optional `level`, `acknowledged`, `page`, `size`; response `data={items,page,size,total}`.
- `POST /media/mserver/storage/alert/ack` — permission `150302`; required `alert_id`.

Item alert: `id`, `level`, `type`, `pool_id`, `pool_name`, `message`, `created_at`, `acknowledged`.

> Lưu ý code hiện tại: parser của `acknowledged` kiểm tra nhầm `level.empty()` trước khi `stoi(allArgs["acknowledged"])`. Tránh gửi `level` mà bỏ trống `acknowledged`; filter này cần backend sửa để an toàn.

## 10. Expired range

- `GET/POST /media/mserver/storage/expiredSegment/list` — permission `150301`; optional `camera_id`, `from_time`, `to_time`, `page`, `size`.
- `POST /media/mserver/storage/expiredSegment/approve` — JSON, permission `150302`; required `range_ids[]`.
- `POST /media/mserver/storage/expiredSegment/extend` — JSON, permission `150302`; required `range_ids[]` (alias `segment_ids[]` được chấp nhận) và `extend_until`.

Item list gồm `segment_id` và `range_id` (cùng giá trị), camera/stream/time/tier/pool/count/size, `expired_at`, `protected`, `evidence` (hiện luôn `false`).

Approve trả `approved_count` và chỉ update metadata status sang `DELETED`. Extend trả `extended_count` và chỉ update status về `AVAILABLE`; `extend_until` chưa được lưu.

`project_id`, `reason` không được các handler này đọc.

## 11. Protected video

- `POST /media/mserver/storage/protected/create` — params; cần cả permission `1003` và `150302`; `start_time`, `end_time` phải tạo khoảng hợp lệ; request cần gửi `camera_id`; optional `type` (default `PROTECTED`), `reason`; trả `protected_id`.
- `POST /media/mserver/storage/protected/delete` — params; cần cả permission `1003` và `150302`; required `protected_id`.
- `GET/POST /media/mserver/storage/protected/list` — cần cả permission `1003` và `150301`; optional `camera_id`, `type`, `from_time`, `to_time`, `page`, `size`.

Backend lưu `type` như chuỗi, không validate giới hạn enum. Các giá trị quy ước: `PROTECTED`, `EVIDENCE`, `LOCKED_SEGMENT`.

Handler create hiện chưa `CHECK_ARGS_("camera_id")` và chưa kiểm tra camera tồn tại; client vẫn phải coi `camera_id` là field bắt buộc.

## 12. Luồng tích hợp khuyến nghị hiện tại

### 12.1 Quản trị pool/policy

```text
pool/options + mountpoint/available
  -> pool/testConnection
  -> pool/create
  -> policy/create
  -> policy/assignCamera(s)
  -> camera/effectivePolicy hoặc effectivePolicy/list
```

### 12.2 Timeline và replay

```text
camera/timeline
  -> playback/resolve
  -> READY: dùng luồng playback/replay hiện hữu của hệ thống
  -> RESTORE_REQUIRED: thông báo chưa sẵn sàng
```

Auto-restore chỉ chạy khi server bật `storage.auto_restore_on_record_access=1` và luồng replay truy vấn `TimeBlock`. FE có thể poll `restoreJob/list/detail` để quan sát job do backend tự tạo, sau đó retry playback.

Không dùng `restoreJob/create` làm trigger production cho đến khi backend nối endpoint với worker.

## 13. Chức năng chưa có

- Không có `/storage/policy/assignProject` hoặc `/storage/policy/assignGroup`.
- Không có WebSocket/SSE `/ws/storage/events`; FE phải polling nếu cần realtime.
- `playback/resolve` không trả `playback_url`.
- `restoreJob/create` chưa dispatch restore worker.
- `storage.auto_restore_max_concurrent` đã khai báo nhưng chưa được enforce.
- HTTP static-file miss không có hook trả `202`; restore được kích hoạt trong resolver `TimeBlock` của replay MP4.
