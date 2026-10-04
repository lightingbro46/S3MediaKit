# Plan: hỗ trợ camera ONVIF chỉ bật xác thực HTTP Digest

Ngày lập: 2026-10-03. Nhánh đề xuất: `feature/onvif-http-digest` (tách từ `develop`).
Trạng thái: Q1–Q3 đã chốt 2026-10-03 (mục 8); đã code trên `feature/onvif-http-digest`, kết quả ở mục 9.

## 1. Bối cảnh và mục tiêu

Camera ONVIF (Hikvision, Dahua, Uniview…) cho chọn kiểu xác thực của dịch vụ web ONVIF:

| Chế độ trên camera | Camera chấp nhận |
|---|---|
| `digest&ws-username token` | WS-Security UsernameToken trong SOAP header **hoặc** HTTP Digest |
| `digest` | **Chỉ** HTTP Digest (RFC 2617/7616). Request không có `Authorization: Digest` bị trả `401` + `WWW-Authenticate: Digest realm=…, nonce=…, qop=…` dù XML có WS-Security hợp lệ |

Codebase hiện chỉ gửi WS-UsernameToken:

- gSOAP client `OnvifControl` ([manager/Control/OnvifDeviceControl.cpp](../manager/Control/OnvifDeviceControl.cpp)):
  `connect()` (dòng 282) và `setCameraTimeManual()` (dòng 1261) chỉ đăng ký `soap_wsse`;
  `setCredentials()` (dòng 1323) chỉ gọi `soap_wsse_add_UsernameTokenDigest[_at]` + `soap_wsse_add_Timestamp`.
  Plugin `http_da` đã được build trong `onvifcore` (`src/gsoap/plugin/httpda.cpp`, có `WITH_OPENSSL`) nhưng không
  ai đăng ký, không ai xử lý `soap->error == 401`.
- Client SOAP tự viết `SoapUtil` ([src/Onvif/SoapUtil.cpp](../src/Onvif/SoapUtil.cpp)), dùng cho
  `/index/api/getStreamUrl`: tự dựng `<Security><UsernameToken>`, gặp status khác 200 là báo lỗi.

Hậu quả với camera để chế độ `digest`: `GetSystemDateAndTime` (không cần xác thực) vẫn chạy, nhưng
`getDeviceInformation()` trong `connect()` nhận 401, `connect()` thất bại, camera bị coi là ONVIF lỗi. Ngoài ra chuỗi
lỗi chứa "Unauthorized" nên `isAuthOrTimestampFault()` (dòng 1027) xoá clock offset như thể camera lệch giờ.

Mục tiêu:

1. Camera ở chế độ `digest` kết nối, lấy profile/URL, PTZ, encoder config, relay, imaging, recording/replay, đặt
   giờ… như camera ở chế độ `digest&ws-username token`.
2. Camera hiện chạy được **không đổi hành vi**, không thêm round trip: chưa từng nhận challenge Digest thì request
   giống hệt hôm nay.
3. Tự nhận biết, **không** thêm config hay cột DB theo camera.
4. Không bao giờ gửi mật khẩu dạng HTTP Basic qua HTTP thường.

Ngoài phạm vi: RTSP Digest (đã có ở `RtspPlayer`/`RtspPusher`/`BackchannelPusher`), HTTPS cho ONVIF, PSIA (có plan
riêng, dùng lại phần tính Digest ở P4 được).

## 2. Thiết kế

### 2.1 Chiến lược: phản ứng theo challenge, gửi cả hai

- Vẫn gửi WS-Security trên mọi request như hiện tại (camera `digest&ws` vẫn chạy y cũ).
- Khi một request nhận **401 có challenge `Digest`**: lưu challenge (realm, nonce, opaque, qop, algorithm) vào
  `http_da_info` của controller, gửi lại **đúng một lần** cùng request đó kèm `Authorization: Digest`.
- Từ đó, mọi request trong phiên đều gửi Digest chủ động (`http_da_restore` trước mỗi request, `nc` tăng dần), nên
  không tốn thêm round trip cho tới khi camera đổi nonce.
- Camera đổi nonce (401 + `stale=true` hoặc nonce mới): lưu lại challenge mới, gửi lại một lần.
- Gửi lại vẫn 401 thì dừng: đó là sai user/mật khẩu, báo lỗi xác thực, không lặp.

Lý do gửi cả hai thay vì "chọn một": không phải nhớ chế độ của từng camera, không cần migration, và camera đổi chế
độ giữa chừng vẫn chạy. Rủi ro camera `digest` từ chối request có WS-Security nằm ở mục 6.

### 2.2 gSOAP (`OnvifControl`)

Thành viên mới trong `OnvifDeviceControl.h`, đều được bảo vệ bởi `_soap_mtx` như `_m_soap`:

```cpp
struct http_da_info _da_info;   // challenge Digest đã học, chuỗi malloc bởi soap_strdup(NULL, ...)
bool _da_ready = false;         // true khi _da_info có nonce hợp lệ
```

Điểm sửa:

| Chỗ | Thay đổi |
|---|---|
| `connect()` dòng 282 | `soap_register_plugin(_m_soap, http_da)` cạnh `soap_wsse` |
| `disconnect()` (dòng ~357) | `if (_da_ready) http_da_release(_m_soap, &_da_info)` **trước** `soap_free` |
| `setCredentials()` | Cuối hàm: `if (_da_ready) http_da_restore(_m_soap, &_da_info)`. Bắt buộc gọi trước **mỗi** request vì `http_parse()` của gSOAP xoá `soap->userid/passwd/authrealm` sau mọi response (stdsoap2.cpp:8066). Hiện cả 46 lệnh đều gọi `setCredentials()` ngay trước lệnh proxy, nên đây là điểm móc duy nhất |
| Hàm mới `bool learnDigestChallenge(soap *s, http_da_info &info, bool &ready)` | Gọi khi `s->error == 401`. Nếu `s->authrealm` có và plugin bắt được nonce: release info cũ (nếu có), `http_da_save(s, &info, s->authrealm, user, pass)`, kiểm tra `info.nonce != NULL`, ép `qop` về `auth` (mục 2.4), `ready = true`, trả `true`. Challenge chỉ có `Basic` → trả `false` |
| Gửi lại một lần | Xem Q1 ở mục 8: helper chung cho cả 46 lệnh, hoặc chỉ `getDeviceInformation()` + làm mới trong `reportError()` |
| `reportError()` | `_m_soap->error == 401` và có challenge Digest thì **không** gọi `invalidateClockOffset()`: đó là xác thực HTTP, không phải lệch giờ |
| `setCameraTimeManual()` dòng 1261 | `local_soap` đăng ký thêm `http_da`, đi qua cùng helper gửi lại. Không dùng chung `_da_info` của `_m_soap` vì plugin mới có `nc` bắt đầu từ 0, nonce cũ + `nc` cũ bị camera coi là replay |

Helper gửi lại (nếu chốt Q1 = A):

```cpp
// Gọi một lệnh proxy có xác thực. Trả mã SOAP của lần gọi cuối.
int OnvifControl::callWithAuth(const std::function<int()> &call) {
    if (!setCredentials()) return SOAP_ERR;
    int ret = call();
    if (ret == 401 && learnDigestChallenge(_m_soap, _da_info, _da_ready)) {
        if (!setCredentials()) return SOAP_ERR;
        ret = call();
    }
    return ret;
}
```

Mỗi call site đổi từ

```cpp
if (!setCredentials()) { return false; }
if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) { reportError(); return false; }
```

sang

```cpp
if (callWithAuth([&] { return _proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse); })) { reportError(); return false; }
```

Lambda bắt `[&]` chạy đồng bộ trong hàm, không thoát khỏi phạm vi, nên không đụng luật "không bắt `this` trong
callback". Request object (`soap_new_…(_m_soap)`) và response được dùng lại cho lần gửi thứ hai; P1 phải kiểm tra
proxy gSOAP reset response trước khi parse (code sinh ra gọi `soap_default` / `soap_get`).

### 2.3 An toàn: không bao giờ rơi về Basic

Trong `http_da_post_header`, khi không có nonce plugin chuyển nguyên header `Authorization` cho `fposthdr` gốc, mà
stdsoap2 tự sinh `Authorization: Basic base64(user:pass)` khi `soap->userid`/`soap->passwd` được đặt
(stdsoap2.cpp:8696). Vì vậy:

- Chỉ đặt `soap->userid/passwd` qua `http_da_restore`, và chỉ khi `_da_ready` (đã có nonce).
- Camera chỉ có Basic: mặc định từ chối, log cảnh báo kèm tên config, không gửi mật khẩu. Đặt
  `onvif.allowHttpBasicAuth=1` (Q3) thì trả lời Basic và log cảnh báo mật khẩu đi dạng rõ.
- Log không in nonce/response/mật khẩu. Log mức Debug chỉ ghi "đã chuyển sang HTTP Digest, realm=…".

### 2.4 `qop=auth-int` và hook sửa timestamp

`installSoapHook()` (dòng 1183) sửa `wsu:Created/Expires` trong `fsend`, **sau** khi `http_da` đã băm body ở
`fpreparesend`. Với `qop=auth-int` (băm cả body) thì digest sẽ sai mỗi khi hook bật. Plugin chọn `auth-int` nếu
camera chào `qop="auth,auth-int"`. Cách xử lý: sau `http_da_save`, nếu `info.qop` chứa `auth` thì thay bằng `"auth"`
(free + `soap_strdup(NULL, "auth")`). Camera chỉ chấp nhận `auth-int` (hiếm) thì vẫn dùng `auth-int`, nhưng tắt hook
trên context đó; nếu lệch giờ ≥ 5 s thì WS-Security có thể hết hạn, mà camera ở chế độ `digest` vốn bỏ qua WS-Security
nên không ảnh hưởng.

### 2.5 `SoapUtil` (`/index/api/getStreamUrl`)

- `sendSoapRequest(url, action, body, cb, timeout)` thêm `user_name`, `pwd` (mặc định rỗng). Khi `parser.status()`
  là `401`, có `WWW-Authenticate: Digest` và user không rỗng: tính header, gửi lại **một lần** bằng
  `HttpRequester` mới, cùng body, thêm `Authorization`. Lần hai vẫn 401 thì trả lỗi như hiện tại.
- Hàm tĩnh mới `SoapUtil::makeDigestAuthorization(challenge, method, uri, user, pwd, nc, cnonce)` theo RFC 2617
  (MD5, qop `auth` hoặc không qop, có `opaque`), dùng `toolkit::MD5`. Hàm cần cho code thật, đồng thời test được
  trực tiếp bằng vector RFC.
- Năm hàm `sendGet*` truyền `user_name/pwd` xuống. Callback giữ nguyên chạy trên poller của `HttpRequester`.

### 2.6 Không đổi

- Không config mới, không migration, không đổi API REST hay C API.
- `OnvifControl` vẫn chạy trên `BlockingExecutors::device()` qua `_device_queue`; không thêm thread.
- Camera `digest&ws` không bao giờ nhận 401, nên `_da_ready` luôn false và request y hệt hôm nay.

## 3. Các bước

| Bước | Nội dung | Xong khi |
|---|---|---|
| P0 | Thêm `--auth {wsse,digest,both}` vào `tests/integration/stress_vms/onvif_mock.py` (mặc định `both` = hành vi cũ). `digest`: thiếu/sai `Authorization: Digest` → 401 + challenge (`qop="auth"`, `opaque`, nonce xoay theo `--nonce-ttl-sec`, nonce cũ → `stale=true`); `wsse` chỉ kiểm WS-Security | Chạy server hiện tại với mock `--auth digest` tái hiện lỗi connect |
| P1 | `OnvifControl`: đăng ký `http_da`, `_da_info/_da_ready`, `learnDigestChallenge`, restore trong `setCredentials`, release trong `disconnect`, ép `qop=auth`, `reportError` không xoá clock offset khi 401 Digest | Build `MediaServer` đạt |
| P2 | Gửi lại một lần theo Q1 (helper `callWithAuth` cho 46 lệnh, hoặc phương án tối giản) + `setCameraTimeManual` | Mock `--auth digest`: connect, profile, PTZ, set encoder, đặt giờ đạt; mock `--auth wsse`/`both`: số request mỗi thao tác không đổi so với trước |
| P3 | Phân loại lỗi: lần gửi lại vẫn 401 → log "sai user/mật khẩu (HTTP Digest)"; challenge chỉ Basic → log riêng. Nếu plan PSIA đã có `AUTH_FAILED` thì map vào đó | Log đúng với mock khi đổi mật khẩu |
| P4 | `SoapUtil`: `makeDigestAuthorization` + gửi lại trong `sendSoapRequest`, truyền user/pwd | `/index/api/getStreamUrl` với mock `--auth digest` trả URL |
| P5 | Unit test (mục 4), cập nhật `docs/` mục ONVIF (thêm đoạn về hai chế độ xác thực) | ctest unit toàn bộ đạt |
| P6 | Chạy thật: camera Hikvision (hoặc hãng có tuỳ chọn) lần lượt ở `digest` và `digest&ws-username token` | Bảng kết quả ở mục 9 |

## 4. Kiểm thử

Unit (thêm vào `tests/unit/onvif/`, target `unit_xml` hoặc target mới link `onvifcore`):

1. `makeDigestAuthorization` với vector RFC 2617 §3.5 (Mufasa / `testrealm@host.com` / `Circle Of Life`, qop `auth`,
   `nc=00000001`, `cnonce=0a4f113b`) → `response="6629fae49393a05397450978507c4ef1"`; thêm case không qop và có
   `opaque`.
2. Parse challenge: thứ tự tham số khác nhau, có/không dấu nháy, `algorithm=MD5`, `stale=true`, nhiều header
   `WWW-Authenticate` (Basic + Digest) → chọn Digest.
3. gSOAP end-to-end trong process: một `TcpServer` nhỏ trên 127.0.0.1 giả `device_service` yêu cầu Digest; `OnvifControl`
   (hoặc một `soap` + `DeviceBindingProxy`) gọi `GetDeviceInformation` → đúng 2 request ở lần đầu, 1 request ở lần
   sau, nonce xoay → 2 request, mật khẩu sai → dừng ở 2 request, server chỉ chào Basic → **không** có header
   `Authorization: Basic` nào được gửi.

Tích hợp: mock P0 ở cả ba chế độ, đếm request theo thao tác để chứng minh camera `wsse/both` không tốn thêm round trip.

## 5. Ảnh hưởng hiệu năng

- Camera `digest&ws`: không đổi.
- Camera `digest`: mỗi lần `connect()` thêm 1 request (401 đầu tiên), mỗi lần camera đổi nonce thêm 1 request. Tính
  MD5 không đáng kể.

## 6. Rủi ro

| Rủi ro | Xử lý |
|---|---|
| Camera chế độ `digest` từ chối request có WS-Security (`mustUnderstand="1"`) thay vì bỏ qua | Kiểm ở P6. Nếu gặp: khi `_da_ready` và lỗi là SOAP fault về Security, bỏ `soap_wsse_add_*` cho phiên đó (thêm cờ `_wsse_off`) |
| Camera khoá tài khoản sau N lần sai (Hikvision mặc định khoá IP) | Mỗi connect với mật khẩu sai nay thành 2 request lỗi thay vì 1. Giữ nguyên chu kỳ thử lại 60 s; ghi chú trong docs |
| `nc` bị camera kiểm chặt khi dùng chung nonce giữa hai context | Không chia sẻ `_da_info` giữa `_m_soap` và `local_soap` (mục 2.2) |
| Rò bộ nhớ `http_da_info` khi học lại challenge | `learnDigestChallenge` release info cũ trước khi save; `disconnect` release; test 3 chạy dưới ASAN |
| Response object dùng lại ở lần gửi thứ hai còn dữ liệu cũ | P1 đọc code proxy sinh ra; lần một bị 401 thì chưa parse body nên response vẫn ở trạng thái mặc định |
| Phát hiện nhầm "sai mật khẩu" thành lệch giờ (hiện tại) | `reportError` bỏ qua `invalidateClockOffset` khi là 401 Digest |

## 7. File dự kiến sửa

- `manager/Control/OnvifDeviceControl.h/.cpp`
- `src/Onvif/SoapUtil.h/.cpp`
- `tests/unit/onvif/test_onvif_digest.cpp` (mới), `tests/unit/CMakeLists.txt`
- `tests/integration/stress_vms/onvif_mock.py`
- `docs/` (mục ONVIF), plan này

## 8. Câu hỏi đã chốt (2026-10-03)

Đã chốt: **Q1 = A** (`callWithAuth` cho cả 46 lệnh), **Q2 = có**, **Q3 = cho phép Basic qua cờ config**
`onvif.allowHttpBasicAuth` (mặc định 0 = từ chối). Phần còn lại của mục này giữ lại để biết lý do.

- **Q1. Phạm vi gửi lại.**
  A (đề xuất): helper `callWithAuth` cho cả 46 lệnh. Diff cơ học nhưng đụng nhiều dòng; lệnh nào trúng lúc camera
  đổi nonce cũng tự hồi phục.
  B (tối giản): chỉ gửi lại ở `getDeviceInformation()` (lệnh xác thực đầu tiên của `connect()`/`probeConnection()`);
  các lệnh khác dựa vào Digest chủ động, gặp 401 thì `reportError()` học nonce mới và **lệnh đó thất bại một lần**,
  lệnh sau chạy. Diff nhỏ, nhưng PTZ/set encoder có thể hỏng ngẫu nhiên một lần mỗi chu kỳ nonce.
- **Q2. Có gửi lại trong `SoapUtil` (`/index/api/getStreamUrl`) đợt này không?** Đề xuất: có (P4), vì API này dùng
  khi thêm camera thủ công.
- **Q3. Camera chỉ chấp nhận Basic:** đề xuất từ chối và log rõ (mục 2.3). Phương án khác là cho phép Basic qua cờ
  config, không đề xuất vì lộ mật khẩu trên mạng.

## 9. Kết quả (2026-10-04, nhánh `feature/onvif-http-digest`)

Khác với thiết kế ở mục 2:
- Không dùng `soap->authrealm` để biết camera đòi gì, mà thêm một plugin gSOAP nhỏ (`httpAuthPlugin`) đăng ký sau
  `http_da` để ghi lại challenge Digest/Basic của từng response và realm của Digest. Lý do: stdsoap2 đặt
  `authrealm` cho cả Basic, và `http_da` bỏ qua challenge Digest nếu realm trùng với một header Basic đứng trước.
- `OnvifHttpAuth` (trong `OnvifDeviceControl.cpp`) gom phần lưu/nạp lại/giải phóng `http_da_info`; `callWithAuth`
  nạp lại credential trước mỗi lệnh (kể cả lệnh thứ hai sau một `setCredentials()`, như GetMoveOptions).
- `nc` của `http_da` có thể nhảy (vd 1 → 3), RFC 2617 chấp nhận vì chỉ yêu cầu tăng dần.

Kiểm thử:
- `unit_onvif_http_auth` 14 case (vector RFC 2617, chọn challenge, camera giả: không xác thực / chỉ Digest / nonce dùng
  1 lần / sai mật khẩu dừng ở 2 request / chỉ Basic từ chối mặc định / Basic khi bật cờ / `SoapUtil` Digest, không
  user, từ chối Basic); lặp 60 lần không lỗi. ctest unit toàn bộ 215/215.
- Chạy thật `run_stress.sh smoke steady`, 1 node, 30 camera, mock `--auth digest --nonce-uses 25` (nonce riêng từng
  thiết bị): ĐẠT, 60/60 luồng, `onvif_fail=0`, 88 lần 401 (30 challenge đầu + đổi nonce), **0 lệnh thất bại vì 401 sau
  khi gửi lại**. Đối chứng không xác thực: ĐẠT, cùng `faults=60`, 0 lần 401, GetProfiles/GetCapabilities bằng nhau.
- Lượt đầu mock dùng chung một nonce cho 30 camera nên 36 lệnh bị 401 lần hai (nonce bị camera khác làm hết hạn giữa
  lúc nhận challenge và gửi lại); đã sửa mock thành nonce theo từng thiết bị như camera thật.

Còn lại: chạy với camera thật đặt chế độ `digest` (mục 6, rủi ro camera từ chối WS-Security đi kèm), và
`/index/api/getStreamUrl` trên camera thật.
