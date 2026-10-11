# Plan: đẩy sự kiện ONVIF lên Service API qua Kafka

Trạng thái: **K1–K4 xong** (2026-10-11): librdkafka vào build, `OnvifEventKafka`, `EventSpool`, `KafkaProducer` có unit test nhưng chưa gắn vào luồng sự kiện (K5). K0, K5–K8 chưa làm. Nối tiếp phần nhận sự kiện ONVIF (PullPoint, đã merge develop): sự kiện đã
được nhận, chuẩn hoá, dedup và phát qua `kBroadcastOnvifEvent`, hiện chỉ ghi log. Đợt này thêm **producer Kafka**
trong MediaServer để gửi các sự kiện đó lên Service API; Service API là consumer.

Quyết định đã chốt (2026-10-11), chi tiết ở mục 10:

| # | Quyết định |
|---|---|
| D1 | Cấu hình Kafka lấy từ Service API |
| D2 | Tên topic cập nhật sau: lấy từ cấu hình Service API, code không đặt mặc định |
| D3 | Không gửi sự kiện `Initialized` |
| D4 | Không quay về webhook HTTP khi Kafka lỗi/chưa cấu hình |
| D5 | Broker không nhận được → **ghi đệm xuống đĩa** và **gọi API cảnh báo** |
| D6 | Value là JSON |
| D7 | Bù cho D3: `Initialized` khác giá trị **đã gửi** gần nhất thì gửi thành một message `Changed` (`resync=true`), mục 4.1 |

## 1. Yêu cầu

### 1.1 Chức năng

| # | Yêu cầu |
|---|---|
| K-F1 | Mỗi sự kiện phát qua `kBroadcastOnvifEvent` (đã dedup trạng thái), **trừ `propertyOp=Initialized`**, được gửi thành **một message Kafka**. Ngoại lệ D7: `Initialized` mang giá trị khác giá trị đã gửi gần nhất của cùng trạng thái → gửi thành `Changed` (`resync=true`). |
| K-F2 | Message JSON có phiên bản schema, định danh sự kiện duy nhất + số thứ tự, `deviceId`, `mediaServerId`, `eventType` chuẩn hoá, trạng thái đã chuẩn hoá (`active`), giờ camera + giờ server, topic gốc, `Source`/`Data`. |
| K-F3 | Thứ tự sự kiện **trong một camera** được giữ tới consumer (key = `deviceId`). |
| K-F4 | Cấu hình broker (địa chỉ, topic, bảo mật, tài khoản) lấy từ Service API qua kênh cấu hình đã mã hoá. Đổi lúc chạy không cần khởi động lại. |
| K-F5 | Không gửi được lên broker (broker chết, sai tài khoản, topic không tồn tại, hàng đợi đầy) → sự kiện ghi vào **spool trên đĩa**, broker có lại thì gửi bù theo đúng thứ tự; vào/ra trạng thái này và spool sắp đầy/đầy → **cảnh báo qua `on_system_alert`**. |
| K-F6 | Spool sống qua khởi động lại MediaServer (tắt bình thường lẫn crash, crash mất tối đa ~1 s gần nhất). |
| K-F7 | Kafka tắt hoặc cấu hình thiếu (không brokers / không topic): không gửi, không spool, chỉ log như hiện tại. Không có đường webhook HTTP thay thế. |
| K-F8 | Tầng gửi là **chung** (`EventBus`), không gắn chết vào ONVIF: sau này motion phần mềm (`src/Motion`), sự kiện ISAPI/hãng dùng lại, chỉ thêm nguồn. |

### 1.2 Phi chức năng

| # | Yêu cầu | Hệ quả thiết kế |
|---|---|---|
| K-N1 | 300 camera / node, có lúc bão sự kiện | Gửi bất đồng bộ, gom batch (`linger.ms`), nén |
| K-N2 | Không bao giờ chặn poller camera | `produce` không cờ BLOCK; ghi đĩa ở `SerialTaskQueue` trên pool blocking, poller chỉ đẩy vào hàng đợi bộ nhớ có trần |
| K-N3 | Broker chết lâu không mất sự kiện | Spool đĩa (mục 6), trần dung lượng + tuổi |
| K-N4 | Không gửi trùng do retry, không đảo thứ tự khi bình thường | Producer idempotent (`enable.idempotence=true`, `acks=all`); khi đang có spool, sự kiện mới cũng xếp sau spool |
| K-N5 | Bộ nhớ và đĩa có trần | `queue.buffering.max.kbytes`; `kafka.spoolMaxMB`, `kafka.spoolMinFreeMB` |
| K-N6 | Không lộ mật khẩu SASL | Không ghi log, không dump vào `config.ini` (mục 5.3) |
| K-N7 | Quan sát được | Dòng thống kê định kỳ + cảnh báo `on_system_alert` (mục 7) |

### 1.3 Ngoài phạm vi

- Consumer phía Service API (lưu DB, rule engine, đẩy FE) — plan này chỉ chốt hợp đồng message (mục 4) và mã cảnh
  báo mới (mục 7).
- Chuyển các webhook hiện có (`on_device_changed`, `media-device-info`, …) sang Kafka.
- MediaServer làm consumer (nhận lệnh qua Kafka).

## 2. Chọn thư viện: librdkafka (C API)

| | librdkafka (C) | cppkafka / modern-cpp-kafka | Tự viết giao thức |
|---|---|---|---|
| Độ chín | Chuẩn de facto, Confluent duy trì | Bọc librdkafka | Không |
| C++11 | C API, dùng thẳng được | cppkafka C++11 nhưng ít duy trì; modern-cpp-kafka cần C++17 | — |
| Producer idempotent, nén, SASL/TLS | Có | Có (qua librdkafka) | Rất tốn công |
| Mock cluster để unit test | Có (`test.mock.num.brokers`, `rd_kafka_mock_*`) | Qua librdkafka | — |

Chọn **librdkafka, dùng C API** qua lớp bọc mỏng (`KafkaProducer`, RAII cho `rd_kafka_t`/`rd_kafka_conf_t`).

Build theo khuôn FFmpeg/AWS SDK:

- Submodule `3rdpart/librdkafka`, ghim tag **v2.15.1** (2026-09-08), shallow.
- `cmake/BundledRdKafka.cmake`: build tĩnh một lần vào `release/linux/3rdpart/librdkafka` (dùng chung mọi build dir,
  build id = commit + tham số + compiler), `RDKAFKA_BUILD_STATIC=ON`, tắt examples/tests, `WITH_SSL=ON` (OpenSSL hệ
  thống, đã dùng bởi S3ToolKit), `WITH_ZSTD=ON` (cần `libzstd-dev`), zlib, lz4 bản kèm trong librdkafka,
  `WITH_CURL=OFF`, `WITH_PLUGINS=OFF`. `WITH_SASL=ON` vì bản CMake chỉ bật SCRAM khi có cờ này; Cyrus/GSSAPI bị chặn
  bằng `PKG_CONFIG_EXECUTABLE=/bin/false` (librdkafka chỉ dò libsasl2 khi tìm thấy pkg-config) để máy dev có
  `libsasl2-dev` và image Docker ra cùng một thư viện. Kết quả `builtin.features`: ssl, sasl_plain, sasl_scram,
  sasl_oauthbearer, zstd, lz4, gzip, snappy; `WITH_SASL_CYRUS 0`.
- Tuỳ chọn CMake `ENABLE_KAFKA` (mặc định `ON`), macro `ENABLE_KAFKA`. Bản `OFF`: `EventBus` là no-op, chỗ gọi vẫn
  biên dịch, không `#ifdef` rải trong code ONVIF.
- `-DENABLE_BUNDLED_RDKAFKA=OFF` dùng librdkafka hệ thống (pkg-config), như FFmpeg.

## 3. Kiến trúc

```
OnvifEventSubscriber (poller camera)
   │ NOTICE_EMIT kBroadcastOnvifEvent  (đã có)
   ▼
OnvifEventHub::onEvent ── log (đã có)
   │ Initialized: bỏ, trừ khi khác giá trị đã gửi (D7, mục 4.1); giới hạn / camera / phút
   ▼
OnvifEventKafka::toMessage ──► EventBus::publish(record)            (poller camera, không chặn)
                                   │
                    mode == LIVE ? │ ─────────────── không ─────────────┐
                                   ▼                                    ▼
                    KafkaProducer::produce (rd_kafka_producev)    EventSpool::append (hàng đợi bộ nhớ)
                                   │                                    │ SerialTaskQueue / BlockingExecutors::api()
                    librdkafka threads ──► broker                       ▼
                                   │ DR lỗi / purge                spool/seg-*.log  (CRC, fdatasync ≤ 1 s)
                                   └──────────► EventSpool::append       │
                                                                         │ broker có lại: DRAIN
                                   KafkaProducer::produce ◄──────────────┘ (checkpoint khi DR ok)
timer 100 ms trên một poller: rd_kafka_poll(rk, 0) → DR, trạng thái broker, chuyển mode, cảnh báo
```

| File mới | Namespace | Nội dung |
|---|---|---|
| `manager/Common/KafkaProducer.h/.cpp` | `managerkit` | Bọc `rd_kafka_t`: tạo từ `KafkaSettings`, `produce()` không chặn (opaque = id record), DR callback, đếm broker `UP`, `purgeQueued()`, `flush(timeout)`. Chỉ file này `#include <librdkafka/rdkafka.h>`. |
| `manager/Common/EventSpool.h/.cpp` | `managerkit` | Spool trên đĩa: segment append-only, CRC, checkpoint, xoay vòng, trần dung lượng/tuổi, khôi phục khi khởi động, đọc tuần tự để gửi bù. Không phụ thuộc librdkafka → unit test thuần. |
| `manager/Common/EventBus.h/.cpp` | `managerkit` | Singleton. Giữ producer + spool, máy trạng thái LIVE/SPOOL/DRAIN, áp cấu hình mới, timer poll, thống kê, phát cảnh báo. Bản `ENABLE_KAFKA=OFF` là no-op. |
| `manager/Control/OnvifEventKafka.h/.cpp` | `managerkit` | Hàm thuần `OnvifEventMessage` + ngữ cảnh → record (key, header, JSON), chuẩn hoá `active`. |

Chỉnh sửa: `OnvifEventHub::onEvent` gọi `EventBus::publish` (trước bước giới hạn log, để giới hạn log không làm mất sự
kiện); `server/Manager.cpp` đọc `media_server.eventBus`; `server/WebHook.cpp` thêm listener cảnh báo; `server/main.cpp`
gọi `EventBus::shutdown()` khi thoát; `src/Common/config.h` thêm broadcast cảnh báo; `conf/config.ini` + `onceToken`
cho khoá `kafka.*`.

Thêm file vào `manager/` cần configure lại build dir (`file(GLOB)`).

### 3.1 Luồng thread

| Việc | Thread | Ghi chú |
|---|---|---|
| Dựng JSON, `rd_kafka_producev` hoặc đẩy vào hàng đợi spool | Poller của camera | Không cờ `RD_KAFKA_MSG_F_BLOCK`; `QUEUE_FULL` → chuyển SPOOL, record đó vào spool. Hàng đợi bộ nhớ của spool có trần `kafka.spoolMemQueue`; đầy → bỏ + đếm (không chặn). |
| Batch, nén, gửi, retry | Thread nội bộ librdkafka (1 main + 1/broker) | ~3–5 thread / node, cố định |
| DR, trạng thái broker, chuyển mode | Timer 100 ms trên **một** poller (`EventPollerPool`), `rd_kafka_poll(rk, 0)` | Không chặn; DR lỗi chỉ đẩy record vào hàng đợi spool |
| Ghi spool, đọc spool để gửi bù, checkpoint, xoá segment | Một `SerialTaskQueue` trên `BlockingExecutors::api()` | Mỗi task gom cả lô (ghi hết hàng đợi bộ nhớ một lần); thứ tự ghi = thứ tự vào hàng đợi |
| Tạo/huỷ producer khi đổi cấu hình | `BlockingExecutors::api()` | `rd_kafka_flush`/`rd_kafka_destroy` chặn tới `kafka.flushTimeoutMs` |
| Thoát chương trình | Luồng `main`, sau `unInstallWebHook()` | Mục 6.6 |

`EventBus` đổi producer bằng cách swap `shared_ptr` dưới mutex ngắn; `publish()` lấy bản sao rồi gọi ngoài mutex.
`mode` là atomic. Không bắt `this` trong callback (singleton gọi `Instance()`; `EventSpool` dùng `weak_ptr`).

## 4. Hợp đồng message (team Service API duyệt)

- **Topic**: **cập nhật sau (D2)** — lấy từ `media_server.eventBus.deviceEventTopic`; code không có tên mặc định. Topic
  do vận hành tạo; MediaServer **không** tự tạo (`allow.auto.create.topics=false`).
- **Key**: `deviceId` → cùng partition → giữ thứ tự theo camera. Camera chuyển node (failover) vẫn cùng key.
- **Header**: `schema=device-event`, `v=1`, `source=onvif`, `eventType`, `mediaServerId`, `replayed=1` khi gửi bù từ
  spool.
- **Value**: JSON UTF-8 (D6):

```json
{
  "schemaVersion": 1,
  "eventId": "ms-01:5f1c2a7e:4666",
  "seq": 4666,
  "source": "onvif",
  "mediaServerId": "ms-01",
  "projectId": "p-123",
  "deviceId": "9f2c...",
  "eventType": "MOTION",
  "kind": "state",
  "active": true,
  "propertyOp": "Changed",
  "resync": false,
  "topic": "tns1:RuleEngine/CellMotionDetector/Motion",
  "cameraTime": "2026-10-11T03:12:05Z",
  "serverTimeMs": 1791688325180,
  "lagMs": 180,
  "sourceItems": { "VideoSourceConfigurationToken": "VSC_1", "Rule": "MyMotionDetectorRule" },
  "data": { "IsMotion": "true" }
}
```

| Field | Ý nghĩa |
|---|---|
| `eventId` | `mediaServerId:bootId:seq` — `bootId` ngẫu nhiên mỗi lần chạy. Duy nhất toàn cụm; consumer bỏ trùng theo field này (at-least-once). |
| `seq` | Tăng dần trong một `bootId`, gán lúc nhận sự kiện (trước khi gửi hay spool). |
| `kind` | `state` (MOTION, TAMPER, VIDEO_LOSS, DIGITAL_INPUT, RELAY, INTRUSION) hoặc `pulse` (LINE_CROSSING) |
| `active` | Chỉ với `state`: trường trạng thái chuẩn hoá về bool (`true`/`1`/`active`/`on`/`closed`… → `true`; mapping chốt ở K2 theo dữ liệu camera thật). Giá trị gốc vẫn trong `data`. |
| `propertyOp` | `Changed`, `Deleted`, `""` với `pulse`. **Không bao giờ `Initialized`** (D3). |
| `resync` | `true`: không phải camera báo thay đổi, mà là trạng thái camera báo lại sau khi đăng ký lại khác với giá trị đã gửi trước đó (mục 4.1). `serverTimeMs` là lúc phát hiện, thay đổi thật xảy ra trong khoảng mất kết nối. |
| `cameraTime` | `Message@UtcTime` của camera; rỗng khi camera không gửi |
| `serverTimeMs` | Giờ server lúc nhận (epoch ms) — giờ của sự kiện, không phải giờ gửi (quan trọng với message gửi bù) |
| `lagMs` | Server trừ giờ camera đã chỉnh lệch giờ |

Đổi schema không tương thích → tăng `schemaVersion` và header `v`; thêm field là tương thích.

Bảo đảm giao nhận:

- **At-least-once**. Trùng xảy ra khi: process khởi động lại giữa lúc gửi; gửi bù từ spool bị ngắt giữa chừng (record
  đã tới broker nhưng checkpoint chưa ghi). Consumer bỏ trùng theo `eventId`.
- **Thứ tự theo camera**: giữ khi bình thường và khi gửi bù (sự kiện mới xếp sau spool). Ngoại lệ ở đúng lúc chuyển
  LIVE → SPOOL: vài record đang nằm trong librdkafka bị trả lỗi/purge sẽ vào spool **sau** vài record mới hơn.
  Consumer áp trạng thái theo `serverTimeMs` (bỏ message `state` cũ hơn message đã áp cho cùng `deviceId` +
  `eventType` + `sourceItems`).
- Trạng thái đổi trong lúc mất kết nối camera: bù bằng mục 4.1.

### 4.1 Đồng bộ lại trạng thái sau khi đăng ký lại (D7)

D3 bỏ `Initialized`, nhưng nếu camera đổi trạng thái trong lúc mất kết nối (vd. motion kết thúc khi đang mất mạng)
thì consumer giữ mãi `active=true`. `OnvifEventHub` vì vậy nhớ **giá trị đã gửi** gần nhất của từng trạng thái:

- Bảng `_sent_states`: `deviceId` → (`stateKey` → giá trị trạng thái), dưới `_mtx` của hub. `stateKey` =
  `eventType` + `Source` như dedup của subscriber. Trần `onvif.eventMaxStateKeys` (256) khoá / camera.
- Cập nhật khi `EventBus::publish` **nhận** record (gửi thẳng hoặc vào spool); record bị bỏ (giới hạn tốc độ, hàng đợi
  đầy, Kafka tắt) không cập nhật. `Deleted` xoá khoá sau khi gửi. Sự kiện `pulse` không vào bảng.
- `Initialized` tới:

| Bảng có giá trị cho `stateKey`? | So với `Initialized` | Hành vi |
|---|---|---|
| Có | Bằng | Bỏ (như D3) |
| Có | Khác | Gửi một message `propertyOp=Changed`, `resync=true`, giá trị mới; cập nhật bảng |
| Không | — | Bỏ, **không** ghi bảng (chưa gửi gì nên không biết consumer đang giữ gì) |

- Bảng chỉ nằm trong bộ nhớ của process. Xoá khoá của camera khi camera bị xoá khỏi node. Camera bị tắt sự kiện
  (`enableOnvifEvent=false` / `onvif.eventEnable=0`) giữ bảng để lần bật lại so được.
- Giới hạn còn lại (ghi trong hợp đồng cho Service API): sau khi **MediaServer khởi động lại** hoặc **camera chuyển
  sang node khác** (failover), node mới chưa có giá trị đã gửi → trạng thái đổi trong khoảng đó không được bù. Consumer
  xoá trạng thái `active` của các camera khi nhận `on_server_started` của node / khi camera offline
  (`on_device_changed`), hoặc tự hết hạn `active` sau một thời gian do Service API chọn.

## 5. Cấu hình

### 5.1 Cấp từ Service API (D1)

Config production sinh từ default trong code + Service API, không dùng `conf/config.ini`. Service API trả thêm khối
trong `media_server` của `configuration-2nd-gen` (response đã mã hoá AES-GCM bằng `apiSecret`):

```json
"media_server": {
  "...": "...",
  "eventBus": {
    "enable": true,
    "brokers": "10.0.0.11:9093,10.0.0.12:9093",
    "deviceEventTopic": "<chốt sau>",
    "securityProtocol": "sasl_ssl",
    "saslMechanism": "SCRAM-SHA-512",
    "saslUsername": "ms-01",
    "saslPassword": "…",
    "caPem": "-----BEGIN CERTIFICATE-----…"
  }
}
```

| Field | Bắt buộc | Ghi chú |
|---|---|---|
| `enable` | có | `false` → huỷ producer; spool giữ nguyên trên đĩa (không gửi, không ghi thêm) |
| `brokers` | khi `enable` | `host:port,…` |
| `deviceEventTopic` | khi `enable` | rỗng → coi như cấu hình thiếu (K-F7), log Warn một lần |
| `securityProtocol` | không | `plaintext` (mặc định), `ssl`, `sasl_plaintext`, `sasl_ssl` |
| `saslMechanism` | khi SASL | `PLAIN`, `SCRAM-SHA-256`, `SCRAM-SHA-512` |
| `saslUsername` / `saslPassword` | khi SASL | |
| `caPem` | không | CA cho TLS; rỗng = kho CA hệ thống |

- Đọc trong `loadServerConfigFromJson` (`server/Manager.cpp`), so với bản đang chạy; khác → `EventBus::apply()` (trên
  `api()`). Khối vắng mặt → giữ cấu hình đang chạy.
- Mỗi node một tài khoản SASL (ACL chỉ `WRITE` topic sự kiện) để thu hồi được từng node — đề xuất cho vận hành.
- Trước khi nhận cấu hình lần đầu sau khởi động (Service API chưa trả lời), sự kiện **đi vào spool** nếu lần chạy
  trước đã có cấu hình hợp lệ (lưu cờ không bí mật `kafka.lastEnabled=1`), để không mất sự kiện trong lúc khởi động;
  ngược lại bỏ qua.

### 5.2 Khoá cục bộ `kafka.*` (`onceToken`, `GET_CONFIG`)

Chỉ tham số vận hành của MediaServer; thông tin kết nối không nằm ở đây (trừ ghi đè dev).

| Khoá | Mặc định | Ý nghĩa |
|---|---|---|
| `kafka.devOverride` | `0` | `1`: dùng `kafka.brokers`/`kafka.deviceEventTopic`/`kafka.securityProtocol` cục bộ thay khối Service API (chỉ dev/test, plaintext) |
| `kafka.brokers`, `kafka.deviceEventTopic`, `kafka.securityProtocol` | `""`, `""`, `plaintext` | Dùng khi `devOverride=1` |
| `kafka.compression` | `lz4` | |
| `kafka.lingerMs` | `50` | Gom batch |
| `kafka.queueMaxKBytes` | `16384` | Trần hàng đợi bộ nhớ librdkafka |
| `kafka.messageTimeoutMs` | `60000` | Record kẹt trong librdkafka quá lâu này → DR lỗi → vào spool |
| `kafka.spoolAfterSec` | `15` | Không broker nào `UP` quá lâu này → chuyển SPOOL (purge hàng đợi librdkafka vào spool) |
| `kafka.spoolDir` | `./eventbus_spool` | Cạnh `./db`, không đặt trên đĩa ghi hình |
| `kafka.spoolMaxMB` | `1024` | Trần dung lượng; vượt → xoá segment cũ nhất |
| `kafka.spoolMaxAgeHours` | `72` | Segment cũ hơn → xoá (consumer không còn cần sự kiện quá cũ) |
| `kafka.spoolMinFreeMB` | `512` | Phân vùng còn ít hơn → ngừng ghi spool, bỏ sự kiện mới |
| `kafka.spoolSegmentMB` | `16` | Kích thước một segment |
| `kafka.spoolFsyncMs` | `1000` | Chu kỳ `fdatasync` (crash mất tối đa khoảng này) |
| `kafka.spoolMemQueue` | `20000` | Trần record chờ ghi đĩa trong bộ nhớ |
| `kafka.spoolReplayInFlight` | `1000` | Số record gửi bù đang bay |
| `kafka.spoolReplayRate` | `2000` | Record/s tối đa khi gửi bù (không dội consumer) |
| `kafka.spoolHighPercent` | `80` | Cảnh báo spool sắp đầy |
| `kafka.flushTimeoutMs` | `3000` | Khi thoát / đổi cấu hình |
| `kafka.maxEventsPerCameraPerMin` | `600` | Trần sự kiện / camera / phút; `0` không giới hạn |
| `kafka.alertRepeatSec` | `1800` | Nhắc lại cảnh báo khi vẫn còn đang spool |
| `kafka.statsIntervalSec` | `60` | Dòng thống kê; `0` tắt |

Cố định trong code: `enable.idempotence=true`, `acks=all`, `allow.auto.create.topics=false`,
`client.id=<mediaServerId>`.

### 5.3 Không lộ bí mật

- `loadServerConfigFromJson` ghi thay đổi vào `mINI` rồi `dumpFile`. **Không** đưa thông tin kết nối/`saslPassword`/
  `caPem` vào `mINI`: `EventBus` giữ trong bộ nhớ, `caPem` nạp bằng `ssl.ca.pem` (không ghi file). Chỉ cờ
  `kafka.lastEnabled` được ghi.
- Spool chỉ chứa record sự kiện (không có bí mật). Thư mục spool tạo với quyền `0700`.
- Log cấu hình chỉ in brokers, topic, protocol, username. `log_cb` của librdkafka đi qua `WarnL`/`DebugL`, không bật
  `debug=security`.

## 6. Spool trên đĩa (D5)

### 6.1 Máy trạng thái

```
           không broker UP > spoolAfterSec / QUEUE_FULL / DR lỗi / lỗi không phục hồi
   LIVE ─────────────────────────────────────────────────────────────────► SPOOL
    ▲                                                                       │ producer khoẻ + có broker UP
    │ đọc tới cuối spool, không còn record đang bay                         ▼
    └─────────────────────────────────────────────────────────────────── DRAIN
                                                    DR lỗi khi gửi bù ──► SPOOL
```

| Mode | Sự kiện mới | Spool |
|---|---|---|
| LIVE | `produce` thẳng vào librdkafka | rỗng |
| SPOOL | Ghi vào cuối spool | Ghi; không gửi |
| DRAIN | Ghi vào cuối spool (giữ thứ tự) | Đọc từ checkpoint, `produce` với `replayed=1`, tối đa `spoolReplayInFlight` đang bay, `spoolReplayRate` record/s |

- Vào SPOOL: gọi `rd_kafka_purge(RD_KAFKA_PURGE_F_QUEUE)` → record chưa gửi trả về qua DR (`__PURGE_QUEUE`) → ghi spool.
  Record đang bay thì chờ DR: thành công là xong, lỗi thì ghi spool.
- DRAIN → LIVE: trong task của `SerialTaskQueue`, khi đọc hết spool, hàng đợi ghi rỗng và không còn record gửi bù
  đang bay → đặt `mode=LIVE` dưới mutex của spool (`publish()` kiểm `mode` dưới cùng mutex khi không phải LIVE) để
  không có record nào lọt giữa hai đường.
- Lỗi không phục hồi (sai SASL, `TOPIC_AUTHORIZATION_FAILED`, topic không tồn tại, fatal idempotent): ở SPOOL, thử
  tạo lại producer theo backoff 5 s → 5 phút; đổi cấu hình từ Service API thì thử ngay.
- `enable=false` hoặc cấu hình thiếu: dừng producer, không ghi thêm vào spool, spool cũ giữ lại tới khi bật lại hoặc
  hết `spoolMaxAgeHours`.

### 6.2 Định dạng file

```
eventbus_spool/
  seg-00000000000000000001.log      append-only
  seg-00000000000000000002.log
  checkpoint                        "<segment> <offset>\n" — record tiếp theo cần gửi; ghi tmp + rename
```

Record: `magic u16 | flags u16 | len u32 | crc32 u32 | payload[len]`, payload = `key`, header, value (độ dài-tiền tố).
**Không lưu tên topic**: gửi bù dùng topic hiện hành (D2 có thể đổi tên trong lúc spool).

- Xoay segment khi vượt `spoolSegmentMB`; segment đã gửi hết (checkpoint qua khỏi nó) bị xoá.
- `fdatasync` theo `spoolFsyncMs` và khi đóng segment; checkpoint ghi mỗi giây khi có tiến triển (không fsync mỗi
  record) → gửi lại tối đa ~1 s record sau crash, consumer bỏ trùng theo `eventId`.
- Khởi động: quét segment, record cuối hỏng/cụt (CRC sai, thiếu byte) → cắt tại record tốt cuối cùng, log Warn + đếm
  `spool_corrupt`. Spool không rỗng → bắt đầu ở mode SPOOL.

### 6.3 Trần dung lượng

| Điều kiện | Hành vi |
|---|---|
| Spool ≥ `spoolHighPercent` % `spoolMaxMB` | Cảnh báo `EventBusSpoolHigh` |
| Spool ≥ `spoolMaxMB` hoặc segment cũ hơn `spoolMaxAgeHours` | Xoá segment cũ nhất (mất sự kiện cũ nhất), đếm `spool_dropped`, cảnh báo `EventBusSpoolFull` |
| Phân vùng còn < `spoolMinFreeMB` | Ngừng ghi, bỏ sự kiện mới, đếm, cảnh báo `EventBusSpoolFull` (`reason=disk`) |
| Hàng đợi bộ nhớ chờ ghi đầy (`spoolMemQueue`, đĩa chậm) | Bỏ sự kiện mới, đếm `spool_mem_dropped` |

Ước lượng: ~500 B/record → 1 GB ≈ 2 triệu sự kiện; 300 camera bão liên tục 1 sự kiện/s/camera ≈ 2 giờ, bình thường
nhiều ngày.

### 6.4 Gửi bù

- Đọc theo lô 256 record từ checkpoint, `produce` với opaque = vị trí record; checkpoint chỉ tiến khi các record liên
  tiếp từ đầu đã DR thành công.
- Giới hạn `spoolReplayRate` để không dội consumer sau khi broker chết lâu; dòng thống kê in tiến độ (`spool_pending`,
  ước lượng thời gian còn lại).

### 6.5 Thứ tự với failover

Camera chuyển sang node khác khi node cũ đang có spool: node mới gửi sự kiện LIVE trước, node cũ gửi bù sau →
consumer nhận sự kiện cũ muộn. Quy tắc áp theo `serverTimeMs` ở mục 4 xử lý được; header `replayed=1` để consumer
không bắn thông báo thời gian thực cho sự kiện gửi bù quá cũ (ngưỡng do Service API chọn).

### 6.6 Tắt chương trình

`EventBus::shutdown()` sau `unInstallWebHook()`:

1. Ngừng nhận `publish` (đẩy thẳng vào hàng đợi spool).
2. `rd_kafka_flush(flushTimeoutMs)`; còn sót → `purge` → DR → spool.
3. Ghi hết hàng đợi bộ nhớ, `fdatasync`, ghi checkpoint.

Tổng thời gian ≤ `flushTimeoutMs` + thời gian ghi đĩa; log một dòng số record đã gửi/đã spool.

## 7. Cảnh báo (D5: gọi API cảnh báo)

Dùng đường cảnh báo hệ thống có sẵn `hook.on_system_alert` (`/api/event-rule/system-event`): broadcast mới
`kBroadcastEventBusAlert` (`src/Common/config.h`, tham số `const T &` để `NOTICE_EMIT` tới được listener), listener
trong `server/WebHook.cpp` dựng body như các cảnh báo khác.

| `eventCode` | Khi nào | Field thêm |
|---|---|---|
| `EventBusUnavailable` | Vào SPOOL; nhắc lại mỗi `alertRepeatSec` khi vẫn SPOOL | `reason` (`broker_down`, `auth`, `topic`, `queue_full`, `fatal`), `lastError`, `pendingEvents`, `spoolBytes` |
| `EventBusSpoolHigh` | Spool vượt `spoolHighPercent` | `currentValue` = MB, `threshold` = MB |
| `EventBusSpoolFull` | Bắt đầu mất sự kiện (xoá segment cũ / hết đĩa / hàng đợi bộ nhớ đầy) | `reason` (`max_size`, `max_age`, `disk`, `memory`), `droppedEvents` |
| `EventBusRecovered` | DRAIN xong, về LIVE | `replayedEvents`, `durationSec` |

Body chung: `eventCode`, `eventTime`, `currentValue`, `threshold` như các cảnh báo hiện có. Mỗi lần chuyển trạng thái
gửi một lần (chống lặp); cần team Service API nhận các `eventCode` mới.

Dòng thống kê (cùng chu kỳ với `OnvifEvent stats`):

```
EventBus kafka: mode=LIVE brokers_up=3/3 queued=12 produced=1243 delivered=1240 spooled=0 replayed=0 spool_pending=0 spool_mb=0 dropped{rate=0 spool=0 mem=0} skipped_initialized=287 resynced=3
```

## 8. Ước lượng tài nguyên (300 camera)

- Yên lặng: gần 0 message (chỉ khi trạng thái đổi). Đăng ký lại hàng loạt chỉ sinh message `resync` cho trạng thái
  thật sự đã đổi trong lúc mất kết nối.
- Message ~500 B JSON, nén lz4 batch 50 ms → vài KB/s ngay cả lúc bão.
- Thread thêm: ~3–5 (librdkafka). Bộ nhớ: `queueMaxKBytes` (16 MB) + hàng đợi spool (`spoolMemQueue` × ~500 B ≈ 10 MB).
- Đĩa: chỉ khi SPOOL; ghi tuần tự vài KB/s, `fdatasync` mỗi giây.
- Chi phí trên poller camera: dựng JSON + `producev` hoặc push vào deque — micro giây.

Kiểm bằng số thật ở K7.

## 9. Kế hoạch triển khai

| Bước | Nội dung | File | Kiểm |
|---|---|---|---|
| **K0** | Gửi team Service API: hợp đồng mục 4, khối `eventBus` mục 5.1, `eventCode` mục 7; chờ tên topic (D2) | `docs/` | Biên bản trong doc này |
| **K1** | Submodule `3rdpart/librdkafka` + `cmake/BundledRdKafka.cmake`, `ENABLE_KAFKA`, `ENABLE_BUNDLED_RDKAFKA`; link target manager | `.gitmodules`, `3rdpart/CMakeLists.txt`, `cmake/`, `CMakeLists.txt` | Build `build/` (-j2); build `-DENABLE_KAFKA=OFF` (build type riêng để né output dir chung); `ldd MediaServer` |
| **K2** | `OnvifEventKafka::toMessage` + chuẩn hoá `active` + `eventId`/`seq` + `resync`; quyết định gửi `Initialized` theo bảng giá trị đã gửi (mục 4.1, hàm thuần nhận bảng làm tham số) | `manager/Control/OnvifEventKafka.*` | Unit: từng `eventType`, `Changed`/`Deleted`, `pulse`, thiếu `UtcTime`, ký tự đặc biệt, giá trị trạng thái lạ; `Initialized` bằng / khác / chưa có giá trị đã gửi; `Deleted` xoá khoá; trần khoá / camera |
| **K3** | `EventSpool`: append/đọc/checkpoint/xoay segment/trần dung lượng + tuổi + đĩa trống/khôi phục khi khởi động | `manager/Common/EventSpool.*` | Unit thuần (thư mục tạm): round-trip, cắt record cụt/CRC sai, xoay + xoá segment đã gửi, vượt trần xoá cũ nhất, restart giữa chừng tiếp đúng checkpoint, trùng tối đa 1 lô |
| **K4** | `KafkaProducer` (RAII, settings → conf, produce không chặn, DR, broker `UP`, purge, flush) | `manager/Common/KafkaProducer.*` | Unit với **mock cluster librdkafka** (`test.mock.num.brokers=3`): DR, thứ tự theo key, `QUEUE_FULL` không chặn, purge trả DR, broker down/up |
| **K5** | `EventBus`: LIVE/SPOOL/DRAIN, áp cấu hình, timer poll, giới hạn theo camera, thống kê, `kBroadcastEventBusAlert`, `shutdown()`; gắn `OnvifEventHub::onEvent`, `server/main.cpp`, listener cảnh báo `server/WebHook.cpp` | `manager/Common/EventBus.*`, `manager/Control/OnvifEventHub.cpp`, `server/*`, `src/Common/config.*` | Unit (mock cluster): broker down → SPOOL → up → DRAIN → LIVE, không mất record, thứ tự theo key, sự kiện mới trong DRAIN xếp sau spool; đổi cấu hình khi đang gửi; shutdown khi broker chết → record nằm trong spool; cảnh báo phát đúng một lần mỗi chuyển trạng thái; bảng `_sent_states` trong `OnvifEventHub` chỉ cập nhật khi `publish` nhận record |
| **K6** | Cấu hình: đọc `media_server.eventBus` trong `loadServerConfigFromJson`, `kafka.lastEnabled`, khoá `kafka.*`, không dump bí mật | `server/Manager.cpp`, `conf/config.ini` | Unit/mock: khối có/không, topic rỗng, đổi brokers lúc chạy, `config.ini` sau reload không có password |
| **K7** | E2E: Kafka KRaft trong docker (`tests/integration/kafka/`), `mock_api.py` trả `eventBus` + ghi nhận `system-event`, `onvif_mock.py --events`, MediaServer thật 30 rồi 300 camera; consumer đếm theo `deviceId`, kiểm thứ tự + trùng. Kịch bản: broker dừng 60 s / 30 phút; `kill -9` MediaServer khi đang SPOOL rồi chạy lại; `spoolMaxMB` nhỏ để chạm trần; SASL_SSL | `tests/integration/` | Không mất sự kiện ngoài trần đã cấu hình; trùng chỉ trong cửa sổ đã nêu; cảnh báo tới mock API; thread/CPU/PTZ như baseline đo khi chưa có Kafka; số đo ghi vào doc này |
| **K8** | Docs: `docs/camera-control-protocol.md` mục sự kiện (Kafka, spool, cảnh báo), `conf/config.ini`, hợp đồng gửi team Service API | `docs/` | |

Kết quả K1 (2026-10-11): `build/` và `build-test/` dùng chung prefix (lần đầu build librdkafka ~3 phút với -j2),
MediaServer link được, `ldd` không thêm thư viện mới (libzstd/libsasl2 đã có sẵn từ trước). Test
`unit_rdkafka_bundle` (`tests/unit/common/test_rdkafka_bundle.cpp`): kiểm `builtin.features`, nhận cấu hình
`sasl_ssl` + SCRAM, gửi idempotent qua mock cluster với lz4/zstd/gzip; bộ unit 306/306. Bản `-DENABLE_KAFKA=OFF`:
configure không link, không có macro (chưa build vì chưa có code nào dùng macro). Dockerfile thêm `libzstd-dev` /
`libzstd1`, `build_docker_images.sh` kiểm submodule. Không cần copy `.so`: bản tĩnh.

Kết quả K2–K4 (2026-10-11), các lớp đứng riêng, chưa có chỗ gọi:

- `manager/Common/EventRecord.h` (mới, ngoài bảng mục 3): key + header + value, dùng chung cho ba lớp; không chứa topic.
- K2 `OnvifEventKafka`: `toRecord` (JSON một dòng, UTF-8 giữ nguyên), `normalizeActive` (true/1/active/on/closed/yes
  → `true`; false/0/inactive/off/open/opened/no → `false`; khác → `active: null`), `decide` (Drop/Send/SendResync) và
  `remember` (bảng `SentStates` truyền vào, trần khoá là tham số; khoá cấu hình `onvif.eventMaxStateKeys` làm ở K5).
  Unit `unit_onvif_event_kafka` 9/9.
- K3 `EventSpool`: như mục 6.2, thêm một điểm: segment đã gửi hết **chỉ bị xoá sau khi checkpoint mới đã ghi xuống
  đĩa** (`flushCheckpoint`), nếu không crash giữa hai lần ghi checkpoint sẽ nhảy qua record chưa xác nhận (test bắt
  được ở bản đầu). Đếm `pending`/`droppedRecords` chính xác qua cắt đuôi, xoá theo trần dung lượng và tuổi. Unit
  `unit_event_spool` 10/10 (round-trip, đuôi cụt, CRC sai, trần dung lượng, tuổi, đĩa đầy, crash giữa hai checkpoint).
- K4 `KafkaProducer`: số broker `UP` lấy từ statistics callback (`statistics.interval.ms`, mặc định 1000);
  `classify()` gom mã lỗi thành `broker_down/auth/topic/queue_full/fatal/purged` cho cảnh báo; lỗi cấu hình không
  in giá trị (mật khẩu). Unit `unit_kafka_producer` 6/6 trên mock cluster: thứ tự theo key, header + `replayed=1` tới
  broker, `QUEUE_FULL` không chặn, purge trả DR, broker chết → DR `broker_down` → sống lại gửi được.

Phụ thuộc: K0 → K1 → (K2, K3, K4) → K5 → K6 → K7 → K8. K1–K6 làm được trước khi có tên topic (dev dùng
`kafka.devOverride`). Mỗi bước: `cmake --build build -j2` + `ctest --test-dir build-test -R unit.` (build dir test
configure lại vì có file mới trong `manager/`).

## 10. Quyết định

| # | Câu hỏi | Chốt |
|---|---|---|
| D1 | Cấu hình Kafka từ đâu? | Service API, khối `media_server.eventBus` trong `configuration-2nd-gen` đã mã hoá (5.1) |
| D2 | Topic? | Cập nhật sau; lấy từ `eventBus.deviceEventTopic`, không có mặc định trong code |
| D3 | Gửi `Initialized`? | Không |
| D4 | Rơi về webhook HTTP? | Không |
| D5 | Broker không nhận được? | Spool đĩa (mục 6) + cảnh báo `on_system_alert` (mục 7) |
| D6 | Định dạng value? | JSON + `schemaVersion` |
| D7 | Trạng thái đổi trong lúc mất kết nối camera bị lỡ (hệ quả D3)? | `OnvifEventHub` nhớ giá trị đã gửi; `Initialized` khác giá trị đó → message `Changed` + `resync=true` (mục 4.1). Khởi động lại / failover vẫn không bù được: consumer xoá trạng thái theo `on_server_started` / camera offline |

## 11. Rủi ro

| Rủi ro | Giảm thiểu |
|---|---|
| librdkafka build tĩnh xung đột OpenSSL/zstd với AWS SDK, FFmpeg | Dùng cùng OpenSSL hệ thống; build K1 cả `build/` lẫn `build-test/`; kiểm `ldd MediaServer` |
| Output dir chung `release/linux/` giữa các build dir | Như FFmpeg/AWS: thư mục cài librdkafka riêng theo phiên bản; bản `ENABLE_KAFKA=OFF` dùng build type riêng |
| Spool làm đầy đĩa hệ thống | `spoolMaxMB`, `spoolMinFreeMB`, `spoolMaxAgeHours`; thư mục mặc định cạnh `./db`, không trên đĩa ghi hình |
| Ghi spool chậm chiếm thread `api()` | Một `SerialTaskQueue` (tối đa 1 thread cùng lúc), ghi theo lô; hàng đợi bộ nhớ có trần |
| Lọt record giữa DRAIN và LIVE (đảo thứ tự / mất) | Chuyển mode dưới mutex spool (6.1); unit K5 riêng cho ca này |
| Fatal error idempotent producer làm ngừng gửi im lặng | `error_cb` bắt `RD_KAFKA_RESP_ERR__FATAL` → SPOOL, tạo lại producer, cảnh báo |
| Gửi bù dội consumer sau khi broker chết lâu | `spoolReplayRate`, header `replayed=1` |
| Lỡ trạng thái do không gửi `Initialized` | Mục 4.1 (D7); còn hở khi MediaServer khởi động lại / failover → consumer xoá trạng thái theo `on_server_started` / camera offline |
| Lộ mật khẩu SASL qua `config.ini` dump hoặc log | Mục 5.3; review K6 riêng cho điểm này |
