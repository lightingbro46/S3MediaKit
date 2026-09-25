# Stress test VMS — 300 camera / 600 luồng

Dựng một hệ VMS tổng hợp ở quy mô thật trên máy local: N camera ONVIF ảo, 2N luồng
RTSP, chạy trên 1–2 node MediaServer, rồi ép qua các điều kiện khác nhau và kiểm
các bất biến sau mỗi phase.

Trọng tâm: **hồ sơ lập lịch lưu trữ**, **điều khiển ONVIF**, **lưu trữ đa tầng**,
**đồng bộ DB** và **quản lý dung lượng bằng StorageManager khi `ENABLE_TIER_STORAGE=OFF`**.

## Yêu cầu

- `MediaServer` bản **Debug** — chỉ bản Debug mới đọc `./api-config.json`
  (`ENABLE_DEBUG`, xem `server/Manager.cpp::loadServerConfigJson`).
- `ENABLE_SYNC_CHANGES=ON` nếu chạy kịch bản đồng bộ DB / failover.
- `ENABLE_MOTION=ON` nếu muốn các cohort `motion_night` / `lowres_motion` đúng nghĩa
  (không có macro này `StreamSink` chỉ ghi cảnh báo và không ghi hình).
- Một bản `ENABLE_TIER_STORAGE=OFF` riêng cho kịch bản `storage` (xem cuối file).
- `ffmpeg` (dựng file video mẫu), `python3` (chỉ thư viện chuẩn), `curl`.
- Các cổng trống: 18081–1808N (HTTP), 18551+ (RTSP), 18351+ (RTMP), 18554
  (nguồn RTSP), 18580 (ONVIF giả), 18590 (service API giả).

## Chạy

```bash
tests/integration/stress_vms/run_stress.sh /tmp/s3mk_stress full all
tests/integration/stress_vms/run_stress.sh /tmp/s3mk_stress mid onvif
tests/integration/stress_vms/run_stress.sh /tmp/s3mk_stress stop
```

| Tham số | Giá trị |
|---|---|
| `profile` | `smoke` (30 cam / 60 luồng), `mid` (120/240), `full` (300/600) |
| `scenario` | `all`, `ramp`, `steady`, `onvif`, `flap`, `schedule`, `retention`, `failover`, `storage`, `recover`, `soak`, `videorule` |
| `MEDIASERVER_DIR` | thư mục chứa binary (mặc định `<repo>/release/linux/Debug`) |
| `NODES` | số node (mặc định 2; `storage` và `videorule` luôn 1) |
| `PHASE_SEC` | thời lượng mỗi phase (mặc định 60/120/180 theo profile) |
| `CYCLES` | số chu kỳ `failover`↔`rebalance` của kịch bản `soak` (mặc định 4) |

## Hệ thống giả lập

Toàn bộ tải đến từ ba tiến trình Python, không cần camera thật:

| File | Vai trò |
|---|---|
| `rtsp_source.py` | Máy chủ RTSP phát lại một vòng H.264 đã đóng gói RTP sẵn. Mọi path đều phục vụ được nên 600 luồng chỉ tốn chi phí vá seq/timestamp. Chỉ RTP/AVP/TCP. Đã đo: **600 phiên ổn định ở ~32 Mbps, ~4400 pps, một tiến trình Python**. |
| `onvif_mock.py` | Thiết bị ONVIF giả cho N camera trên **một** cổng; danh tính thiết bị lấy từ địa chỉ loopback đích (127.1.x.y). Phủ đủ chuỗi `OnvifControl::connect()` và nhánh PTZ. Operation ngoài danh sách trả `ActionNotSupported` — giống camera đời thấp. |
| `mock_api.py` | Service API giả, trả 200 cho mọi hook, đếm hook theo path. |

Điều kiện lỗi được điều khiển qua hai file JSON, cả hai tiến trình đọc lại mỗi giây
nên đổi được giữa chừng mà không cần restart:

- `drop_spec.json` — `offline` (trả 404), `flaky` (ngắt phiên theo chu kỳ).
- `onvif_fault_spec.json` — `latency_ms`, `offline`, `fail_ops`, `ptz_omit_zoom_spaces`.

## Cohort camera

Tỉ trọng giữ nguyên ở mọi quy mô nên các lần chạy so sánh được với nhau.
Với `full` (300 camera):

| Cohort | % | ~Số cam | Hồ sơ lập lịch | Lưu trữ | Nguồn |
|---|---|---|---|---|---|
| `always` | 34 | 102 | RecordAlways 24/7 | giữ 1h–24h | ổn định |
| `office` | 18 | 54 | RecordAlways T2–T6 08–18, còn lại NoRecord | 2h–72h | ổn định |
| `motion_night` | 14 | 42 | 06–20 RecordAlways, đêm RecordOnlyMotion | 1h–48h | ổn định |
| `lowres_motion` | 10 | 30 | RecordLowResAndMotion cả tuần | 1h–12h | ổn định |
| `retention_auto` | 8 | 24 | RecordAlways | **Min/Max ở chế độ Auto** | ổn định |
| `long_retention` | 6 | 18 | RecordAlways, `notRecordSubStream` | 24h–30 ngày | ổn định |
| `no_record` | 4 | 12 | `enableRecording=false` | — | ổn định |
| `flaky` | 3 | 9 | RecordAlways | 1h–24h | **ngắt mỗi 45s** |
| `offline` | 3 | 9 | RecordAlways | 1h–24h | **404 + ONVIF chết** |

Ngoài ra cứ 25 camera có 1 camera bị đặt **ONVIF trả lời chậm 8s** để soi pool
`device()` và nhánh timeout.

### Ghi liên tục theo chế độ (nguồn: `manager/Camera/StreamSink.cpp`)

Kịch bản **không bơm sự kiện chuyển động**, nên mọi chế độ "theo sự kiện" nghĩa là
không ghi liên tục. `check_stress.py` dùng đúng bảng này để đối chiếu:

| RecordMode | Luồng chính | Luồng phụ |
|---|---|---|
| 0 NoRecord | không | không |
| 1 RecordLowResAndMotion | chỉ theo sự kiện | **ghi liên tục** |
| 2 RecordOnlyMotion | chỉ theo sự kiện | không bao giờ |
| 3 RecordAlways | ghi liên tục | ghi liên tục |

> Vì vậy **không** đặt `notRecordSubStream` cho cohort `lowres_motion`: luồng phụ mới
> là luồng ghi liên tục của chế độ đó, chặn nó thì cohort không ghi được gì.

## Các phase

| Phase | Thay đổi | Điều muốn quan sát |
|---|---|---|
| `ramp` | khởi động toàn bộ | thời gian đến khi mọi luồng lên; ONVIF connect chạy qua pool `device()` |
| `steady` | nền, có tải REST | mốc chuẩn để so sánh các phase sau |
| `onvif_storm` | **mọi** ONVIF trả lời chậm 1.5s | pool `device()` bão hoà mà **poller phải sạch** |
| `source_flap` | một nửa nguồn ngắt mỗi 20s | reconnect, liên tục của bản ghi, rò rỉ fd |
| `schedule_flip` | đảo hồ sơ `office` ↔ `always` | scheduler đổi chế độ ghi tại mốc giờ |
| `retention_drop` | ép `keepArchivedMaxFor=1h` | dọn dung lượng, dựng lại timefile, xoá bookmark |
| `failover` | node-a nhả hết camera | node-b nhận qua FAILOVER, DB ESC hội tụ |
| `rebalance` | trả quyền sở hữu về ban đầu | không kẹt assignment, không "database is locked" |
| `recover` | gỡ mọi lỗi tiêm vào | hệ quay lại mức `steady` |

Kịch bản `soak` lặp `failover`↔`rebalance` `CYCLES` lần (`failover_1`, `rebalance_1`,
`failover_2`, ...). Nó có để trả lời một câu hỏi cụ thể: fd có **rò** không. Một lần
failover/rebalance như trong `all` không đủ — fd tăng khi node nhận thêm camera là chuyện
đương nhiên. Chỉ khi fd tăng **đơn điệu theo số chu kỳ** thì mới là rò.

```bash
NODES=2 PHASE_SEC=120 CYCLES=4 \
    tests/integration/stress_vms/run_stress.sh /tmp/s3mk_soak mid soak
```

Lưu ý khi đọc: harness đặt `manager.failoverActiveDelaySec=15` (mặc định của server là
**1800**), nên camera bị chuyển đi được nhả sau ~15 s. Nếu đo trên cấu hình mặc định thì
node vừa nhả camera vẫn giữ chúng trong danh sách tới 30 phút — fd chưa về mức nền trong
khoảng đó **không** phải rò.

> **Các phase không cộng dồn.** Mỗi phase gọi `setup_stress.py phase <tên>`, và hàm đó
> dựng lại toàn bộ danh sách camera từ `SCHEDULES` gốc rồi mới áp thay đổi riêng của
> phase. Nghĩa là `retention_drop` hoàn tác hoán đổi lịch của `schedule_flip`, chứ không
> giữ lại. `check_stress.py` vì vậy chỉ kiểm theo lịch đã hoán đúng ở phase `schedule`.
> Muốn một thay đổi tồn tại qua nhiều phase thì phải đưa nó vào `build_cameras()`.

## Bất biến được kiểm

`check_stress.py` chạy sau mỗi phase và quyết định đạt/không đạt ở cuối:

- Không có `## signal:` (stack trace) trong log của bất kỳ node nào.
- `database is locked` = 0, `Failed to apply` = 0.
- **`/media/mserver/healthcheck` p95 ≤ 1000ms và max ≤ 5000ms.** Endpoint này chạy
  thẳng trên EventPoller; nó chậm nghĩa là có việc blocking nằm nhầm trên poller
  (xem `docs/threading-blocking-work.md`).
- **Độ trễ PTZ**: p95 ≤ 15 s và tỉ lệ timeout ≤ 25%. Healthcheck sạch **không** đủ để kết
  luận node khoẻ: khi vài camera ONVIF chậm lấp đầy pool `device()`, lệnh PTZ tắc hoàn toàn
  trong khi poller vẫn nhanh và các endpoint đọc-cache vẫn nhanh. Tỉ lệ timeout mới là tín
  hiệu — các lệnh PTZ *thành công* trong tình huống đó vẫn chỉ mất ~18 ms.
- ≥ 80% số luồng khả dụng còn phát ở cuối.
- Trạng thái ghi thực tế của từng luồng (`getMediaList.isRecordingMP4`) khớp hồ sơ
  lập lịch của cohort tại thời điểm kiểm (bỏ qua cohort `offline`/`flaky`).
- Với ≥ 2 node: `vms_resource`, `vms_kvpair`, `vms_resource_status`,
  `vms_resource_assignment` giống nhau giữa các node ở cuối.

Mỗi phase in thêm (không phải tiêu chí đạt/trượt): số file mp4 và dung lượng theo
node, số timefile, RSS/threads/fd/CPU, số lệnh ONVIF theo operation, độ trễ p50/p95/max
của từng nhóm API.

### Đọc số fd cho đúng

`resources.jsonl` lấy mẫu mỗi 5 s và phân loại fd theo đích của symlink
(`socket`/`file`/`eventfd`/`eventpoll`/`timerfd`/`pipe`/`anon`). Báo cáo in
**`fd đầu→cuối` của riêng phase** cộng với `fd đỉnh`.

Cột phải đọc là `fd đầu→cuối`. `fd đỉnh` chỉ nói lúc cao nhất đạt bao nhiêu, và trước đây
`check_stress.py` tính nó trên **cả lần chạy** chứ không cắt theo phase — một đỉnh tích luỹ
thì về bản chất không bao giờ giảm, nên đọc nó như "fd không được giải phóng" là sai. Đó
chính là chỗ làm quan sát fd trong `REPORT-2026-09-20.md` không kết luận được gì.

Muốn đo được việc **nhả** fd của camera đã chuyển đi thì phase phải đủ dài: writer timefile chỉ
được đóng trong chu kỳ bảo trì kho lưu trữ (timer 300 s), nên dùng `PHASE_SEC=330` trở lên.
`setup_stress.py` cũng hạ `record.timefileIdleCloseSec` xuống 30 s cho node của harness (mặc định
600 s thì một phase không bao giờ thấy writer idle). Xác nhận cơ chế đã chạy bằng log:

```bash
grep -h "idle time file writer" <work_dir>/node-*/log/*.log
```

Kết quả ghi ra `<work_dir>/report_<phase>.json` và `report_final.json`.

## Kịch bản `storage` — StorageManager khi `ENABLE_TIER_STORAGE=OFF`

Khi tắt tier storage, `StorageManager::start()` gọi `enforceStoragePolicy()` trong
timer 300s (`manager/Local/StorageManager.cpp:479`); khi bật thì `TierStorageManager`
lo phần này và `enforceStoragePolicy` **không** chạy. Vì vậy kịch bản này cần một bản
build riêng:

```bash
cmake -S . -B build-tier-off -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TIER_STORAGE=OFF -DENABLE_AWS_SDK=OFF -DENABLE_SYNC_CHANGES=ON
cmake --build build-tier-off --target MediaServer -j2   # -j2: máy 8GB RAM, -j6 bị OOM khi link

# build ghi đè release/linux/Debug, nên tách binary ra trước khi chạy
mkdir -p /tmp/bin-tier-off && cp -a release/linux/Debug/MediaServer \
    release/linux/Debug/default.pem release/linux/Debug/*.so* /tmp/bin-tier-off/

MEDIASERVER_DIR=/tmp/bin-tier-off PHASE_SEC=420 \
    tests/integration/stress_vms/run_stress.sh /tmp/s3mk_storage full storage
```

Kịch bản chạy `steady` rồi `retention_drop` và **chờ tối thiểu 2 chu kỳ timer 300s**,
vì `enforceStoragePolicy` chỉ chạy theo chu kỳ đó. Cần quan sát:

- Số file mp4 và dung lượng giảm sau khi `keepArchivedMaxFor` bị siết xuống 1h.
- Số timefile được dựng lại (`recreateTimeFile`) khớp số camera có bản ghi.
- `cohort retention_auto` (Min/Max Auto) không bị xoá quá tay: khi `keepArchivedMaxForAuto`
  bật, mốc lấy từ `archiveStartTime` trong thống kê camera, không có thì mặc định 6 tháng.
- Vòng lặp `keep_percent` giảm dần 5% chỉ chạy khi đĩa thật sự vượt
  `storage.limitPercentUsage`. Muốn ép nhánh này phải hạ ngưỡng xuống dưới mức dùng
  thật của đĩa:

```bash
STORAGE_ARGS="--limit-percent 5 --remove-percent 10" \
    tests/integration/stress_vms/run_stress.sh /tmp/s3mk_storage full storage
```

> `estimateSpaceToReclaim()` đọc mức dùng của **mount point thật** chứa thư mục ghi,
> không phải dung lượng riêng của thư mục. Hạ `limitPercentUsage` xuống dưới mức dùng
> hiện tại của đĩa là cách duy nhất để kích hoạt nhánh thu hồi mà không phải đổ đầy đĩa.

## Kịch bản `videorule` — chạy thật video config rule không cần camera

Checklist "Kiểm tra cuối" của [docs/video-config-rule-fix-plan.md](../../../docs/video-config-rule-fix-plan.md),
mỗi mục trên một camera riêng, chạy song song (`check_video_rule.py`):

| Camera | Mục |
|---|---|
| `cam-0000` | áp rule 120 s → webhook `SUCCESS` (đúng `Idempotency-Key`) và tra cứu ra `SUCCESS`; hết hạn → về baseline |
| `cam-0001` | gửi lại cùng cấu hình giữa chừng → gia hạn, không đẩy lại cấu hình; về baseline ở hạn mới |
| `cam-0002` | rút mạng (fault `offline`) quanh lúc hết hạn → cắm lại thì baseline được đẩy lại |
| `cam-0003` | `onvifSetVideoConfigs` khi rule chạy → `deferred=true`, camera giữ rule; hết hạn → về cấu hình người dùng |

Sau đó node restart với `manager.enableAuthorize=1` (phase `videorule_auth`) để kiểm thiếu/sai
`X-Secret-Key` → 401 và kết quả vẫn tra cứu được sau restart. Cả lượt mất ~10 phút:

```bash
tests/integration/stress_vms/run_stress.sh /tmp/s3mk_vr smoke videorule
```

Mock phục vụ được kịch bản này nhờ: `onvif_mock.py` trả đúng schema Media2 (`tr2:`) và lưu cấu
hình encoder theo thiết bị (`GET /_mock/encoders`), `--stream-base` làm `GetStreamUri` trả đúng
URL luồng của camera (server nhận ra profile chính bằng cách so hai URL này), và `mock_api.py`
giữ body webhook `action-callback` (`GET /_mock/hooks`). Kết quả từng mục ghi ở
`<work>/video_rule_rule.json` và `video_rule_auth.json`.

## Kết quả và lỗi đã phát hiện

Báo cáo đầy đủ của lần chạy 2026-09-20 (300 camera / 600 luồng, 2 node + 1 node tier-off):
[`REPORT-2026-09-20.md`](REPORT-2026-09-20.md) — 7 vấn đề, trong đó có lỗi segfault dưới đây.


**Segfault khi camera ONVIF không khai báo zoom space** —
`manager/Control/OnvifDeviceControl.cpp:401`:

```cpp
if (...->Spaces->AbsolutePanTiltPositionSpace.size() > 0) {
    ...
    _ptzProfile.absMinZoom = ...->Spaces->AbsoluteZoomPositionSpace[0]->XRange->Min;  // không kiểm size
```

Điều kiện `if` chỉ kiểm `AbsolutePanTiltPositionSpace` nhưng thân lệnh lại truy cập
`AbsoluteZoomPositionSpace[0]`. Camera chỉ có pan/tilt mà không có zoom là hợp lệ theo
ONVIF, và khi đó **cả tiến trình MediaServer chết**. Cùng lỗi ở dòng 413/414
(`ContinuousZoomVelocitySpace`) và 424/425 (`RelativeZoomTranslationSpace`).

Tái hiện: đặt `ptz_omit_zoom_spaces` cho một thiết bị trong `onvif_fault_spec.json`:

```json
{"devices": {"127.1.1.5": {"ptz_omit_zoom_spaces": true}}}
```

Mặc định mock khai báo đủ cả 6 space nên kịch bản chạy được; bật cờ này chỉ để tái hiện lỗi.

**Sửa tay file đó chỉ sống được một phase.** `setup_stress.py phase <tên>` gọi lại
`write_specs()`, mà hàm này dựng lại `onvif_fault_spec.json` từ đầu. Muốn cờ sống qua mọi phase
thì dùng biến môi trường — `run_stress.sh` truyền nó xuống mọi lần gọi `setup_stress.py`:

```bash
STRESS_PTZ_OMIT_ZOOM=10 run_stress.sh /tmp/s3mk_ptz mid onvif    # cứ 10 camera thì 1 camera
STRESS_PTZ_OMIT_ZOOM=all run_stress.sh /tmp/s3mk_ptz smoke steady # toàn bộ thiết bị
```

Cờ được áp **sau** các nhánh riêng của từng phase, vì `onvif_storm` và `recover` ghi đè
`default`/`devices` của spec.

## File

| File | Vai trò |
|---|---|
| `run_stress.sh` | Điều phối: dựng cấu hình, bật dịch vụ giả, chạy từng phase, lấy mẫu tài nguyên |
| `setup_stress.py` | Sinh `config.ini`, `api-config.json`, spec lỗi; định nghĩa cohort và phase |
| `rtsp_source.py` | Nguồn RTSP tổng hợp |
| `onvif_mock.py` | Thiết bị ONVIF giả |
| `mock_api.py` | Service API giả cho hook |
| `load_driver.py` | Sinh tải REST (PTZ / storage) và đo độ trễ, gồm probe healthcheck |
| `check_stress.py` | Thu thập bằng chứng, đối chiếu lịch ghi, so DB giữa node, kết luận đạt/trượt |
| `check_video_rule.py` | Checklist chạy thật của video config rule (kịch bản `videorule`) |
