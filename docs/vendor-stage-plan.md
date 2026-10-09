# Plan: tầng plugin hãng chạy trước giao thức điều khiển (loa trước, camera sau)

Ngày: 2026-10-08. Trạng thái: V1 (loa) đã làm; V2 (camera, no-op) đã làm; V3–V4 chưa làm.
Tiếp nối V0: loa có `manufacturer` rỗng tự nhận hãng (qua API hãng hoặc deviceInfo của giao thức).

## 1. Bối cảnh

Một thiết bị có hai tầng điều khiển:

| Tầng | Ở đâu | Làm gì |
|---|---|---|
| Plugin hãng | `ext-plugin/` (`IDevice`, `DeviceFactory`; hiện chỉ có `BoschIpSpeaker`) | Nhận diện hãng; chuẩn bị điều khiển (Bosch tạo tài khoản `speaker.onvifUsername` và bắt buộc dùng ONVIF); tính năng riêng của hãng (file âm thanh, volume) |
| Giao thức điều khiển | `manager/Control/` (`DeviceControl`: ONVIF/ISAPI/PSIA, `ProtocolLock`, `ReconnectPacer`) | PTZ, profile, giờ, backchannel; dùng chung cho camera và loa |

Plugin không thay thế giao thức. Nó **đứng trước** giao thức (quyết định tài khoản và giao thức được
phép dùng) và **bổ sung** tính năng bên cạnh.

Hiện trạng (sau V0):

- Logic nhận hãng (`_detecting`, `_brand`, probe API hãng, giãn chu kỳ, nhận tên từ deviceInfo) nằm
  riêng trong `SpeakerController`.
- Danh sách hãng có plugin là mảng cứng `kSupportedBrands` trong `SpeakerController.cpp`.
- Thông tin riêng của Bosch đang hard-code rải rác:
  - `requiresSeparateDeviceCredential()` trong `IDevice.h`;
  - `sharedDsp = brand == BOSCH`;
  - `getDeviceBrandName()`;
  - `_lock.setSetting("ONVIF")` trong `setupDeviceController()`.
- Camera chưa có plugin nào (`DeviceFactory::createCamera` đang bị comment) và chưa có bước hãng.

## 2. Mục tiêu

1. Mọi thiết bị (camera, loa) chạy cùng một chu kỳ:
   **xác định hãng → (có plugin) chuẩn bị điều khiển → connect/probe giao thức theo ràng buộc của
   plugin → tính năng hãng qua plugin.**
2. Thông tin về plugin (hãng nào, cho loại thiết bị nào, tên ghi ngược, có tài khoản riêng không,
   giao thức bắt buộc) nằm ở **một chỗ**: registry của `DeviceFactory`. Thêm hãng mới = thêm plugin +
   một dòng registry, không sửa controller.
3. Làm **loa trước** (đã có plugin và V0 để đối chiếu), giữ nguyên hành vi. **Camera sau**, và với camera
   chưa có plugin thì không đổi hành vi gì.

Giữ nguyên từ V0:

- Chỉ probe API hãng khi `manufacturer` rỗng.
- Hãng đã khai mà có plugin thì vào thẳng plugin.
- Hãng đã khai mà không có plugin thì chỉ probe giao thức.

## 3. Thiết kế

### 3.1 Registry plugin (`ext-plugin/`)

```cpp
// IDevice.h
struct VendorPluginInfo {
    DeviceType  type;
    DeviceBrand brand;
    std::string name;              // manufacturer ghi ngược khi nhận qua API hãng ("Bosch")
    bool ownControlAccount;        // plugin tạo tài khoản điều khiển riêng (speaker.onvifUsername)
    std::string requiredProtocol;  // giao thức plugin cần ("ONVIF"); "" = theo cấu hình
    bool sharedDsp;                // backchannel dùng chung DSP với file âm thanh của plugin
};

// DeviceFactory.h
static const std::vector<VendorPluginInfo> &plugins(DeviceType type);   // theo thứ tự probe
static bool findPlugin(DeviceType type, DeviceBrand brand, VendorPluginInfo &info);
```

- Thay `kSupportedBrands`, `requiresSeparateDeviceCredential()`, `getDeviceBrandName()` và
  `sharedDsp = BOSCH`.
- `getDeviceBrand(string)` vẫn ở `IDevice.h`: nó map tên cấu hình sang brand, không phụ thuộc loại thiết bị.
- `requiredProtocol` là chuỗi để `ext-plugin` không phải include `Control/DeviceControl.h`.
- `plugins()`/`findPlugin()` viết inline trong `DeviceFactory.h`. Lý do: `SpeakerStatistic` chỉ đọc
  registry mà nằm trong nhiều target test không link `ext-plugin`, giống `requiresSeparateDeviceCredential()`
  cũ vốn inline trong `IDevice.h`.

### 3.2 `VendorStage` (`manager/Control/VendorStage.{h,cpp}`)

Lớp dữ liệu thuần, không có thread hay timer, giống `ProtocolLock`/`ReconnectPacer`. Controller gọi
nó trên poller sở hữu.

```cpp
class VendorStage {
public:
    struct ProbeResult { DeviceBrand brand; bool rejected; bool answered; };
    typedef std::function<void(const ProbeResult &)> OnProbe;

    // Thiết bị `type` với manufacturer cấu hình: brand + plugin; rỗng = cần nhận hãng.
    void configure(DeviceType type, const std::string &manufacturer);

    bool detecting() const;
    DeviceBrand brand() const;
    bool hasPlugin() const;
    const VendorPluginInfo &plugin() const;

    // Bắt đầu một chu kỳ nhận hãng.
    void startCycle();
    // Kết quả probe API hãng; true = đã nhận ra hãng (brand()/plugin()/name()).
    bool onVendorProbe(const ProbeResult &result);
    // DeviceInfo của một lần connect giao thức thành công khi đang nhận hãng;
    // true = có tên để ghi ngược. Dù kết quả thế nào thì cũng thôi nhận hãng.
    bool identify(const std::string &manufacturer);
    // Chu kỳ kết thúc không có controller; `unreachable`: không gì trả lời.
    void onCycleFailed(bool unreachable);
    void resetBackoff();
    // 60 s, nhân đôi mỗi chu kỳ không gì trả lời khi đang nhận hãng, tối đa `max_interval_sec`.
    int cycleIntervalSec(int max_interval_sec) const;
    const std::string &name() const;  // tên đã nhận

    // Đăng nhập lần lượt API của các plugin loại `type` bằng tài khoản `cfg`; `cb` gọi một lần, từ thread bất kỳ.
    static void probe(DeviceType type, const IDeviceConfig &cfg, const OnProbe &cb);
};
```

Phần gắn với thread và vòng đời vẫn nằm trong controller, vì nó gắn với `_poller`, `_device_queue`,
`_ctrl_generation` và `_lock` của controller:

- quay về poller bằng `weak_ptr`;
- chuyển sang plugin qua `_device_queue` (đã có ở V0);
- tạo `_vendor_ctr`.

Controller chỉ hỏi `VendorStage` để quyết định.

### 3.3 Chu kỳ chung (controller)

```
onManager():
    nếu _probing hoặc chưa tới hạn (interval = stage.cycleIntervalSec): return
    nếu stage.detecting(): startDetect()  → VendorStage::probe → onVendorProbeDone
                                            → nhận ra: applyPlugin(), connect ở tick sau
                                            → không: runControlCycle()
    ngược lại: runControlCycle()          → connect / probe giao thức
connect thành công khi detecting: stage.identify(deviceInfo.manufacturer)
    → có plugin: chuyển sang plugin qua _device_queue
    → không plugin: chỉ ghi ngược tên
applyPlugin(): tạo plugin, provision tài khoản nếu ownControlAccount,
               _lock.setSetting(requiredProtocol) nếu khác rỗng
```

### 3.4 Camera (V2–V4)

- `CameraController` giữ một `VendorStage` với `DeviceType::Camera`.
- Khi `DeviceFactory::plugins(Camera)` rỗng (như hiện nay):
  - `configure` không bao giờ bật `detecting`;
  - `hasPlugin()` luôn false;
  - chu kỳ camera giống hệt hiện tại. Đây là điều kiện để merge V2.
- Loại trừ `GENERIC_RTSP_CAMERA` và camera chỉ có URL RTSP mà không có IP.
- Ghi ngược `manufacturer` cho camera là một quyết định riêng (câu hỏi mở 2). Webhook camera hiện đã
  gửi `deviceInfo.manufacturer`.
- Ranh giới với ISAPI: Hikvision ISAPI đang là một **giao thức**. Plugin Hikvision (nếu có) chỉ làm
  phần riêng của hãng, không làm lại PTZ/profile.
- Điểm mở rộng V4: plugin trả về `DeviceControl` của riêng nó (giao thức `VENDOR` trong `ProtocolLock`),
  dành cho hãng chỉ điều khiển được qua API riêng (SDK/CGI).

## 4. Các bước

| # | Việc | Phạm vi | Ảnh hưởng |
|---|---|---|---|
| V0 | Loa tự nhận hãng khi `manufacturer` rỗng | `Speaker/` | xong (chưa commit) |
| **V1.1** | Registry `VendorPluginInfo` + `DeviceFactory::plugins/findPlugin`; bỏ `requiresSeparateDeviceCredential`, `getDeviceBrandName`, `kSupportedBrands` | `ext-plugin/`, `Speaker/` | chỉ loa |
| **V1.2** | `VendorStage` + unit test riêng | `Control/` (mới) | chưa ai dùng |
| **V1.3** | `SpeakerController` dùng `VendorStage`: bỏ các biến `_brand/_detecting/_detect_*`; `setupDeviceController` đọc `ownControlAccount`/`requiredProtocol`; `sharedDsp` từ registry; `probe()` của discovery dùng registry; `SpeakerStatistic::saveOnvifCredentials` dùng `findPlugin` | `Speaker/` | chỉ loa, hành vi giữ nguyên |
| V2 | `CameraController` giữ `VendorStage(Camera)`, chưa có plugin → không đổi hành vi; unit test khẳng định không probe hãng | `Camera/` | xong: camera no-op, test `CameraProtocolTest.CameraWithoutPluginNeverDetectsItsBrand` |
| V3 | Plugin camera đầu tiên: `DeviceFactory::createCamera`, đăng ký registry, tính năng hãng qua `CameraController` | `ext-plugin/`, `Camera/` | khi có yêu cầu hãng cụ thể |
| V4 | (tuỳ chọn) Plugin cung cấp `DeviceControl` (`ControlProtocol::VENDOR`) | `Control/` | khi có hãng cần |

V1 là refactor trong phạm vi loa: toàn bộ `SpeakerProtocolTest` (24 test, kể cả 7 test của V0) phải đạt
mà **không sửa kỳ vọng**.

## 5. Kiểm thử

- `test_vendor_stage.cpp` (mới, `unit.vendor_stage_all`):
  - `configure` với hãng có plugin, không plugin, rỗng, và loại thiết bị không có plugin;
  - `onVendorProbe` khi nhận ra / bị 401 / không trả lời;
  - `identify` với tên rỗng, hãng không plugin, hãng có plugin;
  - backoff 60→600 và reset.
- `SpeakerProtocolTest.*`: không đổi kỳ vọng.
- ctest toàn bộ unit.
- V2: thêm test camera "không plugin → không probe hãng, chu kỳ 60 s".

## 6. Câu hỏi mở

1. Thứ tự probe khi có nhiều plugin cùng loại: theo thứ tự đăng ký, hay cho cấu hình
   (`speaker.vendor_probe_order`)? V1 dùng thứ tự đăng ký.
2. Camera có ghi ngược `manufacturer` như loa không, hay chỉ báo qua webhook như hiện tại?
3. Plugin camera đầu tiên là hãng nào? V3 chờ yêu cầu cụ thể.
