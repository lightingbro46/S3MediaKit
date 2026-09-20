# Backlog: API `/media/*` Còn Blocking Trên EventPoller

Kết quả rà soát toàn bộ **134 route `/media/*`** (2026-09-20, nhánh `feature/api-blocking-audit`).
Đây là danh sách việc cần làm, không phải mô tả hiện trạng kiến trúc — mô hình thread và cách
dispatch nằm ở [threading-blocking-work.md](threading-blocking-work.md).

Tài liệu này **gộp luôn phần còn lại** của hai plan cũ (`docs/features/worker.md` và
`docs/sync-followup-plan.md`), xem [§5](#5-tồn-đọng-gộp-từ-hai-plan-cũ). Sau khi hai file đó được
xoá, đây là nguồn duy nhất cho việc còn phải làm.

## Cách rà soát

Script phân tích từng thân handler `api_regist("/media/...")`, xác định phạm vi **lambda đầu tiên**
của `dispatchApiWork` / `runApiWork` / `runApiCallbackWork` / `runSearch` / `lookupOwner` (lambda đó
chạy trên worker; mọi lambda còn lại — completion, done — chạy trên poller của request), rồi tìm các
lời gọi blocking **nằm ngoài** phạm vi đó.

Từ 134 route: 39 route bị gắn cờ, sau khi soi tay còn **34 route cần sửa thật**. Phần false positive
được liệt kê ở [§6](#6-đã-kiểm-tra-và-không-cần-sửa) để lần sau không phải soi lại.

Mẫu nhận biết blocking: `*Imp`/`Repository`/`findBy*` (SQLite), `File::*`/`::stat`/`scanDir`,
`TimeQuery`/`MultiTimeDemuxer`, `TierStorageManager::Instance()`, `FFmpegSnap`/`FFmpegProbe`/
`FFmpegOverlayFilter`, `OnvifControl`, `SearchEngine::`/`MotionSearch`.

## Tổng quan ưu tiên

| Ưu tiên | Nhóm | Số route | Lý do |
|---|---|---|---|
| **P1** | [§1 Thumbnail](#1-p1--thumbnail-2-route--1-lỗi-lifecycle) | 2 | Nặng nhất: timefile + overlay ghi disk + tier resolve, tất cả trên poller |
| **P1** | [§2 `/media/api/storage/*`](#2-p1--họ-mediaapistorage-25-route) | 25 | Cả họ endpoint bị bỏ sót hoàn toàn khi refactor `/media/mserver/storage/*` |
| **P2** | [§3 Storage còn sót](#3-p2--storage-còn-sót-1-route) | 1 | `playback/resolve` nằm trong đường phát lại |
| **P2** | [§4 WebApi.cpp lẻ](#4-p2--các-route-lẻ-trong-webapicpp-3-route) | 3 | `getSyncStatus`, `listFiles`, `systemStatisticHistory` |
| P2 | [§5.2 Backpressure](#52-các-mục-còn-lại) | — | Pool `api` chưa có giới hạn tổng, mới có theo nhóm |
| P3 | [§4](#4-p2--các-route-lẻ-trong-webapicpp-3-route) | 1 | `serveFile` — một `::stat`, chỉ đáng lo trên NFS |

Thứ tự đề xuất: **§1.1** (sửa `RecordedThumbnailTask::start()`, nhỏ và độc lập) → **§5.2
backpressure tổng** (làm trước khi đổ thêm 25 endpoint vào pool `api`) → **§2** → **§1** → **§3** →
**§4**.

---

## 1. P1 — Thumbnail (2 route + 1 lỗi lifecycle)

Hai endpoint dùng chung một thân xử lý và **không có dispatch nào** cho phần nặng.

| Route | File:dòng | Trạng thái |
|---|---|---|
| `/media/esc/recordedThumnail` | [WebApiPlayback.cpp:113](../server/WebApiPlayback.cpp#L113) | Chưa dispatch gì cả |
| `/media/esc/bookmark/recordThumbnail` | [WebApiBookmark.cpp:409](../server/WebApiBookmark.cpp#L409) | Chỉ dispatch phần tra owner node + đọc bookmark ([:426](../server/WebApiBookmark.cpp#L426)); từ `on_access` ([:452](../server/WebApiBookmark.cpp#L452)) trở đi quay lại poller |

Các lời gọi blocking trên poller của request (số dòng theo `recordedThumnail`):

| Dòng | Việc |
|---|---|
| [:128](../server/WebApiPlayback.cpp#L128), [:130](../server/WebApiPlayback.cpp#L130) | `SearchEngine::findCurrentOwnerNode` / `findOwnerNodeAtTime` → `VmsResourceAssignmentImp` = SQLite |
| [:142](../server/WebApiPlayback.cpp#L142) | `new TimeQuery(tuple)` → `MultiTimeDemuxer::openFile()` = mở timefile từ disk |
| [:174](../server/WebApiPlayback.cpp#L174) | `query->getLastBlock()` |
| [:191](../server/WebApiPlayback.cpp#L191) | `query->getRecordedTimePeriod()` (đồng bộ; callback chỉ là cách trả kết quả) |
| [:222](../server/WebApiPlayback.cpp#L222) | `buildThumbnailOverlay()` → lấy policy watermark/privacy + `FFmpegOverlayFilter::build` **ghi file SVG mask ra disk** (`File::create_path` + `File::saveFile`) |
| [:233](../server/WebApiPlayback.cpp#L233), [:240](../server/WebApiPlayback.cpp#L240) | `File::fileExist` / `fileSize` / `delete_file` |
| [:280](../server/WebApiPlayback.cpp#L280) | `task->start()` — xem dưới |

### 1.1 Lỗi lifecycle: `RecordedThumbnailTask::start()` chạy tick đầu tiên inline

[FFmpegSource.cpp:754](../server/FFmpegSource.cpp#L754) gọi `tick()` **đồng bộ trên thread gọi**, và
`tick()` gọi `TierStorageManager::resolveObjectReadSource()`
([FFmpegSource.cpp:799](../server/FFmpegSource.cpp#L799)) — SQLite, và với segment nằm trên object
storage thì có thể đi tới MinIO/S3. Các tick *sau* chạy trên `_poller`, chỉ tick đầu nằm sai chỗ.

**Sửa:** `start()` post `tick()` đầu tiên sang `_poller` thay vì gọi inline — giống cách
`FFmpegExtractor::makeExtract` đã làm với `prepareExtractSource`
([FFmpegSource.cpp:1360](../server/FFmpegSource.cpp#L1360)). Sửa chỗ này là hết blocking cho cả hai
endpoint ở bước `task->start()`, độc lập với việc dispatch handler.

### 1.2 Hướng làm

1. Sửa `RecordedThumbnailTask::start()` trước (1.1) — nhỏ, độc lập, có lợi ngay.
2. Gom toàn bộ đoạn từ owner lookup tới `task->start()` vào một `runApiWork`, trả lời bằng
   `runApiCallbackWork` vì `RecordedThumbnailTask` trả kết quả qua callback muộn.
3. Nhánh forward sang node sở hữu ([:133](../server/WebApiPlayback.cpp#L133),
   `kBroadcastSyncThumbnail`) **giữ nguyên** — HTTP bất đồng bộ, không blocking.
4. `responseSnap` → `invoker.responseFile()` giữ trên poller của request (session affinity).
5. Cân nhắc `apiWorkLimit("media.thumbnail")` vì overlay + ffmpeg khá nặng.

Phần chờ ffmpeg đã ổn từ 2026-09-20: `startSnap` dùng `makeSnapOnWorker` →
`BlockingExecutors::ffmpeg()`.

---

## 2. P1 — Họ `/media/api/storage/*` (25 route)

**Phát hiện đáng kể nhất của đợt rà soát.** `WebApi.cpp` có một họ endpoint storage **song song**
với `/media/mserver/storage/*`, dùng `CHECK_SECRET()` / `CHECK_CLUSTER_AUTHOR_ASYNC` thay vì JWT
người dùng. Đây **không phải** forwarder — chúng tự gọi `TierStorageManager` và repository trực tiếp
trên poller của request. Bước 3 và Bước 4 của đợt refactor trước chỉ đụng tới `/media/mserver/...`,
nên cả họ này bị bỏ sót.

27 route trong họ, 25 route còn blocking:

| Route | WebApi.cpp | Ghi chú |
|---|---|---|
| `/media/api/storage/dashboard/detail` | 2766 | `getDashboardDetail()` |
| `/media/api/storage/alert/list` | 2777 | `StorageAlertImp::query` + `countQuery` + `loadPoolMap` |
| `/media/api/storage/restoreJob/list` | 2798 | |
| `/media/api/storage/tieringJob/list` | 2821 | |
| `/media/api/storage/expiredSegment/list` | 2846 | |
| `/media/api/storage/protected/list` | 2872 | |
| `/media/api/storage/mountpoint/available` | 2905 | quét mount |
| `/media/api/storage/pool/testConnection` | 2920 | **network/AWS**, chậm nhất nhóm |
| `/media/api/storage/camera/effectivePolicy/list` | 2962 | |
| `/media/api/storage/camera/timeline` | 2989 | |
| `/media/api/storage/camera/summary` | 3040 | |
| `/media/api/storage/pool/list` | 3082 | |
| `/media/api/storage/pool/detail` | 3096 | |
| `/media/api/storage/pool/create` | 3108 | mutation |
| `/media/api/storage/pool/update` | 3130 | mutation |
| `/media/api/storage/pool/delete` | 3160 | mutation |
| `/media/api/storage/policy/list` | 3182 | |
| `/media/api/storage/policy/detail` | 3203 | |
| `/media/api/storage/policy/create` | 3216 | mutation |
| `/media/api/storage/policy/update` | 3235 | mutation |
| `/media/api/storage/policy/delete` | 3258 | mutation |
| `/media/api/storage/policy/assignCamera` | 3285 | mutation |
| `/media/api/storage/policy/assignCameras` | 3306 | mutation |
| `/media/api/storage/policy/removeCamera` | 3337 | mutation |
| `/media/api/storage/policy/removeCameras` | 3352 | mutation |

Đã kiểm tra, **không cần sửa**: `/media/api/storage/dashboard/summary` (2772) và
`/media/api/storage/pool/options` (2896) — chỉ đọc cache/hằng số.

### 2.1 Hướng làm

- Đọc: `dispatchApiWork(..., apiWorkLimit("storage.read"))`.
- **Mutation: bắt buộc dùng `apiWorkSerial("storage.mutation")`** — cùng nhóm với các mutation ở
  `WebApiStorage.cpp` đã làm ở Bước 4, nếu không thì hai họ endpoint vẫn ghi chồng nhau lên cùng
  cache pool/policy và cùng đăng ký storage. Đây là điểm dễ bỏ sót nhất.
- `pool/testConnection` chạy worker nhưng **không** serial (giống `/media/mserver/...`).
- Giữ nguyên `CHECK_SECRET()` / `CHECK_CLUSTER_AUTHOR_ASYNC` trên poller của request trước khi
  dispatch: đây là trust boundary khác với API người dùng, đừng gộp.
- Không đổi route/payload/mã lỗi.

> **Cân nhắc trước khi làm:** hai họ endpoint này trùng chức năng gần như hoàn toàn. Nếu định gộp
> `/media/api/storage/*` thành forwarder gọi lại handler của `/media/mserver/storage/*`, hãy làm
> việc đó **trước**, rồi mới dispatch — sẽ đỡ 25 lần sửa lặp. Nhưng đổi cấu trúc này chạm trust
> boundary, cần quyết định riêng.

---

## 3. P2 — Storage còn sót (1 route)

| Route | File:dòng |
|---|---|
| `/media/mserver/storage/playback/resolve` | [WebApiStorage.cpp:846](../server/WebApiStorage.cpp#L846) |

Không có dispatch. Trên poller của request:

- [:872](../server/WebApiStorage.cpp#L872) `TierStorageBase::loadPoolMap()`
- [:873](../server/WebApiStorage.cpp#L873) `SegmentTierRangeImp::queryByCamera()`
- [:950](../server/WebApiStorage.cpp#L950) truy vấn restore job
- [:961](../server/WebApiStorage.cpp#L961) `TierStorageManager::validateRestoreWindowCache()`
- [:987](../server/WebApiStorage.cpp#L987) `isPoolAvailableForPlayback()` cho **từng pool**

Endpoint này nằm trong đường người dùng bấm phát lại, nên độ trễ ở đây thấy rõ.
Hướng làm: `dispatchApiWork` toàn bộ `on_access`, giữ `CHECK_USER_DEVICE_AUTHOR_ASYNC` trước.

---

## 4. P2 — Các route lẻ trong `WebApi.cpp` (3 route)

| Route | WebApi.cpp | Việc blocking | Ưu tiên |
|---|---|---|---|
| `/media/api/getSyncStatus` | 2504 | `TransactionSequenceImp::findAll()`, `PeerAckLogImp::findAll()`, rồi `TransactionLogImp::findSinceSeq()` **cho từng peer** — quét toàn bảng, `transaction_log` có thể rất lớn | P2 |
| `/media/api/listFiles` | 2600 | `::stat` + `File::scanDir`, **2 lần `::stat` cho mỗi entry** trong thư mục | P2 |
| `/media/api/systemStatisticHistory` | 2737 | `getSystemStatisticHistory()` đọc DB thống kê | P2 |
| `/media/api/serveFile` | 2666 | Một `::stat` rồi `responseFile` | P3 |

`/media/api/systemStatisticHistory` là **bất đối xứng rõ**: bản sinh đôi
`/media/mserver/systemStatisticHistory` ([WebApiMonitor.cpp:29](../server/WebApiMonitor.cpp#L29)) đã
`dispatchApiWork` từ Bước 3, bản `/media/api/...` thì chưa. Copy y nguyên cách làm bên kia.

`serveFile` chỉ có một `::stat`; trên đĩa local là không đáng kể, chỉ đáng làm nếu
`api.downloadRoot` nằm trên NFS/mount mạng.

---

## 5. Tồn đọng gộp từ hai plan cũ

### 5.1 Trạng thái

| Plan | Trạng thái |
|---|---|
| `docs/sync-followup-plan.md` (P0–P8) | **Xong hết.** Đã kiểm chứng cả hai mục "chỉ sửa tài liệu" của P8: ghi chú mật khẩu camera nằm trong dữ liệu đồng bộ có ở [api-sync.md:449](api-sync.md), ghi chú chưa có `ownershipGeneration` có ở [api-sync.md:47](api-sync.md) |
| `docs/features/worker.md` (Bước 1–6) | Các bước đều đánh dấu xong; còn 3 mục ở [§5.2](#52-các-mục-còn-lại), và một lỗ hổng phạm vi ở [§5.3](#53-lỗ-hổng-phạm-vi-của-workermd) |

### 5.2 Các mục còn lại

| Mục | Nguồn | Xử lý |
|---|---|---|
| **Backpressure tổng cho pool `api`** | `worker.md` mục 6 phần đánh giá, và "Còn tồn đọng" | **Chưa làm** — xem dưới |
| Phần tạo thumbnail của `bookmark/recordThumbnail` (TimeQuery/ffmpeg) | `worker.md` Bước 2 ghi "để Bước 5", nhưng Bước 5 chỉ làm extraction | Đã gộp vào [§1](#1-p1--thumbnail-2-route--1-lỗi-lifecycle) |
| `/index/api/getSnap` chọn thread theo query param `async` | `worker.md` "Chưa làm (cố ý)" | Cố ý bỏ, xem [§7](#7-ngoài-phạm-vi) |

**Backpressure tổng.** `ApiWorkLimit` hiện chỉ giới hạn **theo nhóm**
(`sync.snapshot` 2, `sync.changes` 8, `device.discovery` 4). Không có trần cho toàn pool `api`, nên
nhiều nhóm cùng bận vẫn có thể lấp kín mọi thread và làm hàng đợi dài vô hạn — request đã bị client
timeout vẫn chạy tiếp. Hướng làm:

- Một bộ đếm in-flight toàn cục cho `BlockingExecutors::api()`, config
  `api.limit.global.maxInflight` / `maxQueueMs`, vượt thì trả 503 `CODE_SERVER_BUSY` (902012) như
  giới hạn theo nhóm.
- Giới hạn nhóm vẫn được kiểm tra trước, để một nhóm nặng không chiếm hết hạn mức chung.
- Cần đặt mặc định đủ rộng để không làm đổi hành vi của các endpoint hiện có.
- Test: mở rộng `unit_api_work_dispatcher` (vượt trần chung → 503 đúng một lần, bộ đếm giảm đúng cả
  khi work ném exception, giới hạn nhóm vẫn ưu tiên).

Việc này **nên làm trước** [§2](#2-p1--họ-mediaapistorage-25-route): thêm 25 endpoint nữa vào pool
`api` mà chưa có trần chung thì rủi ro lấp pool tăng lên đáng kể.

### 5.3 Lỗ hổng phạm vi của `worker.md`

`worker.md` tuyên bố phạm vi là "**chỉ các API có route `/media/*`**", và "Exit criteria" ghi *"tất
cả blocking DB/filesystem/S3/HTTP/FFmpeg trong nhóm `/media/*` đã được phân loại"*. Thực tế cả 6
bước chỉ đụng tới `/media/mserver/*` và `/media/esc/*`.

Họ `/media/api/*` (trust boundary `CHECK_SECRET` / `CHECK_CLUSTER_AUTHOR_ASYNC`) **chưa từng được
phân loại** — đó chính là [§2](#2-p1--họ-mediaapistorage-25-route) và
[§4](#4-p2--các-route-lẻ-trong-webapicpp-3-route) của tài liệu này, 28 route. Nói cách khác, exit
criteria của `worker.md` chưa đạt, và đợt làm tiếp theo mới đóng được nó.

---

## 6. Đã kiểm tra và không cần sửa

Ghi lại để lần rà soát sau không mất công soi lại.

| Route / vị trí | Vì sao không cần |
|---|---|
| `/media/mserver/systemStatistic`, `/media/api/systemStatistic` | `makeSystemStatisticJson()` chỉ đọc snapshot đã cache của các monitor (`getCurrentUsage()`), không I/O |
| `/media/mserver/reader/available` | `isReaderCountAvailable()` = biến đếm trong bộ nhớ |
| `/media/mserver/description` | `getOsInfo()` trả `_info` đã cache; `estimateMaxAvailableDevice()` đọc cache + `mINI` |
| `/media/mserver/device/onvifSetVideoConfigs` | `loadVideoEncoderConfig()` là mutex + map trong bộ nhớ ([CameraStatistic.cpp:1134](../manager/Camera/CameraStatistic.cpp#L1134)), không SQLite |
| `/media/esc/extractArchived/list` | `src->status()` đọc trạng thái trong bộ nhớ có mutex |
| `/media/esc/recordedTimePeriod`, `/media/esc/searchMotion` | Đã dùng `runApiCallbackWork`; script gắn cờ nhầm do lambda có `mutable` trước `{` (đã sửa script) |
| `/media/mserver/device/subnetScan` | Đã `dispatchApiWork`; dòng bị gắn cờ thuộc helper `addSubnetScan` nằm giữa hai `api_regist` |
| `/media/esc/bookmark/search` `/detail` `/recent` | Dùng wrapper `runSearch()` ([WebApiBookmark.cpp:29](../server/WebApiBookmark.cpp#L29)) bọc `runApiCallbackWork` |
| `/media/esc/bookmark/create` `/update` | Dùng wrapper `lookupOwner()`; phần ghi DB nằm trong lambda worker |
| Nhánh forward sang node sở hữu (bookmark, motion, playback, thumbnail) | Routing theo node, không phải quyết định threading; nhánh forward là HTTP bất đồng bộ thật |
| `ptz_control*`, `imageMoveControl`, `relayOutputControl`, `speaker/*`, `validateCredential` | `CameraController`/`SpeakerController` đã đẩy SOAP/SDK sang `BlockingExecutors::device()` |
| `/media/live/*`, `/media/record/*` | Media transport, không phải API handler |

---

## 7. Ngoài phạm vi

- **`/index/api/getSnap`** — client chọn thread qua query param `async`, nhánh `async=0` chờ ffmpeg
  trên một poller của `WorkThreadPool`. Là API của repo gốc, đã thống nhất giữ nguyên contract. Nếu
  đổi ý: cho `FFmpegSnap::makeSnap` gọi `makeSnapOnWorker` thay vì `WorkThreadPool` — một dòng,
  không đổi signature.
- **Decode/transcode** vẫn chạy trên `WorkThreadPool`; chưa có pool riêng, chưa đánh giá.
- **Chưa có giới hạn tổng** số request đồng thời cho pool `api`; mới có `ApiWorkLimit` theo nhóm.

---

## 8. Kiểm chứng sau khi sửa

Mỗi nhóm xong thì chạy:

```bash
cmake --build build -j2                       # máy dev chỉ build được với -j2
ctest --test-dir build-test --output-on-failure -R unit.
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_worker_apis workerapis
```

Cần giữ nguyên: route, `code`/`msg`/`data`, phân trang, lỗi auth/permission, mask credential, và
mỗi request gọi `invoker` đúng một lần.

Nên bổ sung vào `tests/integration/sync_cluster/check_worker_apis.py`: gọi endpoint nặng của nhóm
vừa sửa rồi đo `/media/mserver/healthcheck` song song, giống cách mục 1/2 đang làm với discovery.

### Chạy lại script rà soát

Script nằm ở `/tmp` nên không theo repo. Cách dựng lại: duyệt từng `api_regist("/media/...")`, xoá
comment và string literal, xác định phạm vi **lambda đầu tiên** của mỗi
`dispatchApiWork`/`runApiWork`/`runApiCallbackWork`/`runSearch`/`lookupOwner`, rồi báo các mẫu
blocking nằm ngoài phạm vi đó. Hai cái bẫy đã gặp:

1. Lambda có `mutable` / `noexcept` / `-> T` giữa `)` và `{` → nhận diện phạm vi hỏng, báo nhầm.
2. Helper định nghĩa giữa hai `api_regist` bị tính vào handler phía trước.
