# Backlog: API `/media/*` Còn Blocking Trên EventPoller

Kết quả rà soát toàn bộ **134 route `/media/*`** (2026-09-20, nhánh `feature/api-blocking-audit`),
và trạng thái sau đợt sửa cùng ngày. Mô hình thread và cách dispatch nằm ở
[threading-blocking-work.md](threading-blocking-work.md).

Tài liệu này **gộp luôn phần còn lại** của hai plan cũ (`docs/features/worker.md` và
`docs/sync-followup-plan.md`, cả hai đã xoá), xem [§5](#5-tồn-đọng-gộp-từ-hai-plan-cũ).

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

## Tổng quan trạng thái

| Ưu tiên | Nhóm | Số route | Trạng thái |
|---|---|---|---|
| **P1** | [§1 Thumbnail](#1-p1--thumbnail-2-route--1-lỗi-lifecycle) | 2 | **Xong** 2026-09-20 |
| **P1** | [§2 `/media/api/storage/*`](#2-p1--họ-mediaapistorage-25-route) | 25 | **Xong** 2026-09-20 |
| **P2** | [§3 Storage còn sót](#3-p2--storage-còn-sót-1-route) | 1 | **Xong** 2026-09-20 |
| **P2** | [§4 WebApi.cpp lẻ](#4-p2--các-route-lẻ-trong-webapicpp-4-route) | 4 | **Xong** 2026-09-20 (kể cả `serveFile` P3) |
| P2 | [§5.2 Backpressure](#52-các-mục-còn-lại) | — | **Xong** 2026-09-20 — trần tổng `api.limit.global.*` |

Thứ tự đã làm: §1.1 → §5.2 backpressure tổng → §2 → §1 → §3 → §4.

---

## 1. P1 — Thumbnail (2 route + 1 lỗi lifecycle) — XONG

Hai endpoint dùng chung một thân xử lý và trước đó **không có dispatch nào** cho phần nặng.

| Route | File | Đã làm |
|---|---|---|
| `/media/esc/recordedThumnail` | [WebApiPlayback.cpp](../server/WebApiPlayback.cpp) | Toàn bộ thân `on_access` vào `runApiCallbackWork`, nhóm `media.thumbnail` |
| `/media/esc/bookmark/recordThumbnail` | [WebApiBookmark.cpp](../server/WebApiBookmark.cpp) | Phần từ `on_access` trở đi (TimeQuery + overlay + task) vào `runApiCallbackWork`, cùng nhóm |

Các lời gọi blocking trước đây nằm trên poller của request, nay chạy trên worker:
`SearchEngine::findCurrentOwnerNode` / `findOwnerNodeAtTime`, `new TimeQuery(tuple)`
(`MultiTimeDemuxer::openFile()`), `query->getLastBlock()`, `query->getRecordedTimePeriod()`,
`buildThumbnailOverlay()` (ghi file SVG mask ra disk), `File::fileExist`/`fileSize`/`delete_file`,
`task->start()`.

Mọi câu trả lời đi qua `reply(...)` của `runApiCallbackWork`, nên `responseSnap` /
`invoker.responseFile` vẫn chạy trên poller của request (session affinity). Nhánh forward sang node
sở hữu (`kBroadcastSyncThumbnail`, `kBroadcastSyncBookmarkThumbnail`) cũng được emit qua `reply`,
giữ nguyên hành vi HTTP bất đồng bộ cũ.

### 1.1 Lỗi lifecycle: `RecordedThumbnailTask::start()` chạy tick đầu tiên inline — XONG

`start()` gọi `tick()` **đồng bộ trên thread gọi**, và `tick()` gọi
`TierStorageManager::resolveObjectReadSource()` — SQLite, và với segment nằm trên object storage thì
có thể đi tới MinIO/S3. Các tick *sau* chạy trên `_poller`, chỉ tick đầu nằm sai chỗ.

**Đã sửa:** `start()` post `tick()` đầu tiên sang `_poller` thay vì gọi inline
([FFmpegSource.cpp](../server/FFmpegSource.cpp)), giống cách `FFmpegExtractor::makeExtract` làm với
`prepareExtractSource`.

---

## 2. P1 — Họ `/media/api/storage/*` (25 route) — XONG

**Phát hiện đáng kể nhất của đợt rà soát.** `WebApi.cpp` có một họ endpoint storage **song song**
với `/media/mserver/storage/*`, dùng `CHECK_SECRET()` / `CHECK_CLUSTER_AUTHOR_ASYNC` thay vì JWT
người dùng. Đây **không phải** forwarder — chúng tự gọi `TierStorageManager` và repository trực tiếp
trên poller của request. Bước 3 và Bước 4 của đợt refactor trước chỉ đụng tới `/media/mserver/...`,
nên cả họ này bị bỏ sót.

Đã dispatch 25/27 route trong họ:

- **Đọc** (14 route: `dashboard/detail`, `alert/list`, `restoreJob/list`, `tieringJob/list`,
  `expiredSegment/list`, `protected/list`, `mountpoint/available`, `camera/effectivePolicy/list`,
  `camera/timeline`, `camera/summary`, `pool/list`, `pool/detail`, `policy/list`, `policy/detail`):
  `dispatchApiWork(..., apiWorkLimit("storage.read"))`.
- **Mutation** (10 route: `pool/create|update|delete`, `policy/create|update|delete`,
  `policy/assignCamera|assignCameras|removeCamera|removeCameras`):
  `apiWorkSerial("storage.mutation")` — **cùng nhóm** với các mutation ở `WebApiStorage.cpp`, nếu
  không thì hai họ endpoint sẽ ghi chồng nhau lên cùng cache pool/policy và cùng đăng ký storage.
- `pool/testConnection` chạy worker nhưng **không** serial (giống `/media/mserver/...`): nó không ghi
  gì cả.
- `pool/list` đổi từ `API_ARGS_MAP` (đồng bộ) sang `API_ARGS_MAP_ASYNC`. Contract không đổi: đường
  đồng bộ cũng chỉ trả `invoker(200, headerOut, val)`.
- `RETURN_API_RESPONSE` trong phần đã chuyển sang worker đổi thành `throw ApiResponseException` —
  `dispatchApiWork` giữ nguyên các trường đã gán vào `val` (ví dụ `ref_count`, `is_default_pool`).

`CHECK_SECRET()` / `CHECK_CLUSTER_AUTHOR_ASYNC` vẫn nằm trên poller của request trước khi dispatch:
đây là trust boundary khác với API người dùng, không gộp. Route, payload, mã lỗi không đổi.

Đã kiểm tra, **không cần sửa**: `/media/api/storage/dashboard/summary` và
`/media/api/storage/pool/options` — chỉ đọc cache/hằng số.

> **Còn tồn đọng (không làm trong đợt này):** hai họ endpoint này trùng chức năng gần như hoàn toàn.
> Gộp `/media/api/storage/*` thành forwarder gọi lại handler của `/media/mserver/storage/*` sẽ bỏ
> được 25 bản sao, nhưng chạm trust boundary nên cần quyết định riêng.

---

## 3. P2 — Storage còn sót (1 route) — XONG

`/media/mserver/storage/playback/resolve` ([WebApiStorage.cpp](../server/WebApiStorage.cpp)) trước
đây không có dispatch. Toàn bộ `on_access` nay chạy trong `dispatchApiWork`, giữ
`CHECK_USER_DEVICE_AUTHOR_ASYNC` trên poller của request. **Không** gắn nhóm `storage.read`: đây là
đường người dùng bấm phát lại, không nên chia hạn mức với các endpoint liệt kê của trang quản trị;
nó chỉ chịu trần chung `global`.

Phần đã chuyển: `TierStorageBase::loadPoolMap()`, `SegmentTierRangeImp::queryByCamera()`, truy vấn
restore job, `TierStorageManager::validateRestoreWindowCache()`, và
`isPoolAvailableForPlayback()` cho **từng pool**. Endpoint này nằm trong đường người dùng bấm phát
lại, nên độ trễ ở đây thấy rõ.

---

## 4. P2 — Các route lẻ trong `WebApi.cpp` (4 route) — XONG

| Route | Việc blocking | Đã làm |
|---|---|---|
| `/media/api/getSyncStatus` | `TransactionSequenceImp::findAll()`, `PeerAckLogImp::findAll()`, rồi `TransactionLogImp::findSinceSeq()` **cho từng peer** — quét toàn bảng | `dispatchApiWork` bên trong `on_access` của `CHECK_CLUSTER_AUTHOR_ASYNC` |
| `/media/api/listFiles` | `::stat` + `File::scanDir`, **2 lần `::stat` cho mỗi entry** | Đổi sang `API_ARGS_MAP_ASYNC` + `dispatchApiWork`; các nhánh lỗi vẫn gán `val["code"]`/`val["msg"]` và trả 200 y như cũ |
| `/media/api/systemStatisticHistory` | `getSystemStatisticHistory()` đọc DB thống kê | `dispatchApiWork`, copy y nguyên cách làm của bản sinh đôi `/media/mserver/systemStatisticHistory` ([WebApiMonitor.cpp](../server/WebApiMonitor.cpp)) |
| `/media/api/serveFile` (P3) | Một `::stat` rồi `responseFile` | `runApiWork`: chỉ `::stat` chạy trên worker, `responseFile` vẫn trên poller của request vì session sở hữu việc stream file |

---

## 5. Tồn đọng gộp từ hai plan cũ

### 5.1 Trạng thái

| Plan | Trạng thái |
|---|---|
| `docs/sync-followup-plan.md` (P0–P8) | **Xong hết.** Đã kiểm chứng cả hai mục "chỉ sửa tài liệu" của P8: ghi chú mật khẩu camera nằm trong dữ liệu đồng bộ có ở [api-sync.md:449](api-sync.md), ghi chú chưa có `ownershipGeneration` có ở [api-sync.md:47](api-sync.md) |
| `docs/features/worker.md` (Bước 1–6) | **Xong hết**, kể cả 3 mục ở [§5.2](#52-các-mục-còn-lại) và lỗ hổng phạm vi ở [§5.3](#53-lỗ-hổng-phạm-vi-của-workermd) |

### 5.2 Các mục còn lại

| Mục | Nguồn | Trạng thái |
|---|---|---|
| **Backpressure tổng cho pool `api`** | `worker.md` mục 6, "Còn tồn đọng" | **Xong** — xem dưới |
| Phần tạo thumbnail của `bookmark/recordThumbnail` | `worker.md` Bước 2 ghi "để Bước 5", nhưng Bước 5 chỉ làm extraction | **Xong** ở [§1](#1-p1--thumbnail-2-route--1-lỗi-lifecycle) |
| `/index/api/getSnap` chọn thread theo query param `async` | `worker.md` "Chưa làm (cố ý)" | Cố ý bỏ, xem [§7](#7-ngoài-phạm-vi) |

**Backpressure tổng — đã làm.** `ApiWorkLimit` trước đó chỉ giới hạn **theo nhóm**, không có trần cho
toàn pool `api`, nên nhiều nhóm cùng bận vẫn có thể lấp kín mọi thread và làm hàng đợi dài vô hạn.
Nay `runApiWork` áp thêm một giới hạn **toàn cục** cho **mọi** request:

- Nhóm dành riêng `global`, config `api.limit.global.maxInflight` (mặc định 256) /
  `api.limit.global.maxQueueMs` (mặc định 30000). Vượt thì trả 503 `CODE_SERVER_BUSY` (902012) như
  giới hạn theo nhóm, giữ connection.
- **Giới hạn nhóm được kiểm tra trước**, để một nhóm nặng bị chặn trên hạn mức của chính nó và không
  ăn lẹm vào hạn mức chung; slot nhóm được trả lại nếu request bị trần chung từ chối.
- `maxQueueMs` thực tế là mức **chặt hơn** giữa nhóm và `global`.
- Mặc định đặt rộng hơn mọi giới hạn nhóm hiện có, nên không endpoint nào đổi hành vi.
- Test: `unit_api_work_dispatcher` có thêm 5 case (mặc định đủ rộng; request không thuộc nhóm nào
  vẫn bị trần chung chặn; nhóm được kiểm tra trước trần chung; slot chung được trả lại khi work ném
  exception; deadline hàng đợi chung áp cho request không nhóm).

Hai nhóm mới đi kèm đợt này: `storage.read` (32 / 15000) và `media.thumbnail` (8 / 15000).

### 5.3 Lỗ hổng phạm vi của `worker.md` — ĐÃ ĐÓNG

`worker.md` tuyên bố phạm vi là "chỉ các API có route `/media/*`", nhưng cả 6 bước chỉ đụng tới
`/media/mserver/*` và `/media/esc/*`. Họ `/media/api/*` (trust boundary `CHECK_SECRET` /
`CHECK_CLUSTER_AUTHOR_ASYNC`) chưa từng được phân loại — đó chính là [§2](#2-p1--họ-mediaapistorage-25-route)
và [§4](#4-p2--các-route-lẻ-trong-webapicpp-4-route), 29 route. Đợt này đã đóng nốt.

---

## 6. Đã kiểm tra và không cần sửa

Ghi lại để lần rà soát sau không mất công soi lại.

| Route / vị trí | Vì sao không cần |
|---|---|
| `/media/mserver/systemStatistic`, `/media/api/systemStatistic` | `makeSystemStatisticJson()` chỉ đọc snapshot đã cache của các monitor (`getCurrentUsage()`), không I/O |
| `/media/api/storage/dashboard/summary`, `/media/api/storage/pool/options` | Chỉ đọc cache/hằng số |
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

Toàn bộ 32 route trong tài liệu này đã xong. Những việc còn lại quanh chủ đề blocking nằm ở
[api-blocking-followup-plan.md](api-blocking-followup-plan.md):

| Mục | Nội dung |
|---|---|
| §1 | Gộp hai họ endpoint storage bằng hàm thuần, giữ nguyên guard ở mỗi họ |
| §2 | Gắn `storage.read` cho các read của `/media/mserver/storage/*` |
| §3 | `/index/api/getSnap` chọn thread theo query param (cố ý giữ nguyên) |
| §4 | Decode/transcode vẫn trên `WorkThreadPool` — cần đo trước |
| §5 | Siết bài đo healthcheck của bước 6 trong `check_worker_apis.py` |
| §6 | `api.limit.global.maxQueueMs` áp cả cho nhóm serial |

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

`tests/integration/sync_cluster/check_worker_apis.py` đã được bổ sung **bước 6** cho họ
`/media/api/*` vừa dispatch: 21 route đọc + 5 mutation (giữ nguyên status/code), và một vòng
`dashboard/detail` song song với `/media/mserver/healthcheck` giống cách bước 1/2 làm với discovery.
Họ này nằm sau `CHECK_SECRET()`, nên `run_cluster.sh` truyền secret của node qua biến môi trường
`API_SECRET` (đọc từ `config.ini` mà MediaServer ghi lại lúc khởi động, cần `tr -d '\r'` vì file
dump theo CRLF).

### Chạy lại script rà soát

Script nằm ở `/tmp` nên không theo repo. Cách dựng lại: duyệt từng `api_regist("/media/...")`, xoá
comment và string literal, xác định phạm vi **lambda đầu tiên** của mỗi
`dispatchApiWork`/`runApiWork`/`runApiCallbackWork`/`runSearch`/`lookupOwner`, rồi báo các mẫu
blocking nằm ngoài phạm vi đó. Hai cái bẫy đã gặp:

1. Lambda có `mutable` / `noexcept` / `-> T` giữa `)` và `{` → nhận diện phạm vi hỏng, báo nhầm.
2. Helper định nghĩa giữa hai `api_regist` bị tính vào handler phía trước.
