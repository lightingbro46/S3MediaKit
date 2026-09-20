# Mô Hình Thread Và Công Việc Blocking

Tài liệu này mô tả nơi mỗi loại công việc được chạy trong MediaServer: thread nào sở hữu
state gì, việc blocking (SQLite, filesystem, S3, SOAP/ONVIF, ffmpeg) chạy ở đâu, và handler
REST chuyển việc sang worker bằng cách nào.

Đây là phần dễ gây lỗi nhất của repo. Một `EventPoller` bị block không chỉ làm chậm request
đang chạy — nó dừng toàn bộ stream, timer và hàng đợi có thứ tự gắn với thread đó.

Tài liệu liên quan: [api-sync.md](api-sync.md), [storage-tiering-flow.md](storage-tiering-flow.md).

## 1. Các Loại Executor

Có hai họ executor, khác nhau về bản chất chứ không chỉ về kích thước.

| Executor | Loại | Dùng cho |
|---|---|---|
| `EventPollerPool` | `EventPoller` | Socket, session, `MediaSource`/muxer, timer của media |
| `WorkThreadPool` | `EventPoller` | Timer nền và hàng đợi có thứ tự: `SyncManager`, ghi ESC của từng camera, tier storage, health check của `ClusterManager` |
| `BlockingExecutors::api()` | `ThreadPool` | Phần blocking của handler REST |
| `BlockingExecutors::device()` | `ThreadPool` | SOAP/ONVIF và SDK của thiết bị đã biết (PTZ, speaker) |
| `BlockingExecutors::discovery()` | `ThreadPool` | Dò thiết bị: ONVIF connect/ffprobe cho từng địa chỉ |
| `BlockingExecutors::ffmpeg()` | `ThreadPool` | Chờ tiến trình ffmpeg/ffprobe (snapshot, thumbnail) |

Điểm mấu chốt: **mỗi thread của `WorkThreadPool` là một `EventPoller`**, không phải thread
rảnh. Nó đang chạy timer và các hàng đợi cần giữ thứ tự. Một lời gọi blocking dài ở đó
(ví dụ ONVIF connect tới IP không trả lời, ~20s) làm trễ tất cả những việc đó. Vì vậy các
pool trong `BlockingExecutors` (`manager/Common/BlockingExecutors.h`) là `toolkit::ThreadPool`
thuần: không poller, không timer, block bao lâu cũng được.

Các pool được tách riêng theo nhóm công việc để một việc chậm không ảnh hưởng nhóm khác:

- Một lần quét dải IP dài **không** được làm chậm lệnh PTZ tới camera đã biết → `discovery`
  tách khỏi `device`.
- Một thumbnail chậm **không** được ăn hết thread phục vụ API → `ffmpeg` tách khỏi `api`.

## 2. Quy Tắc

### 2.1 Không chọn thread dựa vào input

Handler **không** được đọc dữ liệu request để quyết định có dispatch sang worker hay không.

Ví dụ đã từng sai: `/media/mserver/device/discovery` rẽ nhánh theo
`start_with(address, "rtsp://")` — address RTSP thì gọi thẳng trên poller của request, address
IP mới đẩy sang `api worker`. Hai vấn đề:

1. Tiền đề sai. Nhánh "bất đồng bộ" thực ra post sang một poller của `WorkThreadPool` rồi
   `process->wait(true)` ở đó — tránh block poller của request bằng cách block một poller
   khác quan trọng hơn.
2. Nhánh đó đi vòng qua `apiWorkLimit`, nên không còn backpressure.

Việc phân loại cũng bị lặp: handler đoán trước hàm ở dưới sẽ đi nhánh nào, còn hàm đó lại tự
phân tích address lần nữa. Bỏ dòng `if` đi là code đọc kết quả khi callback chưa chạy và trả
về dữ liệu rỗng — không crash, không log.

Quy tắc: hàm cấp dưới tự quyết định và công bố contract của mình; handler luôn dispatch.

### 2.2 Một hàm, một contract threading

Không viết hàm mà hành vi threading thay đổi theo tham số. Quy ước đặt tên trong repo:

| Dạng | Contract |
|---|---|
| `probe()`, `snap()`, `snapWithFilter()`, `discovery_device()` | **Blocking.** Chạy trên chính thread gọi, `cb` chạy đúng một lần **trước khi return**. Phải gọi từ `BlockingExecutors::*`, không gọi trên `EventPoller` |
| `makeProbe()`, `makeSnapOnWorker()`, `makeSnapWithFilter()` | **Async.** Post bản blocking sang `executor` (`toolkit::TaskExecutor::Ptr`); `cb` chạy trên executor đó, sau khi hàm đã return |

Hàm async nhận `TaskExecutor::Ptr` chứ không phải `EventPoller::Ptr`: `EventPoller` kế thừa
`TaskExecutor` nên caller cũ truyền poller vẫn biên dịch được, còn caller mới truyền được pool
thuần.

Contract phải ghi trong header và **được khoá bằng test**, không chỉ bằng comment — xem
`SubnetScanTest.DiscoveryDeviceAnswersBeforeReturningForRtspUrl`.

### 2.3 Thread affinity

- `Session`, `MediaSource` và muxer thuộc về đúng một `EventPoller`; chỉ được chạm từ poller đó.
- Chuyển thread bằng `poller->async()`, không reach across thread để sửa state.
- Không capture raw `this` trong timer/worker callback; dùng `weak_ptr` + `lock()` (C++11,
  không có `weak_from_this()`). Singleton gọi `ClassName::Instance()` bên trong callback.
- Không giữ mutex của manager khi gọi SQLite, filesystem, HTTP hay AWS.
- Không dùng `EventPollerPool::getPoller()` bên trong worker để quay lại request thread; phải
  capture `EventPoller::Ptr` của request trước khi dispatch.

### 2.4 Giữ thứ tự khi cần

Khi nhiều việc blocking phải chạy tuần tự (cùng một thiết bị, cùng một cache dùng chung), dùng
`SerialTaskQueue` (`manager/Common/SerialTaskQueue.h`) đặt trên một pool, thay vì tự khoá:

- `CameraController` / `SpeakerController`: một queue cho mỗi thiết bị, trên
  `BlockingExecutors::device()` — lệnh tới cùng camera không chồng nhau, lệnh tới camera khác
  vẫn song song.
- `apiWorkSerial("storage.mutation")`: một queue trên pool `api` cho toàn bộ mutation storage,
  vì chúng vừa sửa DB vừa sửa cache pool/policy và đăng ký storage.

## 3. Handler REST: `ApiWorkDispatcher`

`server/ApiWorkDispatcher.h` là đường duy nhất để đưa phần blocking của handler ra khỏi poller
của request.

```text
Request EventPoller
  -> auth / permission / validation, copy giá trị request
  -> BlockingExecutors::api()
       SQLite / filesystem / S3 / SOAP / ffmpeg   (được phép throw)
  -> Request EventPoller
       completion (state thuộc poller) -> invoker
```

`api_regist` chạy mọi handler trên poller của HTTP session; hậu tố `_ASYNC` chỉ lùi thời điểm
gửi response, **không** chuyển việc sang thread khác. Handler phải chủ động dispatch.

### 3.1 Các helper

| Helper | Dùng khi |
|---|---|
| `dispatchApiWork(sender, headerOut, val, invoker, work, completion, limit)` | Dạng thường: chạy `work(val)` trên worker, `completion(val)` trên poller của request, rồi trả `200 + val` |
| `runApiWork(owner, session, work, done, limit)` | Cần tự điều khiển phần trả lời, hoặc còn bước bất đồng bộ nữa ở giữa |
| `runApiCallbackWork(owner, session, invoker, start, limit)` | Service trả lời qua callback, có thể muộn và từ thread khác (ví dụ hỏi peer). `reply(respond)` đảm bảo trả lời đúng một lần, trên poller của request |
| `apiRequestPoller(sender)` / `apiRequestSession(sender)` | Lấy poller và session (weak) của request trước khi dispatch |

Mẫu áp dụng:

```cpp
api_regist("/media/...", [](API_ARGS_MAP_ASYNC) {
    CHECK_AUTH_TOKEN();
    CHECK_ARGS_("id");                        // auth/validation: trên request poller
    std::string id = allArgs["id"];           // copy giá trị trước khi dispatch

    dispatchApiWork(sender, headerOut, val, invoker,
        [id](Json::Value &val) {              // api worker: blocking, được phép throw
            val["data"] = queryFromDb(id);
        },
        [id](Json::Value &val) {              // (tuỳ chọn) request poller: state thuộc poller
        });
});
```

Lambda phải capture **bằng giá trị**; không capture `allArgs`, `val`, `headerOut` bằng
reference, không capture raw `this`.

### 3.2 Ánh xạ lỗi

Giữ nguyên chuỗi `catch` của đường xử lý đồng bộ (`responseApiException()`):

| Exception | Kết quả |
|---|---|
| `ApiRetException` | status/code của exception, sau đó đóng connection |
| `ApiResponseException(code, msg)` | Như `RETURN_API_RESPONSE`: status của `code`, giữ các trường `val` đã gán, **không** đóng connection |
| `ApiBusyException` | 503 `CODE_SERVER_BUSY` (902012), giữ connection |
| `SqliteException` | `API::SqlFailed`, HTTP 500 |
| `std::exception` | `API::Exception`, HTTP 500 |

Nếu session đã đóng trước khi worker bắt đầu, phần việc blocking bị bỏ qua luôn.

### 3.3 Backpressure

`apiWorkLimit("<nhóm>")` giới hạn theo nhóm endpoint:

- `maxInflight`: số request của nhóm đang chờ hoặc đang chạy; vượt quá thì trả 503 `902012`
  ngay, **không** xếp hàng.
- `maxQueueMs`: request chờ trong hàng đợi lâu hơn mức này thì bị bỏ.

Ngoài giới hạn theo nhóm còn có **giới hạn tổng cho cả pool `api`** (`api.limit.global.*`,
nhóm `global` là tên dành riêng). Nó áp cho **mọi** request đi qua `runApiWork` /
`dispatchApiWork` / `runApiCallbackWork`, kể cả request không thuộc nhóm nào. Thứ tự:

1. Kiểm tra giới hạn của nhóm trước — nhóm nặng bị chặn trên hạn mức của chính nó, không ăn lẹm
   vào hạn mức chung.
2. Rồi mới kiểm tra `global`. Nếu vượt, slot của nhóm đã lấy được trả lại trước khi trả 503.
3. `maxQueueMs` thực tế là **mức chặt hơn** giữa nhóm và `global`.

Mặc định của `global` đặt rộng để không đổi hành vi của endpoint hiện có; xem
[§5](#5-cấu-hình).

### 3.4 Transaction đọc rồi ghi

Khi handler chạy trên worker, các request chạy song song thật. Mọi transaction **có thể ghi**
phải dùng `beginWriteTxn()` (`BEGIN IMMEDIATE`), kể cả khi nó đọc trước. Transaction deferred
đọc rồi ghi sẽ fail `database is locked` (`SQLITE_BUSY_SNAPSHOT`) khi một connection khác commit
xen vào — xem `SyncApplyTest.BatchWaitsForConcurrentWriter`.

## 4. Các Luồng Cụ Thể

### 4.1 Dò thiết bị và quét dải IP

`SubnetScan::discovery_device(option, address, cb)` (`manager/Control/SubnetScan.cpp`) là
**blocking với mọi dạng address**:

- Address là IPv4 → `OnvifControl::connect()` + các lời gọi SOAP. Tới ~20s nếu IP không trả lời.
- Address là `rtsp://...` → `FFmpegProbe::probe()`, chờ tiến trình ffprobe (mặc định 10s).

Hai đường vào, tách biệt rõ:

| Endpoint | Chạy ở đâu | Giới hạn |
|---|---|---|
| `/media/mserver/device/discovery` | `runApiWork` → `BlockingExecutors::api()` | `api.limit.device.discovery.*` |
| `/media/mserver/device/subnetScan` (đồng bộ) | `SubnetScan::scanRange()` trên `api worker`, probe trên `discovery` | `api.limit.device.discovery.*` + `searcher.max_parallel` |
| `/media/mserver/device/subnetScan/create` (nền) | `SubnetScan::startProbes()` trên `discovery` | `searcher.max_parallel` |

`startProbes()` là nơi duy nhất dispatch sang `BlockingExecutors::discovery()` và là nơi giữ
bound `searcher.max_parallel`; probe mặc định gọi thẳng bản blocking nên chỉ có một hop.
`makeScan()` chỉ mượn một poller của `WorkThreadPool` cho **timer đóng scan**, không chạy SOAP
ở đó.

### 4.2 Snapshot và thumbnail

`FFmpegSnap` tách "chạy process" khỏi "chạy ở đâu":

- `snap()` / `snapWithFilter()` — blocking, chạy trên thread gọi.
- `makeSnapOnWorker()` / `makeSnapWithFilter()` — post sang executor, mặc định
  `BlockingExecutors::ffmpeg()`. **Code mới dùng hai hàm này.**
- `makeSnap(bool async, ...)` — **giữ lại cho `/index/api/getSnap` của repo gốc**. Nó vẫn chờ
  ffmpeg trên một poller của `WorkThreadPool`, và tham số `async` do client gửi vẫn quyết định
  thread. Không dùng cho code mới; xem [§6](#6-hạn-chế-đã-biết).

`RecordedThumbnailTask` (dùng cho `bookmark/recordThumbnail`) dùng `makeSnapOnWorker`. Callback
`onSnap`/`complete()` có mutex riêng và vốn không chạy trên `_poller`, nên gọi từ pool nào cũng
an toàn.

### 4.3 Điều khiển thiết bị

`ptz_control*`, `imageMoveControl`, `relayOutputControl`, `onvifSetVideoConfigs`,
`onvifGetConfigProfiles`, `speaker/playAudioFile`, `validateCredential`:

```text
request poller -> owner poller của camera (lookup + ownership)
               -> SerialTaskQueue của thiết bị trên BlockingExecutors::device()  (SOAP/SDK)
               -> owner poller của camera (cập nhật state) -> invoker
```

Handler không cần `dispatchApiWork`: `CameraController`/`SpeakerController` đã tự đưa phần
blocking ra pool `device`.

### 4.4 Phân biệt với routing theo node

Một số handler (`bookmark/create|update|delete`, `motion`, `playback`) rẽ nhánh theo **node sở
hữu** dữ liệu: local thì làm tại chỗ, không phải local thì forward sang peer qua HTTP.

Đây **không** phải vi phạm [§2.1](#21-không-chọn-thread-dựa-vào-input). Đó là quyết định
routing, không phải quyết định "việc này có blocking không" — nhánh forward là HTTP bất đồng bộ
thật sự. Đừng "sửa" nó thành dispatch.

## 5. Cấu Hình

Pool (`manager/Common/BlockingExecutors.cpp`):

| Key | Mặc định | Ghi chú |
|---|---|---|
| `blocking.apiThreads` | `max(2, hw/2)` | Vẫn đọc `api.workerThreads` cũ nếu key mới bằng 0 |
| `blocking.deviceThreads` | 4 | |
| `blocking.discoveryThreads` | 8 | |
| `blocking.ffmpegThreads` | 4 | |

Backpressure (`server/ApiWorkDispatcher.cpp`), đọc lại ở mỗi request:

| Nhóm | `maxInflight` | `maxQueueMs` |
|---|---|---|
| `api.limit.global.*` | 256 | 30000 |
| `api.limit.sync.snapshot.*` | 2 | 30000 |
| `api.limit.sync.changes.*` | 8 | 30000 |
| `api.limit.device.discovery.*` | 4 | 30000 |
| `api.limit.storage.read.*` | 32 | 15000 |
| `api.limit.media.thumbnail.*` | 8 | 15000 |

Quét dải IP (`manager/Control/SubnetScan.cpp`):

| Key | Mặc định | Ghi chú |
|---|---|---|
| `searcher.max_parallel` | 8 | Số IP được probe đồng thời trong **một** lần quét |
| `searcher.timeout_sec` | 10 | |
| `searcher.delay_close_sec` | 300 | Thời gian giữ kết quả scan nền trước khi đóng |

## 6. Hạn Chế Đã Biết

- `/index/api/getSnap` vẫn để client chọn thread qua query param `async`, và nhánh `async=0`
  vẫn chờ ffmpeg trên một poller của `WorkThreadPool`. Đây là API của repo gốc, cố ý giữ
  nguyên contract. Nếu muốn xử lý mà không đổi contract: cho `makeSnap` gọi `makeSnapOnWorker`
  thay vì `WorkThreadPool`.
- `SubnetScan::scanRange()` (bản đồng bộ) giữ một thread của pool `api` trong suốt lần quét.
  Quét dải lớn toàn IP không route được sẽ giữ thread đó rất lâu.

## 7. Kiểm Thử

Unit test:

| Target | Nội dung |
|---|---|
| `unit_blocking_executors` | Pool là singleton và tách biệt nhau, thread không phải poller, tên thread, số thread song song đúng cấu hình |
| `unit_api_work_dispatcher` | Việc chạy ngoài poller, response chạy trên owner, ánh xạ lỗi, completion lỗi chỉ báo một lần, bỏ qua khi session đã đóng, nhóm serial chạy tuần tự đúng thứ tự, poller không bị chặn, giới hạn theo nhóm và giới hạn tổng `global` |
| `unit_subnet_scan` | Contract blocking của `discovery_device` với mọi dạng address, tiến độ, cancel, bound `max_parallel`, thứ tự IP |
| `unit_camera_controller` | Hàng đợi tuần tự theo thiết bị |

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_AWS_SDK=ON
cmake --build build-test --parallel
ctest --test-dir build-test --output-on-failure -R unit.
```

Kiểm thử tích hợp — `tests/integration/sync_cluster/check_worker_apis.py`, chạy qua:

```bash
bash tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_worker_apis workerapis
```

Script gọi discovery ONVIF **và** discovery RTSP tới một IP bị drop gói, rồi đo độ trễ của
`/media/mserver/healthcheck` trong suốt thời gian đó. Kết quả mong đợi: discovery mất
20s/10s trong khi healthcheck vẫn ở mức vài milli giây.

Bước 6 phủ họ `/media/api/*` (trust boundary `CHECK_SECRET`): 21 route đọc + 5 mutation, kiểm tra
status/code không đổi, cộng một vòng `storage/dashboard/detail` song song với healthcheck.
`run_cluster.sh` truyền secret của node qua `API_SECRET` — secret được MediaServer sinh ngẫu nhiên
lúc khởi động rồi ghi vào `config.ini` (file dump theo CRLF nên phải `tr -d '\r'`).

> `BLACKHOLE_IP` phải là `192.0.2.1` (RFC 5737 TEST-NET-1). Không dùng dải `10.x`: dưới WSL đó
> là NAT gateway của host và trả "connection refused" ngay, làm mọi check theo thời gian trở
> nên flaky.
