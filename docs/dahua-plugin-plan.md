# Plan: plugin camera Dahua (kênh hãng)

Ngày: 2026-10-11. Trạng thái: **tạm dừng, chờ thêm tài liệu trước khi tích hợp.** Câu hỏi 1–4 đã tra tài liệu
công khai 2026-10-11 (mục 5) nhưng chưa đủ để code phần chuẩn bị ONVIF.

**Điều kiện bắt đầu** (cần đủ cả hai, từ tài liệu Dahua chính thức bản mới hoặc từ D0 trên camera thật):

1. Tên bảng cấu hình và cách đọc/ghi công tắc dịch vụ ONVIF của firmware đời mới (Access Platform > ONVIF), qua
   `configManager.cgi` hay `/RPC2`.
2. Tham số của `userManager.addONVIFUserInfoV20` / `modifyONVIFUserInfoV20` / `getONVIFUserInfoAllV20`, và mật
   khẩu gửi thô, hash hay mã hoá qua `system.multiSec`.

Nguồn nên tìm: "Dahua HTTP API" bản 3.x đầy đủ (qua đại lý/đối tác Dahua hoặc cổng hỗ trợ của Dahua), tài liệu
JSON-RPC (RPC2) chính thức nếu có, hoặc một phiên DevTools trên camera Dahua Web 5.0. Chưa đủ hai điều kiện trên thì
không làm bước nào, kể cả D1: tích hợp nửa chừng chỉ nhận hãng mà không chuẩn bị được ONVIF thì không có lợi cho
camera Dahua (ONVIF vẫn 401 khi mật khẩu web đã đổi, và Dahua không có ISAPI để AUTO rơi về).
Khuôn chung: [vendor-plugin-design.md](vendor-plugin-design.md). Plan này chỉ ghi phần riêng của Dahua.

## 1. Mục tiêu

Camera Dahua có hai kênh như Hikvision và Hanwha:

- **Giao thức điều khiển: ONVIF.** Dahua không nói ISAPI/PSIA. PTZ, profile, giờ, replay thẻ SD đều qua
  `OnvifControl` như hiện nay, không viết lại qua API Dahua.
- **Kênh hãng: Dahua HTTP API (CGI)**, `/cgi-bin/*.cgi`, xác thực Digest, trả về `key=value`. Plugin chỉ làm:
  1. nhận hãng + deviceInfo khi `manufacturer` rỗng;
  2. chuẩn bị ONVIF: bật ONVIF khi đang tắt (firmware mới hay tắt mặc định), và tạo hoặc cập nhật user ONVIF
     `camera.onvifUsername`: firmware đời mới có danh sách user ONVIF **riêng** (mục 5, câu hỏi 1).

Không dùng NetSDK (TCP 37777, thư viện nhị phân đóng): HTTP API đủ cho hai việc trên.

Không đổi `CameraController`, `VendorStage`, `SpeakerController`: Dahua chỉ là một dòng registry + một lớp plugin.
Nếu cần sửa controller thì đó là dấu hiệu thiếu đặc tính trong registry (vendor-plugin-design.md mục 5).

## 2. Thiết kế

### 2.1 Nhận hãng

`GET /cgi-bin/magicBox.cgi?action=getSystemInfo` (HTTP API for IPC v1.40 mục 9.8.7), trả về:

```
serialNumber=YZC0GZ05100020
deviceType=IPC-HF3300
hardwareVersion=1.00
```

Nhận diện chặt: HTTP 200, body là `key=value`, có cả `deviceType` lẫn `serialNumber`. Hanwha trả `Model`,
Hikvision trả XML, nên không lẫn. Firmware (`magicBox.cgi?action=getSoftwareVersion` → `version=...`) và hãng thật
(`magicBox.cgi?action=getVendor` → `vendor=Dahua`, OEM trả tên của họ) để sau D1. `VendorCamera` hiện chỉ đọc một
trang, và không đổi lớp chung chỉ vì việc này.

Parser `key=value`: dùng lại `HanwhaCamera::parseSunapiFields`, tách nó thành hàm tự do trong
`ext-plugin/VendorHttp.{h,cpp}` (ví dụ `parseKeyValueLines`). Không chép code.

Tên ghi ngược: `"Dahua"`. `getDeviceBrand()` nhận `dahua`, `imou` và `amcrest` (mục 5, câu hỏi 3).

### 2.2 Chuẩn bị ONVIF

Dahua có hai API web, cùng tài khoản web:

| API | Đường | Xác thực | Có gì cho ONVIF |
|---|---|---|---|
| HTTP API (CGI), có tài liệu | `/cgi-bin/*.cgi`, trả `key=value` | Digest MD5 mỗi request | `configManager.cgi?action=getConfig&name=UserGlobal` → `table.UserGlobal.OnvifLoginCheck=true|false` (bắt buộc đăng nhập ONVIF hay không). **Không** có user ONVIF, chưa thấy công tắc bật/tắt ONVIF |
| JSON-RPC (`/RPC2`), trang web dùng, không có tài liệu chính thức | `POST /RPC2_Login` rồi `POST /RPC2` với `session` | `global.login` hai bước: `MD5(user:random:MD5(user:realm:pass))` | `userManager.getONVIFUserInfoAllV20`, `addONVIFUserInfoV20`, `modifyONVIFUserInfoV20`, `getONVIFGroupInfoAllV20`; `configManager.getConfig/setConfig` (chắc có bảng chứa công tắc ONVIF) |

Hệ quả:

- Registry: `enablesOnvif = true`, `ownControlAccount = true` (giống Hikvision). Lúc khởi tạo camera, user ONVIF
  `admin` được tạo cùng mật khẩu với admin web, nhưng **không đổi theo** khi mật khẩu web đổi về sau. Vì vậy
  camera đã đổi mật khẩu web thì ONVIF đăng nhập bằng mật khẩu cấu hình sẽ 401, mà Dahua không có ISAPI để AUTO rơi về.
- Tạo/cập nhật user ONVIF chỉ qua RPC2. Plugin cần một client RPC2 nhỏ: login hai bước, giữ `session`, gọi method,
  `global.logout`. Tham số của các method `*ONVIFUserInfoV20` chưa có ở nguồn nào, D0 phải bắt từ trang web.
  Có thể mật khẩu phải gửi trong `system.multiSec` (RSA/AES, khoá lấy qua `Security.getEncryptInfo`), như
  `addUser` thường. Nếu đúng vậy thì D2b nặng hơn nhiều, cần quyết lại trước khi làm.
- **Không** đặt `UserGlobal.OnvifLoginCheck=false` để né tài khoản: tắt nó thì ONVIF mở không cần mật khẩu.
  Plugin chỉ đọc giá trị này để ghi log.
- Công tắc bật/tắt ONVIF của firmware đời mới (Web 5.0: Setting > Network > Access Platform > ONVIF) chưa có tên
  bảng cấu hình ở nguồn nào. D0 bắt `configManager.setConfig` của trang này; nếu bảng đó đọc/ghi được qua
  `configManager.cgi` thì dùng CGI, không thì dùng RPC2.

Ghi idempotent theo khuôn chung: đọc trước, chỉ ghi khi khác. 401 ở lần đọc đầu là `Unauthorized`. 401/403 khi
ghi là `Other`. Với RPC2, `global.login` bị từ chối cũng tính là một lần đăng nhập sai (mục 2.4).

### 2.3 Thứ tự dò

Registry camera: Hikvision → Hanwha → **Dahua**. Ba đường dẫn khác hẳn nhau (`/ISAPI`, `/stw-cgi`, `/cgi-bin/magicBox.cgi`)
nên thứ tự không ảnh hưởng độ đúng, chỉ ảnh hưởng thời gian dò. Dahua đứng cuối vì đến sau. Camera không phải
Dahua tốn thêm một request, tối đa 5 s nếu web server treo với đường lạ.

### 2.4 Rủi ro riêng của Dahua

- **Khoá tài khoản.** Dahua khoá đăng nhập sau khoảng 5 lần sai, trong khoảng 30 phút. `VendorStage` và
  `control_auth_retry_sec` (300 s) đã chặn dò lặp khi bị 401. Cần kiểm thêm: dò hãng **một** lần mỗi chu kỳ cộng
  dò ONVIF **không** vượt ngưỡng khoá khi sai mật khẩu.
- **Digest.** Tài liệu (v1.40, các bản 2.x) chỉ nói Digest MD5, `qop="auth"`, realm dạng `DH_<MAC>`. Không nguồn
  nào nói SHA-256. `VendorHttp` đã trả lời đúng kiểu này. D0 chỉ cần nhìn header `WWW-Authenticate` để chắc.
- **Basic bị tắt** trên firmware mới: không sao, `VendorHttp` ưu tiên Digest.
- **Khoá vì RPC2.** Client RPC2 đăng nhập thêm một lần mỗi lần chuẩn bị. Chỉ gọi RPC2 sau khi CGI (Digest) đã
  nhận tài khoản ở bước nhận hãng, để không cộng thêm lần sai khi mật khẩu sai.
- **HTTPS bắt buộc** trên vài firmware (HTTP chuyển hướng 302 sang 443): ngoài phạm vi. Ghi nhận `Other` như hãng khác.

## 3. Các bước

| # | Việc | File | Kiểm |
|---|---|---|---|
| D0 | Trên camera Dahua thật (Web 5.0) với DevTools: bắt request khi bật/tắt ONVIF ở Access Platform, khi thêm và đổi mật khẩu user ONVIF (`*ONVIFUserInfoV20`: tham số, mật khẩu thô/hash/`system.multiSec`); thử `configManager.cgi?action=getConfig&name=<bảng vừa thấy>`; xem `WWW-Authenticate`; `getVendor` | — | ghi vào mục 5 |
| D1 | `DeviceBrand::DAHUA`, `getDeviceBrand()` nhận dahua/imou/amcrest; tách `parseKeyValueLines` ra `VendorHttp`; `DahuaCamera : VendorCamera` chỉ nhận hãng (`getSystemInfo`); dòng registry (chưa `enablesOnvif`); `case` trong `DeviceFactory::createCamera()`; cấu hình lại CMake (GLOB) | `ext-plugin/IDevice.h`, `ext-plugin/DeviceFactory.{h,cpp}`, `ext-plugin/VendorHttp.{h,cpp}`, `ext-plugin/Hanwha/HanwhaCamera.cpp`, `ext-plugin/Dahua/` (mới) | unit parser, mock |
| D2a | `DahuaCamera : IOnvifSetup`, phần bật ONVIF (CGI nếu D0 thấy bảng đọc được qua CGI, không thì RPC2); `enablesOnvif = true` | `ext-plugin/Dahua/`, `ext-plugin/DeviceFactory.h` | unit, mock |
| D2b | Client RPC2 tối thiểu (`ext-plugin/Dahua/DahuaRpc.{h,cpp}`: login hai bước, session, call, logout, bất đồng bộ trên `VendorHttp`) + tạo/cập nhật user ONVIF `camera.onvifUsername`; `ownControlAccount = true`. Nếu D0 thấy cần `system.multiSec` thì dừng, báo lại trước khi làm | `ext-plugin/Dahua/`, `ext-plugin/VendorHttp.{h,cpp}` (POST JSON đã có) | unit hash login theo ví dụ, mock |
| D3 | Mock `dahua_mock.py` (CGI + RPC2) + test `VendorCameraMockTest.Dahua*` (bỏ qua khi thiếu `DAHUA_MOCK_ADDR`); chạy MediaServer thật với mock qua harness `e2e_real` | `tests/integration/stress_vms/`, `tests/unit/manager/test_vendor_stage.cpp` | 5 kịch bản như V4 |
| D4 | Docs: bảng plugin trong `vendor-plugin-design.md` mục 6, `camera-control-protocol.md`; `conf/config.ini` (comment `prepareOnvif`/`onvifUsername` có Dahua) | docs | — |
| D5 | Camera Dahua thật | — | mục 4 |

Tất cả các bước chờ điều kiện bắt đầu ở đầu file. Khi đủ, D1 làm trước (`magicBox.cgi?action=getSystemInfo` có trong
tài liệu từ 2013 tới nay), rồi D2a, D2b.
D1b (Digest SHA-256) bỏ: không nguồn nào thấy Dahua dùng.

## 4. Kiểm thử

- **Unit** (`unit.vendor_stage_all`):
  - `DahuaCameraTest.ParsesSystemInfo`: body thật của Dahua; body `key=value` của Hanwha (không có
    `deviceType`) → false; rỗng; HTML trang lỗi → false.
  - `VendorStageTest.ManufacturerNamesMapToBrands`: `"Dahua"`, `"Zhejiang Dahua Technology"` → `DAHUA`
    (sửa dòng `EXPECT_EQ(UNKNOWN, getDeviceBrand("Dahua"))` hiện có).
  - `VendorStageTest.RegistryDescribesTheCameraPlugins`: thứ tự Hikvision, Hanwha, Dahua và đặc tính của Dahua.
  - Các test parser ghi/đọc mục ONVIF (D2), theo body thật lấy ở D0.
- **Mock** `dahua_mock.py` (Digest MD5, `--onvif-off`, `--onvif-401`, `--state` ghi request và `onvif_user` như
  `psia_mock.py`), phục vụ cả `/RPC2_Login` + `/RPC2` (login hai bước, `*ONVIFUserInfoV20` theo tham số D0 bắt
  được); ONVIF mock chỉ nhận user ONVIF đã tạo.
- **MediaServer thật + mock** (harness `e2e_real`, `E2E_CAMERA_MANUFACTURER=` rỗng):
  1. ONVIF tắt → nhận Dahua, bật ONVIF (và tạo user nếu có), ONVIF thử đúng tài khoản;
  2. chạy lại → không ghi lại thứ đã đúng;
  3. `camera.prepareOnvif=0` → không ghi gì;
  4. khai `Dahua`, sai mật khẩu → API hãng 401, không dò giao thức, chờ 300 s;
  5. camera Hikvision/Hanwha mock vẫn nhận đúng hãng (Dahua đứng cuối không làm lệch kết quả).
- **Camera thật** (D5): firmware cũ ONVIF bật; firmware mới ONVIF tắt; user web không phải admin; đếm số lần
  đăng nhập sai trong một chu kỳ khi sai mật khẩu (không được khoá tài khoản); PTZ/preset/replay thẻ SD qua ONVIF
  sau khi plugin chuẩn bị.

## 5. Câu hỏi mở: đã tra tài liệu công khai 2026-10-11

1. **Dahua có danh sách user ONVIF riêng không? Có**, trên firmware đời mới. Web UI có tab "ONVIF User"
   (System > Account); JSON-RPC có `userManager.*ONVIFUserInfoV20`; Dahua IVSS chia user ONVIF thành 3 nhóm
   admin/user/operator. User ONVIF `admin` tạo lúc khởi tạo cùng mật khẩu, sau đó không đồng bộ với admin web
   (diễn đàn: đổi mật khẩu ONVIF đòi mật khẩu cũ, cách thường làm là tạo user ONVIF mới). Firmware cũ (HTTP API
   v1.40, 2014) chỉ có user web + `UserGlobal.OnvifLoginCheck`. → `ownControlAccount = true`, tạo user mới
   `camera.onvifUsername` thay vì sửa `admin`.
   Chưa biết: tham số của `addONVIFUserInfoV20` / `modifyONVIFUserInfoV20`, có phải mã hoá `system.multiSec`
   không (D0).
2. **Mục cấu hình ONVIF**: tài liệu chỉ có `configManager.cgi?action=getConfig&name=UserGlobal` →
   `table.UserGlobal.OnvifLoginCheck` (bật/tắt **xác thực** ONVIF, không phải bật/tắt dịch vụ). Công tắc dịch vụ
   ONVIF của firmware mới (Access Platform > ONVIF) không có trong tài liệu hay mã nguồn mở nào tìm được (D0).
3. **OEM: có, nhận cùng plugin.** OEM của Dahua dùng gần như nguyên firmware và web của Dahua (chỉ đổi logo,
   màu): Amcrest, IMOU (thương hiệu tiêu dùng của chính Dahua), Lorex (từng thuộc Dahua, bán 11/2022, thiết bị
   đời cũ vẫn trả model Dahua qua `getSystemInfo`), CP Plus, Montavue, Ameta, ... Plugin nhận theo API, không theo
   tên nên tự nhận các máy này. Tên ghi ngược vẫn `"Dahua"`; model thật nằm trong deviceInfo. `getDeviceBrand()`
   chỉ thêm alias chắc chắn là Dahua: `imou`, `amcrest`. Lorex đời mới (sau 2022) có thể không còn firmware Dahua,
   nên không thêm alias, để dò API quyết định.
4. **Digest: MD5**, `qop="auth"`, realm `DH_<MAC>` (HTTP API v1.40 mục 3.2 và các bản 2.x). Firmware mới bỏ
   Basic, chỉ còn Digest. Không nguồn nào nói SHA-256 → bỏ D1b. RPC2 dùng hash riêng (`global.login`, mục 2.2).

Nguồn: Dahua HTTP API for IPC v1.40 (bản PDF trên wiki.dno-it.ru), bản V2.841 trích dẫn trên pdfcoffee/Scribd
(mục 4.6.20 "Onvif Service Authorization"), tài liệu RPC2 dịch ngược `github.com/H3xano/dahua-rpc-api`
(`modules/10-security-users.md`, `01-authentication.md`), diễn đàn ipcamtalk (ONVIF User), IPVM Dahua OEM Directory.
Không có tài liệu chính thức nào của Dahua cho RPC2.
