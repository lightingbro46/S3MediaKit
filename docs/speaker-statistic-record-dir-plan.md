# Plan: chuẩn hoá load `info.txt` thiết bị trong thư mục record

Nhánh: `feature/speaker-statistic-record-dir` (từ `develop` 9e8d04f7).

## Mục tiêu

```
<mp4_save_path>/<record.appName>/<camera_id>/info.txt   {"schema":"generic_rtsp_camera", ...}
<mp4_save_path>/<record.appName>/<speaker_id>/info.txt  {"schema":"generic_ip_speaker", ...}
<mp4_save_path>/<record.appName>/<push_id>/info.txt     {"schema":"generic_push_camera", ...} (đợt S2)
<mp4_save_path>/<speaker.audioFileDir>/...              (mặc định ./www/audioFile, không đổi)
```

- Mọi loại thiết bị lưu `info.txt` trong cùng thư mục record (mặc định `./www/record`).
- Trường `schema` quyết định loại thiết bị. **File không có `schema` = `generic_rtsp_camera`**
  (toàn bộ `info.txt` camera production hiện chưa có trường này).
- **Chỉ quét thư mục record.** Thư mục loa cũ (`<speakerSavePath>/<speakerDir>`, mặc định
  `./www/speaker`) không còn được đọc, không chuyển dữ liệu: loa chưa lên production.
- Bỏ config `speaker.speakerSavePath`, `speaker.speakerDir`.
- Một quy trình load chung để thêm loại thiết bị mới (camera push: drone, mobile video push)
  mà không sửa vòng quét hay `StatisticRecorder`.
- Mọi loại thiết bị ready cùng lúc sau `loadAll()` (đã chốt).

## Hiện trạng

| Thành phần | Hiện tại | Chỗ code |
|---|---|---|
| CameraStatistic | `absolutePath(record.appName, protocol.mp4_save_path)/<id>/info.txt` | `manager/Local/StatisticRecorder.cpp:13-17` |
| SpeakerStatistic | `absolutePath(speaker.speakerDir, speaker.speakerSavePath)/<id>/info.txt` | `manager/Local/StatisticRecorder.cpp:18-22` |
| Audio file | `absolutePath(speaker.audioFileDir, speaker.speakerSavePath)` | `AudioFileManager.cpp:110`, `SpeakerController.cpp:1047`, `BoschIpSpeaker.cpp:199,612` |
| Load khi khởi động | 2 task riêng trên `WorkThreadPool` (sai pool: đọc đĩa phải lên `BlockingExecutors`) | `server/Manager.cpp:64-75`, `CameraManager.cpp:316`, `SpeakerManager.cpp:115` |
| Map thống kê | `_cam_stats_map`, `_sp_stats_map` + hàm riêng cho từng loại | `StatisticRecorder.h` |

Helper camera và loa parse được JSON của nhau (cùng `id`/`name`/`vhost`): nếu không lọc theo
schema, camera nạp loa thành camera không stream.

`getRecorder()` (camera) có 17 chỗ gọi; ngoài các hàm `add*Threshold`/`saveVideoEncoderConfig`, phần
lớn chỉ đọc `getParams()`: `option.keepArchivedMin/MaxFor[Auto]`, `option.name`, `option.ip`,
`stream_map`, `sinfo_map`. Đây là phần camera push sẽ dùng chung (xem mục S2).

## Quy ước `info.txt`

```json
{ "schema": "generic_ip_speaker", "schemaVersion": 1, "id": "...", "vhost": "...", "name": "...", ... }
```

| Trường | Quy tắc |
|---|---|
| `schema` | Macro trong `manager/Common/DeviceSource.h` (`GENERIC_RTSP_CAMERA_SCHEMA`, `GENERIC_IP_SPEAKER_SCHEMA`, sau này `GENERIC_PUSH_CAMERA_SCHEMA`). Thiếu → `generic_rtsp_camera`. |
| `schemaVersion` | Thiếu → 1. Lớn hơn bản build hỗ trợ → không nạp, giữ file. |
| `id`, `vhost`, `name` | Header chung (đã có ở cả camera và loa). |

Một thư mục `record/<id>/` = một `info.txt` = một schema: `device_id` duy nhất trên node, với mọi loại.

## Thiết kế

Header mới `manager/Local/DeviceStatistic.h` (+ `.cpp`), Camera/Speaker đã include `Local/` sẵn.

```cpp
// Phần chung của mọi Statistic lưu ở record/<id>/info.txt.
class DeviceStatisticBase {
public:
    using Ptr = std::shared_ptr<DeviceStatisticBase>;
    virtual ~DeviceStatisticBase() = default;
    virtual std::string schema() = 0;
    virtual std::string deviceId() = 0;
};

// Manager nhận thiết bị đã lưu khi khởi động.
class ISavedDeviceLoader {
public:
    virtual ~ISavedDeviceLoader() = default;
    virtual void addSaved(const DeviceStatisticBase::Ptr &stats) = 0;
    virtual void onSavedLoaded() = 0;   // loadAll() xong: setReady(true)
};

// Một loại thiết bị: định dạng dữ liệu (create) tách khỏi manager quản lý (loader).
struct DeviceStatisticType {
    std::string schema;
    std::function<DeviceStatisticBase::Ptr(const std::string &device_dir)> create;
    std::weak_ptr<ISavedDeviceLoader> loader;
};

struct DeviceStatisticHeader {
    std::string schema;     // đã áp default generic_rtsp_camera
    int schema_version = 1;
    std::string id;
};
// false khi JSON hỏng.
bool parseDeviceStatisticHeader(const std::string &json_str, DeviceStatisticHeader &header);
```

Tách `create` khỏi `loader` vì định dạng dữ liệu và manager không phải 1-1: camera push dùng
chung phần ghi hình với camera RTSP nhưng do manager khác quản lý.

### `StatisticRecorder`

- `registerType(const DeviceStatisticType &)`: gọi tường minh trong `server/Manager.cpp`, không dùng
  `onceToken` tĩnh (tránh thứ tự khởi tạo static và linker bỏ TU).
- Gộp `_cam_stats_map` + `_sp_stats_map` → `unordered_map<string, DeviceStatisticBase::Ptr>`.
- `template <typename T> std::shared_ptr<T> get(const std::string &id)` → `dynamic_pointer_cast`.
  `getRecorder()` / `getSpeakerRecorder()` giữ tên, gọi `get<>` bên trong → 17 + 1 chỗ gọi không đổi.
- `create(schema, id)`: đã có thiết bị (trong map hoặc `info.txt` trên đĩa) mang schema khác →
  `ErrorL` + `nullptr`. Đây là chặn ghi đè khi trùng ID: `setup()` nạp lỗi thì đối tượng rỗng và
  `save()` đầu tiên sẽ đè file thiết bị kia. `getRecorder(id, true)` / `getSpeakerRecorder(id, true)`
  đi qua `create()`.
- `loadAll()`:

```
với mỗi thư mục con của _record_path:
  không có info.txt / rỗng          → WarnL + delete_file (giữ hành vi cũ)
  JSON hỏng                         → WarnL, giữ file
  schema chưa đăng ký               → WarnL, giữ file (file của bản build mới hơn)
  schemaVersion > hỗ trợ            → WarnL, giữ file
  id đã có trong map                → ErrorL, giữ bản đầu
  còn lại → type.create(dir) → map → type.loader.lock()->addSaved(stats)
  mỗi file một try/catch
xong: InfoL số file theo từng schema; onSavedLoaded() cho mọi loader đã đăng ký (kể cả 0 file)
```

### Manager

- `CameraManager`, `SpeakerManager` cài `ISavedDeviceLoader`:
  `addSaved` → `dynamic_pointer_cast` rồi gọi `addCamera(stats)` / `addSpeaker(stats)` hiện có;
  `onSavedLoaded` → `setReady(true)` (`SpeakerManager::setReady` vẫn private, gọi từ bên trong).
- Xoá `CameraManager::loadSavedCameraInfo`, `SpeakerManager::loadSavedSpeakerInfo`,
  `StatisticRecorder::loadSavedCameraStatistics` / `loadSavedSpeakerStatistics`
  (chỉ `server/Manager.cpp` gọi, không test nào dùng).

### Khởi động (`server/Manager.cpp::loadSavedDeviceInfo`)

```cpp
StatisticRecorder::Instance().registerType({GENERIC_RTSP_CAMERA_SCHEMA, createCameraStatistic, CameraManager::Instance().shared_from_this()});
StatisticRecorder::Instance().registerType({GENERIC_IP_SPEAKER_SCHEMA, createSpeakerStatistic, SpeakerManager::Instance().shared_from_this()});
EventPollerPool::Instance().getPoller()->doDelayTask(3000, []() {
    BlockingExecutors::storage()->async([]() { StatisticRecorder::Instance().loadAll(); });
    return 0;
});
```

Giữ độ trễ 3000 ms hiện có. Đọc đĩa chuyển từ `WorkThreadPool` sang `BlockingExecutors::storage()`.
Hệ quả đã chấp nhận: loa ready sau khi quét xong cả camera; trong khoảng đó lệnh cấu hình
`speakers` từ VMS bị từ chối "chưa sẵn sàng" lâu hơn một chút (`server/Manager.cpp:1461`).

## S1 — làm trên nhánh này

### S1.1 Bỏ `speakerSavePath` / `speakerDir`
- `src/Common/config.{h,cpp}`: xoá `kSpeakerSavePath`, `kSpeakerDir` và default.
- Gốc thư mục audio đổi sang `Protocol::kMP4SavePath` (default cũng `./www`, đường dẫn thực tế
  không đổi): `AudioFileManager.cpp:110`, `SpeakerController.cpp:1047`, `BoschIpSpeaker.cpp:199`, `:612`.
- `StatisticRecorder`: bỏ `_speaker_path`.
- `rg 'speakerSavePath|speakerDir|kSpeakerSavePath|kSpeakerDir'` phải về 0.

`audioFile/` nằm ngang hàng `record/`, nên `audioFile/info.txt` của `AudioFileManager` không bị quét.

### S1.2 `DeviceStatistic.{h,cpp}`
Lớp cơ sở, interface loader, `DeviceStatisticType`, `parseDeviceStatisticHeader`.
`manager/CMakeLists.txt` lấy nguồn bằng `file(GLOB MANAGER_SRC_LIST ...)`: phải chạy lại
`cmake -S . -B build` (và `build-test`), nếu không build cũ không biên dịch file mới và lỗi ở bước link.

### S1.3 Schema trong Statistic
- `CameraStatistic`, `SpeakerStatistic` thêm `std::string schema`; Imp kế thừa thêm
  `DeviceStatisticBase` (public), cài `schema()` / `deviceId()` dưới `_mtx`.
- Helper `getParamsString` ghi `schema` + `schemaVersion`.
- Helper `getParams` dùng `parseDeviceStatisticHeader`: camera nhận đúng schema của nó (thiếu =
  camera), loa chỉ nhận `generic_ip_speaker`; schema khác → `WarnL` + `false`.
- File camera cũ có `schema` từ lần `save()` kế tiếp, không chuyển đổi riêng.

### S1.4 `StatisticRecorder`
`registerType`, map chung, `get<T>`, `create`, `loadAll` như mục Thiết kế. Bỏ hai hàm load cũ.

### S1.5 Manager + khởi động
`CameraManager` / `SpeakerManager` cài `ISavedDeviceLoader`; xử lý `nullptr` từ `create()`:
`CameraManager::addCamera(DeviceTuple&, ...)` (`CameraManager.cpp:66`) và
`SpeakerManager::addSpeaker(DeviceTuple&, ...)` (`SpeakerManager.cpp:45`) → `return false`.
`server/HlsPlayback.cpp:496` đã kiểm `if (recorder)`. `server/Manager.cpp` như mục Khởi động.

### S1.6 Bộ quét record khác (chỉ kiểm, không sửa)
Thư mục loa chỉ có `info.txt`; đã đọc code, các bộ quét sau bỏ qua: `recreateTimeFile` /
`MultiTimeRebuilder` (không `.s3db`), retention `StorageManager.cpp:342` (cần `camera/stream`),
`removeExpiredSegment` (chỉ `.mp4`), `MotionBlockManager`, `TimeFileDirtyTracker`, tier
`collectDiskSegmentsFromRoot`. Hệ quả: `MotionBlockManager` có thêm một dòng `DebugL` mỗi loa.

### S1.7 Test
Unit (GTest) trong `tests/unit/manager/test_camera_statistic_sync.cpp` (đã link Camera + Speaker
Statistic, dùng thư mục tạm):

1. Header: thiếu `schema` → `generic_rtsp_camera`, thiếu `schemaVersion` → 1, JSON hỏng → false.
2. Helper: file loa → camera từ chối, loa nhận; ngược lại; schema lạ → cả hai từ chối.
3. Round-trip `getParamsString` → `getParams` giữ `schema`.
4. `loadAll` trên thư mục tạm có camera cũ (không schema), loa, schema lạ, file rỗng, trùng id:
   loader giả nhận đúng loại; file schema lạ còn nguyên; `onSavedLoaded` gọi đúng 1 lần mỗi loader.
   Cần `StatisticRecorder` dùng được với thư mục tạm: hiện là singleton với constructor private nhận
   `record_path` — chọn cách đổi ít nhất khi code (ví dụ để vòng quét là hàm nội bộ nhận
   `record_path` + bảng type), không tách file/API chỉ để test.
5. `create()` cho camera trên id đang là loa → `nullptr`, `info.txt` của loa không đổi.

Lệnh:

```bash
cmake --build build -j2
cmake --build build-test -j2 && ctest --test-dir build-test --output-on-failure -R unit.
```

Chạy tay: MediaServer mock, 1 camera + 1 loa; kiểm cả hai `info.txt` trong `./www/record` có
`schema`; restart → mỗi thiết bị vào đúng manager, cả hai ready; `./www/speaker` không còn được tạo.

### Thứ tự commit S1
1. S1.1–S1.5 (một commit: đổi thư mục mà thiếu lọc schema sẽ nạp chéo).
2. S1.7 test.

## S2 — camera push (drone, mobile video push), khi làm tính năng

Camera push dùng lại một phần trường của camera và có trường riêng (đã chốt), nên **không** dùng
nguyên `CameraStatisticImp`. Tách phần ghi hình dùng chung:

```
DeviceStatisticBase
 ├─ RecordingStatisticImp   (trừu tượng: phần ghi hình dùng chung)
 │   ├─ CameraStatisticImp      (+ ONVIF/PTZ, device_stats, video encoder config, CameraOption)
 │   └─ PushCameraStatisticImp  (+ trường riêng của push)
 └─ SpeakerStatisticImp
```

- **Phần chung** (lấy từ `CameraStatistic` hiện tại): `tuple`, `stream_map`, `sinfo_map`,
  `storage_map`, `bm`, `motion_stats`, `tier_storage_map`, `transport_stats`, `created_at/updated_at`;
  phần option ghi hình `name`, `keepArchivedMin/MaxFor[Auto]`, `recordRootPath`, `recordSchedules`;
  các hàm `addArchiveSize`, `addBookmarkCount`, `addStreamStatistic`, `addMotionKeepThreshold`,
  `addTierKeepThreshold`, `getStreamID`.
- **JSON**: phần chung giữ nguyên key hiện tại (kể cả các mục trong `addParams`), để `info.txt` camera
  production đọc được không cần chuyển đổi; các hàm `make*Json` / `get*` tĩnh trong
  `CameraStatistic.cpp` chuyển sang file của `RecordingStatisticImp`.
- **Chỗ gọi**: các cập nhật theo `device_id` trong `StatisticRecorder` (`addArchiveSize`,
  `addBookmarkCount`, `add*KeepThreshold`, listener `kBroadcast*KeepThreshold`) đổi sang
  `get<RecordingStatisticImp>`; các chỗ đọc `params.option.keepArchived*` / `stream_map` / `sinfo_map`
  (TierStorageManager, FFmpegSource, GlobalMonitor, HealthMonitor, Manager.cpp) đổi sang getter
  của phần chung. Chỉ chỗ thật sự cần ONVIF/`CameraOption` mới giữ `getRecorder()`.
- **Đăng ký**: macro `GENERIC_PUSH_CAMERA_SCHEMA` trong `DeviceSource.h`, `PushCameraManager` cài
  `ISavedDeviceLoader`, một dòng `registerType` trong `server/Manager.cpp`. Vòng quét không đổi.
- **Đã chốt (2026-10-29)**: một schema `generic_push_camera` + `PushCameraOption::originType` (`DeviceOriginType`:
  `mobile_device`, `flying_device`, lưu bằng `getOriginTypeString`),
  một `PushCameraManager`. Đợt này chỉ làm khung: `RecordingStatistic.{h,cpp}` (dữ liệu + `RecordingOption`
  mà `CameraOption` kế thừa + đọc/ghi JSON chung + `RecordingStatisticImp`), `PushCameraStatistic`,
  `PushCameraManager`, `registerType`; chưa có ingest/API tạo camera push.
- **Còn mở**: danh sách trường riêng của push (hiện chỉ `originType`, `enableActive`); sync ESC cho camera
  push (hiện không sync).

## Rủi ro / giới hạn

- **Rollback**: bản build cũ không biết `schema`, sẽ nạp `info.txt` của loa thành camera không
  stream (có thể đẩy lên ESC). Không quay về bản cũ trên node đã có loa, hoặc xoá thư mục loa trước.
  Quy ước `schema`/`schemaVersion` chỉ bảo vệ các bản build từ nay trở đi.
- Thư mục `./www/speaker` cũ trên máy dev/test không tự xoá; loa được tạo lại từ ESC/VMS.
- `dynamic_pointer_cast` ở mỗi `getRecorder()`: chi phí nhỏ so với `getParams()` (copy cả struct).
