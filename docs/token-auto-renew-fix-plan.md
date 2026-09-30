# Plan sửa cơ chế tự động gia hạn quyền media (UserAuthorManager)

Lập ngày 2026-09-30, dựa trên review `manager/User/UserAuthorManager.cpp` và các chỗ gọi nó.

## Trạng thái (2026-09-30, nhánh `fix/token-auto-renew`)

P1–P7 đã làm theo các khuyến nghị: D1a, D2a (xoá cookie HLS gọn hơn dự kiến), D3 giữ nguyên, D4 làm.

- **P5, khác plan:** không truyền token/device xuống `play_plugin`. Listener play vẫn đăng ký session
  gửi yêu cầu; sau đó notice `kBroadcastMediaSessionTransfer` **chuyển** đăng ký đó sang transport
  WebRTC hoặc cookie HLS. Cách này không phải lặp lại logic parse token/device hay bypass realm.
- **D4:** backend từ chối tường minh được xác định là HTTP 4xx, trừ 408/429.
- **Chạy thật** (camera giả, bản build tạm hạ TTL quyền từ 600 xuống 200 s, không commit):
  RTSP/HTTP-FLV/HLS đạt 9/9.
  - Hook chậm 4 s (harness đặt `hook.timeoutSec=5`) rồi 503 liên tục 150 s: phiên sống.
  - Hook từ chối: phiên bị cắt sau 11–17 s.
  - JWT hết hạn: phiên bị cắt sau 37–39 s.
- **WebRTC chưa chạy thật.** Mọi build dir đều tắt `ENABLE_WEBRTC`; build WebRTC còn cần
  `ENABLE_RTPPROXY=ON` (`WebRtcTalk.h` dùng `RtpSender`). Mới chỉ biên dịch object `WebRtcTransport.cpp`.
- **Follow-up ngoài plan:**
  - Lần phát đầu (không phải gia hạn) vẫn cache REJECT 600 s khi hook lỗi mạng/timeout.
  - Log Trace của `SqliteConnection` in nguyên JWT trong SQL của `user_sessions`.

## Bối cảnh

- Timer chạy mỗi 60 s, gọi `onManager()` trên một poller của `EventPollerPool`.
- Quyền theo `token + device` sống `min(now + 600, jwt_exp)` giây (`addAuthorCache`).
- Mỗi tick chạy theo thứ tự: `cleanExpiredTokenCache` → `cleanExpiredAuthorCache` (entry hết hạn thì
  shutdown session) → `checkActiveTokenAuthorizations` (entry còn ≤ 60 s thì gửi hook `on_play` với
  `force_refresh=true`).
- Hook trả về sẽ gọi `addAuthorCache(..., only_if_active=true)`. Hàm này chỉ ghi khi entry cũ **còn hạn**.

## Các vấn đề cần sửa

| # | Mức | Vấn đề | Bằng chứng |
|---|---|---|---|
| 1 | Cao | Thời gian báo trước bằng đúng chu kỳ timer (60 s). Hook chậm, lỗi thoáng qua, hoặc timer lệch 1 s đều làm session bị cắt oan, và không có lần thử lại | `UserAuthorManager.cpp:14`, `:19`, `:88`, `:268`; timeout hook mặc định 10 s, `retry=0` (`WebHook.cpp:92`, `:139`) |
| 2 | Cao | WebRTC và HLS không bị thu hồi. Session được đăng ký là HttpSession của request signaling/m3u8, không phải transport media | `WebHook.cpp:1253-1264`, `WebRtcTransport.cpp:1650`, `HttpFileManager.cpp:350` |
| 3 | Trung bình | Verify RSA và ghi SQLite (`saveUserSession`) chạy dưới `_mtx` toàn cục, trên thread poller | `UserAuthorManager.cpp:228-237`, `UserSessionCache.cpp:190-197` |
| 4 | Trung bình | JWT bị ghi nguyên văn vào log | `UserSessionCache.cpp:151` |
| 5 | Trung bình | Verify cho `exp` leeway 300 s, nhưng cache revoke và `addAuthorCache` từ chối ngay tại `exp`, nên REST và media cho kết quả khác nhau | `UserSessionCache.cpp:33`, `UserAuthorManager.cpp:217`, `:259` |
| 6 | Thấp | Phiên chỉ sống tối đa tới `jwt_exp`; client không có cách thay token cho phiên đang chạy | `UserAuthorManager.cpp:74` |
| 7 | Thấp | Tắt `enable_authorize` khi server đang chạy vẫn cắt các phiên cũ lúc entry hết hạn | `WebHook.cpp:1340`, `UserAuthorManager.cpp:197` |
| 8 | Thấp | Gia hạn bị từ chối không ghi REJECT, nên phiên mới vẫn vào được tới khi entry hết hạn | `WebHook.cpp:1372-1375` |

## Quyết định cần chốt trước khi làm

- **D1 – WebRTC (#2):** chọn một trong hai cách.
  - (a) Đăng ký chính transport RTC để revoke. Đây là cách **khuyến nghị**. Xem bước P5.
  - (b) Chấp nhận WebRTC không bị thu hồi và ghi rõ giới hạn này trong docs.
- **D2 – HLS (#2):** cookie HLS giữ kết quả xác thực, và `sender` chỉ là một kết nối HTTP.
  - (a) Khi revoke thì xoá cookie HLS gắn với token. Cần khảo sát thêm `HttpCookieManager`.
  - (b) Chấp nhận HLS được xác thực lại khi cookie hết hạn (`Hls::kViewerTimeoutSec`) và ghi rõ trong docs.
  - **Khuyến nghị:** khảo sát trước, sau đó chọn (a) nếu diff nhỏ.
- **D3 – #6:** giữ hành vi "phiên chết khi JWT hết hạn". **Khuyến nghị giữ** và chỉ ghi vào docs,
  vì cơ chế thay token cho phiên đang chạy là một tính năng mới cần cả phía client.
- **D4 – #8:** khi hook từ chối **tường minh** (backend trả code lỗi, không phải lỗi mạng) thì revoke
  ngay. **Khuyến nghị làm**, gộp vào P1. Đổi lại, cần phân biệt được lỗi mạng với từ chối trong callback.

## Các bước

### P1 – Gia hạn có biên an toàn và có thử lại (#1, #8)

File sửa: `manager/User/UserAuthorManager.cpp`, `server/WebHook.cpp`.

1. Tăng `kAuthorizationRefreshLeadTime` từ 60 lên 180 s. Giá trị này ≥ 2 chu kỳ timer + timeout hook,
   nên một cặp `token:device` có 2–3 lần thử trước khi hết hạn. Lần thử lại tự xảy ra ở tick sau, vì
   callback đã xoá khoá khỏi `_token_checks`.
2. `cleanExpiredAuthorCache()`: bỏ qua entry có khoá `token:device` còn nằm trong `_token_checks`
   (đang chờ hook trả lời). `do_http_hook` luôn gọi callback khi timeout, nên việc bỏ qua này
   có giới hạn thời gian.
3. `addAuthorCache(..., only_if_active=true)`: chỉ yêu cầu entry **còn tồn tại** với `permit=true`,
   không yêu cầu còn hạn. Mục đích của cờ này là không hồi sinh quyền đã bị `revokeTokenDevice` xoá,
   và việc kiểm tra entry còn tồn tại là đủ cho mục đích đó. Nhờ vậy một hook trả lời muộn vẫn gia hạn được.
4. (D4) Trong listener `kBroadcastDeviceAccess`: khi `force_refresh` và hook từ chối tường minh, gọi
   một API mới `UserAuthorManager::denyTokenDevice(token, device)`. API này ghi REJECT với TTL ngắn
   và gọi `revokeTokenDevice`. Lỗi mạng giữ hành vi hiện tại, tức là để tick sau thử lại.

Tiêu chí đạt:
- Hook trễ 10 s ở mọi pha của timer không làm session bị cắt.
- Hook lỗi một lần rồi hồi phục thì session vẫn sống.
- Hook từ chối tường minh thì session bị cắt ở tick kế tiếp.

### P2 – Log không lộ token (#4)

File sửa: `manager/User/UserSessionCache.cpp`.

Log `sub`/`user_id` nếu decode được. Nếu không decode được thì log độ dài token cùng 8 ký tự cuối của
chữ ký, không bao giờ log cả chuỗi. Rà thêm `rg -n '<< .*token' manager server` để sửa các chỗ tương tự.

### P3 – Leeway `exp` nhất quán (#5)

File sửa: `manager/User/UserSessionCache.cpp`.

Giữ leeway 300 s cho `nbf`/`iat` để chịu lệch đồng hồ, nhưng đặt leeway 0 cho `exp`, trùng với thời
điểm revoke. Trước khi sửa, kiểm tra bản jwt-cpp trong `3rdpart/` có `expires_at_leeway()` hay không.
Nếu không có, tự so `exp` với `now` sau khi verify.
Không kéo dài `_expired_at` (giữ invariant 1 trong `docs/user-author-manager-flow.md`).

### P4 – Đưa việc blocking ra khỏi mutex và khỏi poller (#3)

File sửa: `manager/User/UserAuthorManager.cpp`, `manager/User/UserSessionCache.{h,cpp}`.

1. `getTokenCache`: tra map dưới lock. Nếu chưa có thì nhả lock, dựng `UserSessionCache` (verify RSA)
   ngoài lock, rồi lock lại và `emplace`. Nếu một thread khác đã chèn trước thì dùng bản đã có.
2. Tách `saveUserSession()` khỏi constructor. Sau khi chèn thành công một cache mới có
   `hasProjectAccess()`, đẩy thao tác ghi sang `BlockingExecutors::api()`. Lambda capture giá trị,
   không capture `this`. Việc ghi là fire-and-forget và chỉ log khi lỗi.
3. Cập nhật `docs/threading-blocking-work.md`.

### P5 – WebRTC và HLS bị thu hồi thật (#2, tuỳ theo D1/D2)

1. Đổi `MediaSessionInfo` thành `{device_id, weak_ptr<void> owner, function<void(const SockException&)> close}`.
   Giữ nguyên `registerMediaSession(token, device, weak_ptr<Session>)` như hiện tại: closer của nó gọi
   `session->shutdown()`. Thêm một overload nhận owner và closer tuỳ ý.
2. WebRTC (D1a):
   - Không đăng ký HttpSession khi `args.schema == "rtc"`.
   - Truyền được token/device tới chỗ `play_plugin` tạo transport. Cách dự kiến là phát một notice mới
     sau khi `Type::create(...)` thành công, mang `info` và `weak_ptr<WebRtcTransport>`. WebHook nghe
     notice này, parse token từ `info.params` rồi đăng ký với closer `safeShutdown`.
   - Cần kiểm tra hàm shutdown an toàn luồng của `WebRtcTransport` trước khi viết.
3. HLS (D2a): khảo sát cách `canAccessPath` và `HttpCookieManager` giữ cookie theo `uid`/params. Nếu
   xoá được cookie theo token thì closer của HLS là xoá cookie. Nếu không làm được với diff nhỏ thì
   chuyển sang D2b.

### P6 – Tắt authorize khi đang chạy (#7)

File sửa: `manager/User/UserAuthorManager.cpp`.

Trong `onManager()`, nếu `Manager::kEnableAuthorize` là false thì xoá `_map_token_resource` và
`_map_token_sessions` mà không shutdown session.

### P7 – Cập nhật tài liệu

Sửa `docs/user-author-manager-flow.md`. Mục 3 của tài liệu này đang sai so với code (hook từ chối
không cắt ngay, và thứ tự cleanup cũng khác). Cần ghi lại lead time mới, cơ chế thử lại, các giao thức
được và không được thu hồi, và giới hạn theo `jwt_exp` (D3).

## Kiểm chứng

Hiện chưa có unit test cho `UserAuthorManager`: singleton có timer, còn verify cần khoá RSA thật.
Không tách API chỉ để phục vụ test.

- **Build:** `cmake --build build -j2 --target MediaServer` và build `build-sync-off`.
- **Chạy thật:** dùng `tests/integration/stress_vms/mock_api.py` làm hook `on_play`, thêm tuỳ chọn trễ,
  lỗi 5xx, hoặc từ chối cho endpoint này. Ký JWT test bằng cặp khoá RSA tạo tạm, `exp` = now + 30 phút.
  Để chạy nhanh, tạm giảm TTL 600 s trong bản build thử, không commit.
  Mở RTSP, FLV, WebRTC và HLS, rồi kiểm tra:
  1. Hook trễ 10 s: không phiên nào bị cắt sau 3 chu kỳ.
  2. Hook 5xx một lần: phiên sống.
  3. Hook từ chối: RTSP/FLV (và WebRTC/HLS nếu làm P5) bị cắt trong ≤ 60 s.
  4. JWT hết hạn: mọi phiên bị cắt; log không chứa chuỗi token.
- **Hồi quy:** `ctest --test-dir build-test --output-on-failure -R unit.`

## Thứ tự đề xuất

P2 → P3 → P1 → P4 → P6 → P7, sau đó làm P5 khi đã chốt D1/D2.
P2 và P3 nhỏ và độc lập. P1 là rủi ro vận hành lớn nhất.
