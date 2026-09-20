# Plan: Tồn Đọng Sau Đợt Dispatch API Blocking

Sáu mục còn lại sau khi [api-blocking-backlog.md](api-blocking-backlog.md) đã làm xong toàn bộ 32
route. Mô hình thread và cách dispatch: [threading-blocking-work.md](threading-blocking-work.md).

## Thứ tự đề xuất

| Bước | Mục | Ưu tiên | Vì sao ở vị trí này |
|---|---|---|---|
| 1 | [§1 Gộp hai họ endpoint storage](#1-gộp-hai-họ-endpoint-storage) | P1 | Chặn mục 2; mỗi endpoint storage mới hiện phải sửa hai chỗ |
| 2 | [§2 Gắn `storage.read` cho `/media/mserver/storage/*`](#2-gắn-storageread-cho-các-read-của-mediamserverstorage) | P2 | Sau §1 thì chỉ còn một chỗ để sửa |
| 3 | [§5 Siết bài đo healthcheck của bước 6](#5-siết-bài-đo-healthcheck-của-bước-6) | P2 | Độc lập, rẻ, làm bất cứ lúc nào |
| 4 | [§6 Deadline hàng đợi chung với nhóm serial](#6-deadline-hàng-đợi-chung-áp-cả-cho-nhóm-serial) | P3 | Chỉ làm khi vận hành thấy triệu chứng |
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

Hai họ hiện khác nhau ở vài chỗ nhỏ, phải xử lý trước khi gộp — soát lại từng cặp:

- `pool/list` của `/media/api/...` trả `poolDetailToJson`, của `/media/mserver/...` trả
  `poolSummaryToJson`. **Giữ nguyên khác biệt này**, cho hàm thuần nhận tham số chọn dạng JSON.
- `camera/effectivePolicy/list` của `/media/mserver/...` có thêm filter `policy_id` và dùng
  `effectivePolicyToJson`; bản `/media/api/...` tự dựng item. Thống nhất về hàm chung nhưng **không**
  đổi payload của họ nào.
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
