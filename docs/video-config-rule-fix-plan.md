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
| P5 | #2 Kết quả không có đường ra ngoài | Cao | **Cần chốt quyết định D1** |
| P6 | #6, #7 Báo sai ở nhánh phụ, dừng/xoá camera khi đang chạy rule | Thấp | P2, P4 |

P1 làm trước vì nhỏ, độc lập và là lỗ hổng. P5 để sau P2–P4 vì cần quyết định kênh ra và
nên phát đúng kết quả trước khi đưa nó ra ngoài.

## Quyết định cần chốt trước khi làm

- **D1 (P5)** — kênh trả kết quả cho phía gọi. Đề xuất: webhook mới
  `hook.on_video_config_rule_result` theo mẫu `do_http_hook` trong `server/WebHook.cpp`
  (body: `logId, deviceId, success, errCode, errMsg`), bỏ trống URL thì không gửi.
  Phương án khác: API tra cứu theo `logId` (cần lưu kết quả vào DB), hoặc kênh fork đang dùng
  — nên hỏi phía fork trước.
- **D2 (P3)** — ngữ nghĩa khi yêu cầu mới trùng cấu hình job đang chạy. Đề xuất: coi là
  **gia hạn** — `deadlineAt = max(cũ, mới)`, đổi `logId` sang yêu cầu mới, huỷ job QUEUED
  (báo `RULE_SUPERSEDED`), vì yêu cầu mới nhất là ý định hiện tại.
- **D3 (P6)** — khi camera bị xoá lúc rule đang chạy: đề xuất xoá dòng DB, không cố rollback
  (camera không còn thuộc hệ thống); khi camera chỉ dừng/khởi động lại: giữ nguyên như hiện tại
  (phục hồi lúc kết nối lại).

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

**Test** (`tests/unit/manager/test_camera_controller.cpp`, dùng `FakeOnvifControl` có sẵn):

- rule áp thành công, camera trả profile khớp → sau 1 chu kỳ có đúng 1 broadcast `success=true`;
- `hasPendingProfileConfig()` đúng với rule NEW/PROCESSING, sai khi chỉ có rule SUCCESS;
- camera nhận lệnh nhưng profile vẫn khác sau hết retry → `CONFIG_NOT_APPLIED` đúng 1 lần;
- set lỗi lần đầu, lần sau thành công → không báo lỗi sớm, cuối cùng báo `success=true`.

Mutation check: bỏ bước 1 thì test chu kỳ phải đỏ.

## P3 — Gia hạn và giữ ý định mới nhất (#4) — theo D2

**Sửa** `manager/Camera/VideoConfigJobManager.cpp` `onBaselineReady` (dòng 143):

1. Tách quyết định thành hàm thuần, ví dụ
   `RuleDecision decideRuleRequest(const SlotView &slot, const VideoEncoderConfig &baseline, const RuleRequest &req)`
   trả về một trong: `START`, `EXTEND_ACTIVE`, `QUEUE`, `CANCEL_QUEUED_AND_EXTEND`, `NOOP_REPORT_SUCCESS`.
2. Yêu cầu trùng cấu hình ACTIVE → `EXTEND_ACTIVE`: cập nhật `deadlineAt`, `logId`,
   `updatedAt`; persist; dựng lại timer. Nếu đang có QUEUED → báo `RULE_SUPERSEDED` cho QUEUED
   và xoá nó (cả DB).
3. Không có slot và yêu cầu bằng baseline → báo `success=true` ngay (hiện đang im lặng).
4. Rule mới được áp khi job ACTIVE đã báo kết quả → phải tạo lại `_ruleReportMap` cho `logId`
   mới (gia hạn cũng cần báo kết quả cho `logId` mới: báo ngay `success=true` nếu cấu hình
   hiện đã được xác nhận SUCCESS).

**Test:** GTest cho `decideRuleRequest` phủ đủ bảng quyết định (không cần camera thật):
gia hạn khi đang có QUEUED, gia hạn với deadline ngắn hơn (giữ deadline cũ), yêu cầu bằng
baseline khi đang ACTIVE (vẫn QUEUE để rollback đúng hạn), trùng QUEUED.

## P4 — Ghi DB tuần tự (#5)

**Sửa:**

1. `VideoConfigJobManager::asyncDbTask`: ghi DB qua một `SerialTaskQueue`
   (`manager/Common/SerialTaskQueue.h`) chạy trên `BlockingExecutors::api()` — một hàng đợi
   chung cho manager là đủ (số lệnh ghi rất ít), giữ đúng thứ tự phát sinh trên poller.
   Không dùng `WorkThreadPool`.
2. `VideoConfigRuleJobImp::addOrUpdate`: bọc tìm-rồi-ghi trong `beginWriteTxn()`
   (BEGIN IMMEDIATE), và bỏ qua ghi nếu dòng hiện có `updatedAt` mới hơn — cùng quy tắc
   `removeIfNotNewer`. Logic "mới hơn thì bỏ qua" là quyết định nghiệp vụ → nằm ở `*Imp`,
   SQL ở `*Repository`.

**Test** (`tests/unit/storage/`, theo mẫu `test_tier_txn_lock.cpp`): hai `addOrUpdate` cùng id
đồng thời không lỗi UNIQUE; ghi bản cũ sau bản mới không đè; thứ tự thực thi của hàng đợi
đúng thứ tự `post`.

## P5 — Đưa kết quả ra ngoài (#2) — theo D1

Với phương án webhook đề xuất:

1. `server/WebHook.cpp`: thêm `kOnVideoConfigRuleResult = HOOK_FIELD "on_video_config_rule_result"`,
   đăng ký default rỗng, listener `kBroadcastVideoConfigRuleResult` gọi `do_http_hook`
   (dùng retry chung `hook_retry`).
2. Các nhánh đang im lặng sau khi đã trả `accepted` phải phát kết quả lỗi:
   camera offline (`WebApiControl.cpp:669`) → `DEVICE_OFFLINE`, không phải camera →
   `NOT_A_CAMERA`, không xác định profile token (`:684`) → `PROFILE_TOKEN_NOT_FOUND`.
3. Tài liệu: thêm mục API + webhook vào `docs/` (API mới chưa có tài liệu), liệt kê đủ bảng mã
   lỗi: `GET_CONFIG_FAILED, SET_CONFIG_FAILED, CONFIG_NOT_APPLIED, CONFIG_NOT_CONFIRMED,
   CONTROLLER_OFFLINE, RULE_EXPIRED, RULE_SUPERSEDED, DEVICE_OFFLINE, NOT_A_CAMERA,
   PROFILE_TOKEN_NOT_FOUND`. Cập nhật `conf/config.ini` và `docs/README.md`.

**Test:** listener gọi hook đúng body (mock `do_http_hook` hoặc HTTP server giả như các test
hook hiện có); mỗi `logId` + `deviceId` chỉ nhận đúng 1 kết quả qua các kịch bản P2/P3.

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

## Kiểm tra cuối

- `cmake --build build-test -j2` (máy ~7 GB RAM), `ctest --test-dir build-test -R unit.`.
- Chạy thật với ít nhất 1 camera ONVIF: áp rule 120 s → nhận webhook `success=true` trong
  ~1 chu kỳ; hết hạn → camera về baseline; gửi lại cùng cấu hình trước hạn → deadline được gia
  hạn; rút mạng camera lúc hết hạn → cắm lại thì baseline được đẩy lại.
- Gọi API không có hoặc sai `X-Secret-Key` nhận 401.
