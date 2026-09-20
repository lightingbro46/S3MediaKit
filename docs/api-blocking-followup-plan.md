# Plan: Tồn Đọng Sau Đợt Dispatch API Blocking

Sáu mục còn lại sau khi [api-blocking-backlog.md](api-blocking-backlog.md) đã làm xong toàn bộ 32
route. Mô hình thread và cách dispatch: [threading-blocking-work.md](threading-blocking-work.md).

## Thứ tự đề xuất

| Bước | Mục | Ưu tiên | Trạng thái |
|---|---|---|---|
| 1 | [§1 Gộp hai họ endpoint storage](#1-gộp-hai-họ-endpoint-storage) | P1 | **Xong** — `server/WebApiStorage.h` + 27 hàm thuần, payload đã đồng nhất về bản `mserver` |
| 2 | [§2 Gắn `storage.read` cho `/media/mserver/storage/*`](#2-gắn-storageread-cho-các-read-của-mediamserverstorage) | P2 | **Xong** — 19 read mang nhóm `storage.read`, `playback/resolve` vẫn ngoài nhóm |
| 3 | [§5 Siết bài đo healthcheck của bước 6](#5-siết-bài-đo-healthcheck-của-bước-6) | P2 | **Xong** — đo bằng `pool/testConnection` trỏ vào `BLACKHOLE_IP` |
| 4 | [§6 Deadline hàng đợi chung với nhóm serial](#6-deadline-hàng-đợi-chung-áp-cả-cho-nhóm-serial) | P3 | **Xong** — `ApiWorkLimit::serial` được miễn `global.maxQueueMs` |
| 5 | [§7 Gộp hai họ ngoài storage](#7-gộp-hai-họ-ngoài-storage-systemstatistic) | P2 | **Xong** — `systemStatistic` / `systemStatisticHistory` |
| — | [§3 `/index/api/getSnap`](#3-indexapigetsnap-chọn-thread-theo-query-param) | — | Cố ý giữ nguyên, ghi lại cách xử lý nếu đổi ý |
| — | [§4 Decode/transcode](#4-decodetranscode-vẫn-trên-workthreadpool) | — | Cần đo trước, chưa đủ dữ liệu để lên việc |

---

## 1. Gộp hai họ endpoint storage

**Vấn đề.** `/media/api/storage/*` (25 route, sau `CHECK_SECRET`) và `/media/mserver/storage/*`
(sau JWT người dùng) có thân xử lý gần như y hệt nhau. Đợt vừa rồi phải sửa lặp 25 lần cùng một
logic dispatch. Mỗi endpoint storage thêm mới từ giờ đều phải nhớ sửa hai chỗ, và quên một chỗ thì
lại sinh ra đúng loại lỗi mà đợt rà soát vừa tìm ra: một họ chạy trên worker, họ kia vẫn trên poller.

**Giải pháp: tách thân handler thành hàm thuần, giữ nguyên guard ở mỗi họ.**

```cpp
// WebApiStorage.cpp — dùng chung cho cả hai họ.
// Chạy trên API worker, ném ApiResponseException cho lỗi nghiệp vụ.
void storagePoolList(const ArgsMap &allArgs, Json::Value &val);
void storagePoolCreate(const ArgsJson &allArgs, Json::Value &val);
```

Mỗi họ chỉ còn phần guard riêng và một lời gọi:

```cpp
api_regist("/media/api/storage/pool/list", [](API_ARGS_MAP_ASYNC) {
    CHECK_SECRET();                                    // trust boundary của họ này
    dispatchApiWork(sender, headerOut, val, invoker,
        [allArgs](Json::Value &val) { storagePoolList(allArgs, val); },
        nullptr, apiWorkLimit("storage.read"));
});
```

**Không** làm forwarder HTTP giữa hai họ: guard phải nằm ở từng họ, gộp lại là đổi trust boundary.
Phương án hàm thuần giữ nguyên ranh giới đó, chỉ bỏ phần trùng.

Hai họ hiện khác nhau ở vài chỗ nhỏ, phải xử lý trước khi gộp — soát lại từng cặp.

> **Đã làm khác kế hoạch ban đầu.** Kế hoạch viết là giữ nguyên khác biệt payload giữa hai họ.
> Khi thực hiện thì **đồng nhất payload**, lấy bản `/media/mserver/storage/*` làm chuẩn, và sửa
> console tier storage theo. Lý do: một hàm thuần trả hai dạng JSON tuỳ người gọi thì vẫn còn đúng
> chỗ để hai họ lệch nhau, tức là vẫn còn nguyên vấn đề mà §1 muốn bỏ. Tám chỗ lệch đã xử lý:
>
> | Route | Payload giữ lại (bản `mserver`) |
> |---|---|
> | `pool/list` | `poolSummaryToJson` (trước đó `/media/api/...` trả `poolDetailToJson`) |
> | `policy/list` | policy mặc định của hệ thống mang số camera chưa gán |
> | `policy/detail` | `policyDetailToJson` không kèm các trường summary |
> | `camera/effectivePolicy/list` | `effectivePolicyToJson` + filter `policy_id` |
> | `camera/timeline` | không có trường `policy_name` |
> | `dashboard/summary` | có `capacity_complete` / `capacity_unavailable_pool_count` |
> | `policy/assignCameras` | từ chối request mà toàn bộ `camera_ids` đều không tồn tại |
> | `pool/update` | validate watermark trước khi validate theo type |
>
> Hai chỗ **lấy bản `/media/api/...`** vì bản `mserver` là lỗi, không phải lựa chọn payload:
> `alert/list` guard nhầm trên `level` rồi `std::stoi("")` trên `acknowledged` (500 khi lọc theo
> `level`), và `policy/list` thiếu chặn `size` trong khoảng 1..100 mà mọi list phân trang khác đều có.
>
> Console phải sửa theo hai chỗ: bảng pool đọc được cả `status`/`used_percent` (từ `pool/list`) lẫn
> `health_status`/`usage_pct` (từ `dashboard/detail`), và form sửa pool nạp bản đầy đủ qua
> `pool/detail` thay vì dùng item của `pool/list`.

- Họ `/media/api/storage/*` là **tập con thật sự**: cả 27 route của nó đều có bản tương ứng bên
  `/media/mserver/storage/*`. Chiều ngược lại thì không — 14 route chỉ có ở `/media/mserver/...`:
  `alert/ack`, `camera/effectivePolicy` (số ít), `camera/summary/list`, `expiredSegment/approve`,
  `expiredSegment/extend`, `playback/resolve`, `policy/clone`, `protected/create`,
  `protected/delete`, `restoreJob/create`, `restoreJob/detail`, `tieringJob/cancel`,
  `tieringJob/detail`, `tieringJob/retry`. Gộp theo hướng hàm thuần **không** được thêm route mới
  cho họ `/media/api/*`.

  Kiểm tra lại quan hệ tập con này trước khi làm:

  ```bash
  comm -3 \
    <(grep -oE 'api_regist\("/media/api/storage/[a-zA-Z/]+' server/WebApi.cpp \
      | sed 's|.*/media/api/storage/||' | sort) \
    <(grep -oE 'api_regist\("/media/mserver/storage/[a-zA-Z/]+' server/WebApiStorage.cpp \
      | sed 's|.*/media/mserver/storage/||' | sort)
  ```

**Phạm vi.** `server/WebApiStorage.cpp` (thêm khai báo + định nghĩa hàm thuần), `server/WebApi.cpp`
(rút gọn 25 handler), có thể thêm `server/WebApiStorage.h` nếu chưa có chỗ khai báo dùng chung.

**Kiểm chứng.** `check_worker_apis.py` đã phủ cả hai họ (bước 3/4 cho `/media/mserver/...`, bước 6
cho `/media/api/...`) và so status/code. Chạy `run_cluster.sh ... workerapis` trước và sau, diff hai
log phải giống hệt nhau từng dòng.

---

## 2. Gắn `storage.read` cho các read của `/media/mserver/storage/*`

**Vấn đề.** Các read của họ `/media/mserver/storage/*` có `dispatchApiWork` nhưng không có giới hạn
nhóm, chỉ chịu trần chung `global`. Hạn mức `api.limit.storage.read.maxInflight` = 32 vì thế mới
bảo vệ được một nửa số endpoint storage: 32 request `/media/api/...` bị chặn, nhưng 100 request
`/media/mserver/...` cùng nội dung thì không.

**Giải pháp.** Thêm `apiWorkLimit("storage.read")` vào tham số `limit` của các read trong
`WebApiStorage.cpp` — một dòng mỗi route:

```cpp
    }, nullptr, apiWorkLimit("storage.read"));
```

**Trừ `playback/resolve`.** Endpoint này nằm trong đường người dùng bấm phát lại; chia hạn mức với
các endpoint liệt kê của trang quản trị thì một loạt request admin có thể đẩy nó về 503. Nó chỉ chịu
trần chung `global`. Nếu sau này muốn nó có backpressure riêng thì đặt nhóm riêng
`storage.playback`, không dùng chung `storage.read`.

**Làm sau §1**, nếu không lại sửa lặp ở hai chỗ.

**Kiểm chứng.** Bổ sung vào `check_worker_apis.py` một bài giống bài `discovery over the limit`: bắn
đồng thời nhiều hơn `maxInflight` request tới một read của cả hai họ, đếm số 503/902012, và xác nhận
`playback/resolve` **không** bị chặn khi `storage.read` đã đầy.

**Đã làm (bước 12 của `check_worker_apis.py`).** Một read storage trên node trống trả về trong
khoảng 1ms, nên bắn song song không bao giờ chồng lấn đủ để chạm hạn mức. Bài đo vì thế **chặn sạch
pool `api`** trước: `API_THREADS` request `pool/testConnection` trỏ vào `BLACKHOLE_IP`, mỗi request
giữ một worker khoảng 20s. Từ lúc đó không còn việc nào rời pool, nên mỗi request được nhận vào giữ
slot `storage.read` cho tới khi pool thoát — 40 request (20 `/media/api/...` + 20
`/media/mserver/...`) cho đúng 32 chạy và 8 nhận 503/902012, không phụ thuộc thời điểm đến. Hai
request `playback/resolve` bắn cùng lúc vẫn trả 404/905001.

`setup_nodes.py` vì bài này đặt thêm cho node test: `blocking.apiThreads=4` (mặc định
`max(2, nproc/2)` khác nhau theo máy) và `api.limit.global.maxQueueMs` /
`api.limit.storage.read.maxQueueMs` = 120000, để lý do duy nhất một request bị từ chối là trần
`maxInflight` chứ không phải deadline hàng đợi.

---

## 3. `/index/api/getSnap` chọn thread theo query param

**Vấn đề.** Client chọn thread qua query param `async`; nhánh `async=0` chờ ffmpeg trên một poller
của `WorkThreadPool`. Đây là vi phạm quy tắc "một hàm là blocking hoặc async, không phụ thuộc dữ
liệu request" ghi ở [threading-blocking-work.md](threading-blocking-work.md).

**Quyết định hiện tại: cố ý giữ nguyên.** Là API của repo gốc ZLMediaKit, đổi contract sẽ phá client
đang dùng.

**Giải pháp nếu đổi ý.** Cho `FFmpegSnap::makeSnap` gọi `makeSnapOnWorker` (pool
`BlockingExecutors::ffmpeg()`) thay vì `WorkThreadPool`. Một dòng, không đổi signature, không đổi
contract nhìn từ client: `async=0` vẫn chờ, chỉ là chờ trên thread ffmpeg thay vì trên một poller.

---

## 4. Decode/transcode vẫn trên `WorkThreadPool`

**Vấn đề.** Decode/transcode chạy trên `WorkThreadPool`, mà thread của pool này là `EventPoller`
đang chạy timer và ordered queue. Chưa có pool riêng và **chưa đo** ảnh hưởng.

**Giải pháp: đo trước, quyết sau.** Thêm pool mà không đo thì chỉ dời chỗ tốn CPU, không giảm tải.
Cần đo:

1. Thời gian một vòng decode/transcode trên thread đó (dùng `TranscodeStats` sẵn có).
2. Độ trễ của timer đang chạy cùng poller trong lúc decode (so `doDelayTask` thực tế với chu kỳ đặt).
3. Số thread `WorkThreadPool` so với số stream đang transcode.

Chỉ khi (1) lớn hơn đáng kể so với chu kỳ timer ở (2) mới thêm `BlockingExecutors::transcode()`.

---

## 5. Siết bài đo healthcheck của bước 6

**Vấn đề.** Bài `healthcheck stays fast during api storage reads` trong
`tests/integration/sync_cluster/check_worker_apis.py` chỉ lấy được ~5 mẫu, vì các read của họ
`/media/api/storage/*` trả về trong khoảng 0ms trên node trống. Nó mới là smoke check (route chạy,
status/code đúng), **chưa** chứng minh được tính không-blocking như bài discovery ở bước 1/2.

**Giải pháp, chọn một:**

- Tạo dữ liệu trước khi đo: vài trăm pool/policy/alert qua chính các endpoint mutation, rồi mới bắn
  `dashboard/detail` + `alert/list`. Tốn thời gian setup nhưng đo đúng cái muốn đo.
- Hoặc dùng một endpoint thật sự chậm: `mountpoint/available` trỏ vào một mount mạng không phản hồi,
  giống cách bước 1/2 dùng `BLACKHOLE_IP` (192.0.2.1, RFC 5737).

Phương án hai rẻ hơn và nhất quán với các bài đã có.

**Đã làm: phương án hai, nhưng endpoint chậm là `pool/testConnection` chứ không phải
`mountpoint/available`.** `mountpoint/available` chỉ đọc snapshot mount đã cache nên không chậm
được; muốn nó chậm thì phải có sẵn một mount mạng treo, không tạo được trong test. Ngược lại
`pool/testConnection` với pool MINIO trỏ vào `http://192.0.2.1:9000` đi qua AWS SDK với
`connectTimeoutMs` 5s và 3 lần retry, nên giữ một worker khoảng 20s — và nó **cố ý không thuộc nhóm
nào**, nên thứ duy nhất giữ healthcheck nhanh là việc đã được đẩy khỏi poller.

Bài đo mới gộp luôn phần kiểm chứng của §2 (xem bước 12 ở đó): lấy >100 mẫu `healthcheck` trong lúc
toàn bộ worker của pool `api` đang bị chặn, và khẳng định thêm rằng probe đã đủ chậm
(`min(duration) > 3s`) — thiếu khẳng định này thì một probe trả nhanh sẽ làm bài đo lại vô nghĩa
đúng như bản cũ.

---

## 6. Deadline hàng đợi chung áp cả cho nhóm serial

**Vấn đề.** `api.limit.global.maxQueueMs` mặc định 30000 áp cho **mọi** request, kể cả nhóm serial
`storage.mutation`. Một mutation xếp hàng quá 30s sẽ bị trả 503 `902012` thay vì chạy muộn.

**Đây là chủ ý**: sau 30s client gần như chắc chắn đã timeout, làm việc cũ không còn ai đọc kết quả.
Không đổi gì cho tới khi có triệu chứng thật.

**Giải pháp nếu vận hành thấy mutation bị rớt khi tải cao**, theo thứ tự thử:

1. Nới `api.limit.global.maxQueueMs` trong `config.ini` — không cần build lại.
2. Nếu vẫn không đủ: thêm cờ vào `ApiWorkLimit` để nhóm serial được miễn deadline hàng đợi chung
   (`runApiWork` bỏ qua `global.max_queue_ms` khi `limit.serial`), vì thứ tự và tính toàn vẹn của
   mutation quan trọng hơn việc bỏ bớt việc cũ.

Nếu chọn (2), nhớ bổ sung case vào `unit_api_work_dispatcher`: nhóm serial chờ quá deadline chung
vẫn chạy, nhóm thường vẫn bị bỏ.

**Đã làm: phương án (2).** `runApiWork` bỏ qua `global.max_queue_ms` khi `limit.serial`;
`maxQueueMs` của chính nhóm serial, nếu đặt, vẫn áp dụng. Case
`ApiWorkDispatcherTest.SerialGroupIsExemptFromGlobalQueueDeadline` chặn hết worker, xếp một request
nhóm serial và một request nhóm thường vào cùng thời điểm, rồi kiểm tra request serial trả 200 và
có chạy, request thường trả 503 và không chạy.

---

## 7. Gộp hai họ ngoài storage (`systemStatistic`)

**Rà soát.** Sau §1, soát lại toàn bộ 205 route `api_regist` của `server/` theo tên lá để tìm các
cặp còn lại của hai họ:

```bash
grep -rhoE 'api_regist\("/[a-zA-Z0-9/_]+' server/*.cpp | sed 's/api_regist("//' | sort \
  | awk -F/ '{fam=$2"/"$3; leaf=$0; sub("^/"fam"/","",leaf); print leaf"\t"fam}' \
  | sort | awk -F'\t' '{c[$1]=c[$1]" "$2} END {for (k in c) if (split(c[k],a," ")>1) print k, c[k]}'
```

Kết quả: ngoài 27 route storage đã gộp ở §1, hai họ chỉ còn trùng **hai** route —
`systemStatistic` và `systemStatisticHistory`. Thân của hai cặp này vốn đã giống hệt nhau từng
dòng, chỉ khác guard (`CHECK_AUTH_TOKEN` + `CHECK_USER_PERMISSION` so với
`CHECK_CLUSTER_AUTHOR_ASYNC`).

**Đã làm.** Thêm `server/WebApiMonitor.h` với đúng hai hàm thuần, theo cùng contract của
`WebApiStorage.h`:

- `systemStatistic(val)` — chỉ đọc snapshot đã cache của `GlobalMonitor` (OS, CPU, RAM, net, disk,
  reader), **không** blocking, nên cả hai họ vẫn trả lời ngay trên request poller.
- `systemStatisticHistory(allArgs, val)` — truy vấn DB thống kê, chạy trên API worker ở cả hai họ.

**Không phải cặp để gộp**, đã soát và loại:

- `/media/api/device/statisticsList` và `/media/mserver/device/statistics`: một bên liệt kê mọi
  thiết bị, một bên lấy một thiết bị theo `id`. Payload mỗi item đã dùng chung
  `makeDeviceStatisticJson`, không còn gì để tách.
- `/index/api/{getStatistic,getThreadsLoad,getWorkThreadsLoad}` và bản `/media/api/...` của chúng:
  thân đã là một lời gọi tới hàm dùng chung (`getStatisticJson`, `getThreadsLoad`), không còn logic
  trùng. Lưu ý ba handler `/media/api/...` này bắt `&sender` theo tham chiếu rồi truyền vào
  `API_ARGS_VALUE` bên trong callback của `CHECK_CLUSTER_AUTHOR_ASYNC` — callback đó **có thể** chạy
  sau khi handler đã trả về (nhánh hook bất đồng bộ). Đây là lỗi có sẵn, nằm ngoài phạm vi đợt này,
  cần một đợt riêng vì phải đổi chữ ký `getThreadsLoad`/`getStatisticJson`.

---

## Kiểm chứng chung

```bash
cmake --build build -j2                       # máy dev chỉ build được với -j2
cmake --build build-test -j2
ctest --test-dir build-test --output-on-failure -R unit.
bash tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_worker_apis workerapis
```

Cần giữ nguyên: route, `code`/`msg`/`data`, phân trang, lỗi auth/permission, mask credential, và mỗi
request gọi `invoker` đúng một lần.
