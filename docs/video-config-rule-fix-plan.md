# Plan sửa luồng video config rule

Nhánh sửa: `fix/video-config-rule`, tách từ `feature/merge-video-config-rule` (base `05254316`, port từ fork recording-encryption).
Nguồn: review ngày 2026-09-23. Mỗi phase build, test và commit riêng (commit message
tiếng Việt, chỉ commit khi test đạt). Kiểm tra lại số dòng trước khi sửa — các số dưới đây
tính tại `05254316`.

## Tóm tắt luồng hiện tại

```text
POST /media/mserver/device/onvifApplyVideoConfigRule        (WebApiControl.cpp:608)
  └─ mỗi camera: trả "accepted" ngay, rồi async lên owner poller
       └─ getPrimaryProfileToken → VideoConfigJobManager::handleRuleRequest
            └─ ensureBaselineVideoConfig (đọc camera nếu chưa có, lưu stream_settings)
                 └─ onBaselineReady
                      ├─ chưa có slot  → ACTIVE: persistJob + applyRuleVideoConfig + timer deadline
                      └─ đã có slot    → QUEUED (1 chỗ, cái cũ bị RULE_SUPERSEDED)
timer deadline → promoteOrRollback → QUEUED lên ACTIVE, hoặc finishJob → rollback baseline
controller connect → recoverDeviceJobs → đọc DB → áp tiếp job còn hạn
CameraController::applyMediaProfileSync (chu kỳ bảo trì ~60 s) → retry rule + báo kết quả
```

## Thứ tự và lý do

| Phase | Vấn đề | Mức | Phụ thuộc |
|---|---|---|---|
| P1 | #3 Xác thực sai mô hình: API service-to-service đang đòi JWT | Cao (bảo mật) | — |
| P2 | #1 Kết quả báo muộn/sai, không retry trong thời hạn | Cao | — |
| P3 | #4 Yêu cầu gia hạn bị nuốt, ý định mới nhất bị mất | Trung bình | P2 (cùng đường báo kết quả) |
| P4 | #5 Ghi DB song song, sai thứ tự | Trung bình | — |
| P5 | #2 Kết quả không có đường ra ngoài | Cao | D1 đã chốt: webhook + DB |
| P6 | #6, #7 Báo sai ở nhánh phụ, dừng/xoá camera khi đang chạy rule | Thấp | P2, P4 |
| P7 | #8 `onvifSetVideoConfigs` giằng co với rule đang chạy | Trung bình | P2 (sync mỗi chu kỳ khi rule chờ); theo D4 |

P1 làm trước vì nhỏ, độc lập và là lỗ hổng. P5 để sau P2–P4 vì cần quyết định kênh ra và
nên phát đúng kết quả trước khi đưa nó ra ngoài.

## Quyết định cần chốt trước khi làm

- **D1 (P5) — ĐÃ CHỐT 2026-09-23: cả hai** — webhook `hook.on_video_config_rule_result` **và**
  lưu kết quả vào DB + API tra cứu theo `logId`. Webhook giữ đúng hợp đồng đang có ở fork
  (nhánh `import/legacy-fork-snapshot`): đường dẫn mặc định `/api/media-server/action-callback`,
  body `logId, deviceId, status (SUCCESS|FAILED), errorCode, errorMessage` (null khi thành công).
  Bản fork khai báo `bool success` theo giá trị nên hook của nó thực ra chưa bao giờ được gửi (P2).
- **D2 (P3)** — ĐÃ CHỐT 2026-09-23 theo đề xuất. Ngữ nghĩa khi yêu cầu mới trùng cấu hình job đang chạy: coi là
  **gia hạn** — `deadlineAt = max(cũ, mới)`, đổi `logId` sang yêu cầu mới, huỷ job QUEUED
  (báo `RULE_SUPERSEDED`), vì yêu cầu mới nhất là ý định hiện tại.
- **D3 (P6)** — khi camera bị xoá lúc rule đang chạy: đề xuất xoá dòng DB, không cố rollback
  (camera không còn thuộc hệ thống); khi camera chỉ dừng/khởi động lại: giữ nguyên như hiện tại
  (phục hồi lúc kết nối lại).
- **D4 (P7) — ĐÃ CHỐT 2026-09-23** — người dùng gọi `onvifSetVideoConfigs` lên profile đang
  bị rule chiếm: **chỉ cập nhật baseline** (`_profileConfigMap` + `stream_settings`), **không**
  gửi SOAP; trả thành công kèm cờ "hoãn tới khi rule hết hạn". Rollback cuối rule sẽ đẩy cấu hình
  mới này. Phương án bị loại: từ chối yêu cầu bằng mã lỗi riêng.

## P1 — Xác thực service-to-service bằng `manager.apiSecret` (#3)

Chốt 2026-09-23: API này do service khác gọi, **không có JWT**, chỉ xác thực bằng
`Manager::kApiSecret`; **không** kiểm tra quyền người dùng hay quyền trên thiết bị. Vì vậy
`CHECK_AUTH_TOKEN()` + `CHECK_USER_PERMISSION(...)` của bản port là sai — service gọi không có
token nên luôn bị từ chối khi `enableAuthorize=1`.

**Sửa** `server/WebApiControl.cpp` handler `onvifApplyVideoConfigRule`: thay hai guard trên bằng
`CHECK_API_KEY()` có sẵn (`server/WebApi.h`, đang dùng cho `/media/esc/extractArchived/job`),
**không sửa macro**. Giữ `getOwnership()` (khoá độc quyền thiết bị, không phải quyền người dùng).

Hành vi của `CHECK_API_KEY()` mà phía gọi cần biết (giữ nguyên, ghi vào tài liệu API ở P5):

- đọc `X-Secret-Key` từ header, nhưng cũng nhận nếu nằm ở query string hoặc body;
- `manager.enableAuthorize=0` thì bỏ qua;
- `manager.apiSecret` rỗng thì chỉ cần header có mặt, không so giá trị — môi trường triển khai
  phải cấu hình secret khác rỗng;
- thiếu hoặc sai → HTTP 401, `CODE_UNAUTHORIZED`.

**Test:** không thêm unit test cho macro (không đổi). Build + `ctest -R unit.`.

**Xong khi:** service gọi với `X-Secret-Key` đúng được nhận, không cần JWT; thiếu hoặc sai
secret nhận 401.

## P2 — Báo kết quả đúng lúc, retry trong thời hạn (#1)

**Nguyên nhân:** `hasPendingProfileConfig()` (`CameraController.cpp:33`) chỉ xét
`_profileConfigMap`, nên rule đang chờ không kéo lần đọc lại profile; mặc định
`profileResyncIntervalSec = 900`. Áp trực tiếp thành công chỉ đặt SUCCESS
(`CameraController.cpp:1064`) mà không báo; kết quả chỉ được báo trong `applyMediaProfileSync`.

**Sửa** `manager/Camera/CameraController.cpp`:

1. `hasPendingProfileConfig()` trả `true` khi có entry `_ruleConfigMap` ở NEW hoặc PROCESSING.
   Chi phí: camera có rule đang chờ sẽ đọc lại profile mỗi chu kỳ (~60 s) cho tới khi xác nhận —
   chấp nhận được vì rule là số ít và có thời hạn.
2. Callback của `applyRuleVideoConfigAsync`: set thành công → trạng thái **PROCESSING**
   (không phải SUCCESS). Lần sync kế tiếp đọc lại camera, thấy khớp mới đánh SUCCESS và
   `reportRuleConfigResult(true)`. Giữ nguyên ngữ nghĩa "camera nhận lệnh nhưng không áp"
   (`CONFIG_NOT_APPLIED`) mà nhánh sync đã có.
3. Set thất bại ở lần áp trực tiếp: giữ PROCESSING với `retry_time` hiện có để sync retry
   (hiện đã vậy), nhưng log mức `WarnL` thay vì `TraceL` ở `VideoConfigJobManager::startJob`.
4. `finishJob` (`VideoConfigJobManager.cpp:269`): chỉ báo `CONTROLLER_OFFLINE` khi controller
   thật sự không kết nối; nếu đã kết nối mà chưa xác nhận được thì báo mã mới
   `CONFIG_NOT_CONFIRMED`. Cần thêm một getter kiểu `isControllerConnected()` qua
   `GenericRtspCameraImp`.

5. **Lỗi phát hiện khi viết test:** `BroadcastVideoConfigRuleResultArgs` (`src/Common/config.h`)
   khai báo `bool success` theo giá trị. `NOTICE_EMIT` forward nó thành `bool&&`, không khớp
   `std::function` của listener nào, nên sự kiện **không bao giờ tới listener** (không lỗi, không
   log). Sửa thành `const bool &success` theo quy ước của `BroadcastMediaSeeked2Args`. Bắt buộc
   cho P5: webhook sẽ không nhận được gì nếu thiếu bản sửa này.

**Test** (`tests/unit/manager/test_camera_controller.cpp`, fixture `CameraControllerRuleTest`,
dùng `FakeOnvifControl` có sẵn):

- `PendingRuleKeepsTheProfileReReadDue`: sau lần áp trực tiếp, rule ở PROCESSING và
  `hasPendingProfileConfig()` đúng; sau 1 lần đọc lại thì SUCCESS và hết pending;
- `AppliedRuleIsReportedOnceWhenTheReReadConfirmsIt`: không báo trước khi đọc lại; đọc lại 2 lần
  → đúng 1 kết quả `success=true`, không đẩy lại rule;
- `FailedDirectSetIsRetriedInsteadOfReported`: lần áp trực tiếp lỗi không bị báo, lần đọc lại
  đẩy lại rule rồi xác nhận → `success=true`;
- `RuleTheCameraNeverAppliesIsReportedOnceAfterTheRetries`: camera nhận lệnh nhưng giữ cấu hình
  cũ → 1 lần áp + 5 lần đẩy lại, rồi đúng 1 lần `CONFIG_NOT_APPLIED`.

Giữ nguyên ngữ nghĩa sẵn có: lệnh set **trong lần đọc lại** bị lỗi SOAP thì báo
`SET_CONFIG_FAILED` ngay (giống nhánh baseline), không retry tiếp.

Mutation check đã chạy: bỏ bước 1 → 2 test đỏ; áp trực tiếp đánh SUCCESS như cũ → 1 test đỏ;
`bool success` theo giá trị → 3 test đỏ.

Chưa có unit test cho bước 4 (`finishJob`): `VideoConfigJobManager` là singleton gắn với
`GenericRtspCameraImp`, cần dựng camera thật — kiểm khi chạy thật.

## P3 — Gia hạn và giữ ý định mới nhất (#4) — D2 đã chốt

D2 chốt 2026-09-23 theo đề xuất: yêu cầu trùng cấu hình job đang chạy là **gia hạn** —
`deadlineAt = max(cũ, mới)`, job mang `logId` mới, job QUEUED bị huỷ (`RULE_SUPERSEDED`).

**Đã sửa:**

1. `manager/Camera/VideoConfigRuleDecision.h` (header-only, không cần reconfigure GLOB):
   `decideRuleRequest(...)` trả `START`, `ALREADY_APPLIED`, `EXTEND_ACTIVE` hoặc `QUEUE`, kèm
   `supersedeQueued` và deadline của job nhận request. Không đụng camera, timer hay DB.
2. `VideoConfigJobManager::onBaselineReady` làm theo bảng đó:
   - `ALREADY_APPLIED` (không có job, yêu cầu bằng baseline) → báo `success=true` ngay (trước đây
     im lặng);
   - `EXTEND_ACTIVE` → `extendJob`: cập nhật `logId`, `receivedAt`, `deadlineAt`, `updatedAt`,
     persist, chỉ dựng lại timer khi deadline đổi (`armDeadlineTimer`, tách từ `startJob`);
     nếu có QUEUED thì báo `RULE_SUPERSEDED` và xoá dòng QUEUED;
   - `QUEUE` → như cũ; dòng QUEUED cũ **không** bị xoá riêng mà được `persistJob` ghi đè (xoá
     rồi ghi cùng id trên pool `api()` nhiều thread có thể đảo thứ tự và mất dòng mới — P4).
3. Kết quả cho mọi request của một job (`CameraController`):
   - `RuleReportContext` giữ `logIds` (vector) + `success`; `reportRuleConfigResult` phát cho
     từng `logId`;
   - `attachRuleLogId(token, logId)`: rule chưa báo → gắn thêm, cùng nhận kết quả sau;
     đã báo thành công → báo `success=true` ngay cho `logId` mới; đã báo thất bại hoặc không có
     rule → `NEEDS_REAPPLY`, `extendJob` áp lại cấu hình qua `startJob`;
   - `reportRuleFailureIfPending(token, code, msg)`: `finishJob` dùng để báo
     `CONFIG_NOT_CONFIRMED`/`CONTROLLER_OFFLINE` cho mọi `logId` của job; không có controller
     thì chỉ báo cho `logId` cuối (như trước).
4. Bẫy biên dịch: `NOTICE_EMIT` không nhận literal chuỗi `""` cho tham số `const std::string &`
   (`std::forward` từ chối tạo string tạm) — truyền biến `const string`.

**Test** (`tests/unit/manager/test_camera_controller.cpp`):

- `VideoConfigRuleDecisionTest` (5 test): request đầu START / ALREADY_APPLIED; gia hạn lấy
  deadline muộn hơn, request ngắn hơn không cắt job; gia hạn huỷ QUEUED; cấu hình khác thì QUEUE
  và thay QUEUED (kể cả trùng QUEUED); yêu cầu bằng baseline khi đang chạy thì vẫn QUEUE.
- `CameraControllerRuleExtendTest` (5 test): không có rule → áp lại; gia hạn trước khi xác nhận
  → cả hai `logId` nhận `success`; gia hạn sau khi xác nhận → `success` ngay, không đẩy lại;
  camera đã từ chối → áp lại, không lặp lỗi cho `logId` mới; hết hạn → mỗi `logId` nhận đúng
  một `CONFIG_NOT_CONFIRMED`.

Mutation check đã chạy: gia hạn lấy deadline của request → 1 test đỏ; không gắn `logId` → 2 test
đỏ; rule bị từ chối vẫn gắn → 1 test đỏ.

Chưa có unit test cho `extendJob`/`onBaselineReady` (singleton + `GenericRtspCameraImp`) — kiểm
khi chạy thật: gửi lại cùng cấu hình trước hạn → không rollback ở deadline cũ.

## P4 — Ghi DB tuần tự (#5) — XONG

**Đã sửa:**

1. `VideoConfigJobManager::asyncDbTask`: mọi thao tác DB (ghi và cả lần đọc của
   `recoverDeviceJobs`) đi qua một `SerialTaskQueue` chung (`video-config-rule-db`) trên
   `BlockingExecutors::api()` — chạy đúng thứ tự phát sinh trên poller, lần đọc khi phục hồi thấy
   mọi chuyển trạng thái đã persist trước nó. Không dùng `WorkThreadPool`.
2. `VideoConfigRuleJobImp` (`manager/Storage/VideoConfigRuleJob.h`):
   - `addOrUpdate` tìm-rồi-ghi trong một `beginWriteTxn()` (BEGIN IMMEDIATE); dòng đang có
     `updatedAt` lớn hơn thì bỏ qua, trả `false`;
   - `removeIfNotNewer(id, ts)` chuyển từ manager sang `*Imp`, cũng trong `beginWriteTxn()`.

**Test** (`tests/unit/storage/test_video_config_rule_job.cpp`, target
`unit_video_config_rule_job`, DB mserver tạm dựng từ migration thật):

- `ConcurrentFirstWritesOfOneRowDoNotCollide`: 8 thread cùng ghi lần đầu một id → không lỗi,
  dòng còn lại là bản `updatedAt` mới nhất;
- `OlderWriteDoesNotOverwriteNewerRow`, `NewerOrEqualWriteUpdatesTheRow`;
- `RemoveKeepsARowUpdatedAfterTheTransition`.

Thứ tự thực thi của hàng đợi đã có test ở `test_serial_task_queue.cpp`
(`RunsTasksInOrderWithoutOverlap`), không lặp lại.

Mutation check đã chạy: `addOrUpdate` dùng transaction deferred → test đồng thời đỏ
(`database is locked`, race nên không đỏ mọi lần); bỏ kiểm tra `updatedAt` → 2 test đỏ.

## P5 — Đưa kết quả ra ngoài (#2) — XONG, theo D1 "cả hai"

Tài liệu cho phía gọi: [api-video-config-rule.md](./api-video-config-rule.md).

**Đã sửa:**

1. Webhook (`server/WebHook.cpp`): `kOnVideoConfigRuleResult`, mặc định
   `/api/media-server/action-callback` như fork; listener `kBroadcastVideoConfigRuleResult` gửi
   `VideoConfigRuleResult::toHookJson()` qua `do_http_hook` (retry chung `hook.retry`), thêm header
   `Idempotency-Key: logId|deviceId`.
2. Lưu kết quả: migration `20_video_config_rule_result.sql` (số 19 đã được `develop` dùng), entity +
   `VideoConfigRuleResultImp` ở `manager/Storage/VideoConfigRuleResult.h` (header-only):
   `markPending`, `recordResult` (ghi trong `beginWriteTxn`, giữ `createdAt`), `findByLogId`,
   `removeUpdatedBefore`.
3. `VideoConfigJobManager`:
   - `start()`/`stop()` (gọi từ `server/Manager.cpp`, trước khi nạp camera) đăng ký listener ghi kết
     quả vào DB;
   - `recordAccepted(logId, deviceIds)` ghi `PENDING`; `queryResults(logId, cb)` đọc;
   - mọi thao tác DB của job và kết quả đi chung hàng đợi tuần tự `postDbTask` (P4), nên kết quả
     không bao giờ tới trước dòng `PENDING`, và lần tra cứu thấy mọi lần ghi xếp hàng trước nó;
   - dọn kết quả cũ theo `video_config_rule.result_keep_hours` (mặc định 168, `<= 0` giữ vĩnh
     viễn), tối đa một lần mỗi giờ, chạy khi start và khi có request mới.
4. Handler `onvifApplyVideoConfigRule`: kiểm tra xong toàn bộ danh sách mới ghi `PENDING`, rồi mới
   giao việc cho poller của camera. Các nhánh trước đây im lặng thì giờ báo `DEVICE_OFFLINE`
   (camera ngắt kết nối, hoặc statistic đã bị giải phóng), `NOT_A_CAMERA`,
   `PROFILE_TOKEN_NOT_FOUND`. `deviceId` trùng trong cùng request bị từ chối (`902001`).
5. API mới `GET /media/mserver/device/onvifGetVideoConfigRuleResult?logId=` (`CHECK_API_KEY`),
   `runApiCallbackWork` + `queryResults`.
6. `conf/config.ini`, `docs/README.md`, tài liệu API mới.

**Test** (`tests/unit/storage/test_video_config_rule_job.cpp`, target `unit_video_config_rule_job`
giờ link như `unit_camera_controller`, dùng stub của tier-storage):

- `VideoConfigRuleResultImpTest` (3 test): `PENDING` → kết quả, giữ `createdAt`, lọc theo `logId`,
  sắp theo `deviceId`, gửi lại cùng `logId` thì làm lại từ đầu; kết quả không có dòng `PENDING`
  thì tạo mới; dọn theo mốc thời gian;
- `VideoConfigRuleResultTest.HookBodyMatchesTheForkContract`;
- `VideoConfigJobManagerResultTest.ResultsFollowTheAcceptedRowsInOrder`: `recordAccepted` → báo
  lỗi + phát thành công → tra cứu thấy đủ; sau `stop()` không ghi nữa.

Mutation check đã chạy: listener ghi DB thẳng trên pool `api()` thay vì qua hàng đợi → test thứ tự
đỏ; body thành công dùng chuỗi thay vì `null` → test hợp đồng đỏ.

Chưa có unit test cho listener webhook (cần link `server/WebHook.cpp`) và handler — kiểm khi chạy
thật. Test "mỗi `logId` + `deviceId` nhận đúng 1 kết quả" đã có ở mức controller (P2/P3).

## P6 — Các nhánh phụ (#6, #7)

1. Khôi phục sau restart: job đã hết hạn khi server down → không báo `CONTROLLER_OFFLINE`;
   báo `RULE_EXPIRED` với thông điệp "hết hạn khi server không chạy" (hoặc không báo gì nếu
   D1 chọn lưu kết quả vào DB và kết quả đã được lưu trước đó).
2. `promoteOrRollback` khi QUEUED bằng baseline (`VideoConfigJobManager.cpp:246`): báo
   `success=true` cho `logId` của QUEUED trước khi `finishJob`.
3. Theo D3: khi camera bị **xoá** (không chỉ dừng), xoá các dòng `video_config_rule_job` của
   camera. Cần xác định sự kiện xoá camera (khác `kBroadcastCameraStopped`) — tra
   `CameraManager` trước khi làm.
4. Ghi chú trong `docs/` về trường hợp `_keepConfigProfileAndStream=1` + server restart lúc
   camera offline: cấu hình rule có thể bị nhận làm baseline. Chỉ sửa nếu người dùng yêu cầu
   (cần lưu baseline kèm job — đổi schema = migration mới `20_…`, không sửa `18_…`).

## P7 — `onvifSetVideoConfigs` khi rule đang chạy (#8) — theo D4

**Vấn đề:** `CameraController::setMediaProfileAsync` không biết `_ruleConfigMap` đang giữ token.
Khi rule đang chạy, người dùng set cấu hình qua `onvifSetVideoConfigs` thì:

- camera bị đẩy cấu hình người dùng ngay, chu kỳ sync kế tiếp đẩy lại cấu hình rule, hết hạn lại
  rollback về cấu hình người dùng → encoder khởi động lại nhiều lần, stream giật;
- API trả 200 + cấu hình mới, nhưng camera chỉ giữ nó tới lần sync kế tiếp;
- lần set đó làm rule tốn thêm một lượt `retry_time`.

Không có data race (mọi map trên owner poller, SOAP qua `_device_queue` tuần tự) — lỗi là ở
nghiệp vụ.

**Race phụ cùng chỗ:** `GenericRtspCameraImp::ensureBaselineVideoConfig` đọc camera khi token
chưa có trong `_profileConfigMap`, rồi `addProfileConfig(seed)` **vô điều kiện**. Nếu người dùng
set trong lúc lệnh đọc đang chờ trên device queue, nhánh "chưa có entry" của
`setMediaProfileAsync` đã ghi cấu hình người dùng vào map, rồi seed (cấu hình cũ của camera) đè
lên → baseline và `stream_settings` quay về cấu hình cũ, rollback cuối rule xoá mất thay đổi của
người dùng.

**Sửa:**

1. `CameraController::setMediaProfileAsync`: sau kiểm tra controller đã kết nối và so sánh với
   baseline như hiện tại (trùng → `CODE_ONVIF_SET_CONFIG_NOT_CHANGE`), nếu
   `_ruleConfigMap.count(profileToken)`:
   - `addProfileConfig(profileToken, new_f_config)` với `NEW`, `retry_time=5` (phát
     `kBroadcastStreamSettingChange` để `stream_settings` giữ cấu hình người dùng muốn — cần cho
     restart giữa chừng);
   - **không** `postDeviceTask` lệnh set; không đụng `_ruleConfigMap`/`_ruleReportMap`;
   - gọi `cb` thành công với dấu hiệu "hoãn": `SockException(Err_success, ..., ApiErrCode::CODE_ONVIF_SET_CONFIG_DEFERRED)`
     (mã mới trong `server/WebApiErrCode.h`; `Err_success` nên handler vẫn đi nhánh thành công).

   Quyết định nằm trong controller (không ở handler) vì handler có thể phải đi đường
   `getMediaProfile` bất đồng bộ trước khi set, rule có thể bắt đầu trong khoảng đó.
2. Không cần sửa `applyMediaProfileSync`: nhánh rule `continue` nên baseline `NEW` nằm yên tới khi
   rule kết thúc. `rollbackRuleVideoConfigAsync` đẩy baseline (giờ là cấu hình người dùng); lần
   sync sau xác nhận `SUCCESS` (P2 giữ sync mỗi chu kỳ vì baseline đang `NEW`); rollback lỗi thì
   `markBaselineNeedResync` như hiện tại. `promoteOrRollback` so job QUEUED với baseline mới — đúng
   ý: QUEUED trùng cấu hình người dùng thì kết thúc luôn.
3. Handler `onvifSetVideoConfigs` (`server/WebApiControl.cpp`): nhận mã hoãn thì trả
   `code=0`, `data.flag=true`, `data.config` như cũ, thêm `data.deferred=true` và
   `data.deferredUntil` (ms). `deferredUntil` lấy từ getter mới
   `VideoConfigJobManager::getRuleDeadline(deviceId, token, int64_t &deadlineAt)` — `deadlineAt`
   của QUEUED nếu có, không thì của ACTIVE; chỉ là ước lượng vì P3 có thể gia hạn.
4. Race phụ: tách phần seed thành `CameraController::seedBaselineIfAbsent(token, seed)` trả về
   baseline thực dùng — chỉ `addProfileConfig` khi token vẫn chưa có entry lúc callback chạy,
   ngược lại trả entry hiện có. `ensureBaselineVideoConfig` dùng giá trị trả về.
5. Tài liệu API ở P5: ghi rõ hành vi hoãn và hai trường mới của `onvifSetVideoConfigs`.

**Test** (`tests/unit/manager/test_camera_controller.cpp`, fixture `CameraControllerRuleTest`):

- `SetDuringRuleOnlyUpdatesTheBaseline`: rule đã áp và xác nhận; set cấu hình người dùng →
  `FakeOnvifControl` không ghi nhận lệnh set nào, `cb` mang mã hoãn, baseline = cấu hình người
  dùng, entry rule không đổi; sync tiếp theo không đẩy gì và không báo lại kết quả rule;
- `RollbackAfterDeferredSetPushesTheUserConfig`: rollback → đúng 1 lệnh set với cấu hình người
  dùng; sync sau đó đánh baseline `SUCCESS`;
- `SetEqualToBaselineDuringRuleIsNotChange`;
- `SetWithoutRuleStillPushesToTheCamera` (hồi quy hành vi cũ);
- `SeedDoesNotOverwriteAConcurrentUserSet`: có entry trước khi seed → seed trả entry đó, map giữ
  nguyên.

Mutation check: bỏ kiểm tra `_ruleConfigMap` ở bước 1 → test đầu đỏ; seed ghi vô điều kiện → test
cuối đỏ.

## Kiểm tra cuối

- `cmake --build build-test -j2` (máy ~7 GB RAM), `ctest --test-dir build-test -R unit.`.
- Chạy thật với ít nhất 1 camera ONVIF: áp rule 120 s → nhận webhook `success=true` trong
  ~1 chu kỳ; hết hạn → camera về baseline; gửi lại cùng cấu hình trước hạn → deadline được gia
  hạn; rút mạng camera lúc hết hạn → cắm lại thì baseline được đẩy lại.
- Trong lúc rule chạy, gọi `onvifSetVideoConfigs` → trả `deferred=true`, camera vẫn giữ cấu hình
  rule (không khởi động lại encoder); hết hạn → camera về cấu hình người dùng vừa set.
- Gọi API không có hoặc sai `X-Secret-Key` nhận 401.
