# Plan sửa overlay + auto profile replay

Nhánh `feature/overlay-with-auto-profile`. Plan lập ngày 2026-09-25 từ đợt review phần code
chưa commit (overlay privacy cho live/replay + replay ghép MainStream/SubStream).

## 1. Trạng thái khi dừng (cập nhật 2026-09-25, sau khi làm P1.2–P5)

- Code gốc nằm trong commit `tmp` cũ. Commit này đã được reset và đưa vào stash
  `overlay-auto-profile-tmp-2026-09-25` (SHA `77721b80`), rồi apply lại trên develop `d3252737`.
  **Toàn bộ vẫn chưa commit**: phần từ stash đang ở trạng thái staged, các sửa sau đó chưa stage.
  Stash vẫn còn giữ để dự phòng; drop khi đã commit xong.
- Đã sửa sau khi apply: `server/FFmpegSource.cpp` dùng `notifyStartOnce`; P0, P1, P2.1–P2.3,
  P3.1–P3.2, P4.1–P4.2, P5.1–P5.3, P5.5 và một phần P5.4 (xem mục 3 và mục 6).
- File mới **chưa track**, phải `git add` khi commit: `tests/unit/record/test_merged_mp4_reader.cpp`.
- Build/test: thư mục `build-test/` (Debug, `ENABLE_TESTS=ON`, `ENABLE_AWS_SDK=ON`).
  `3rdpart/{S3ToolKit,jsoncpp,jwt-cpp,media-server,onvif,aws-sdk}` là symlink sang repo chính,
  git hiện `T` và `??`. **Không `git add` các path này**, chỉ stage file cụ thể.
- Kết quả lần chạy cuối: `ctest -j2` 240/240 pass (4 test MinIO bị skip), đã gồm
  `unit.replay_all` (13 test) và `unit.http_all` (24 test).
- **Chưa chạy thử trên MediaServer thật** (camera có Main/Sub, overlay bật).

```bash
cmake --build build-test -j2
(cd build-test && ctest -j2 --output-on-failure)
release/linux/Debug/unit_replay && release/linux/Debug/unit_http
```

## 2. Quyết định đã chốt

- **P0 (2026-09-25):** đã bỏ `Broadcast::supportsViewOverlayOutput`, playback mở cho mọi giao thức.
  RTSP/RTMP/WebRTC phát source gốc, không có overlay. HTTP fMP4/TS/FLV và HLS render overlay qua
  derived transcode. `403` chỉ còn khi `ViewOverlayPolicy::isUnavailable()` (JWT không hợp lệ,
  camera replay không còn cấu hình). **Không thêm lại cơ chế chặn theo giao thức.**

## 3. Tiến độ

| Phase | Nội dung | Trạng thái |
|---|---|---|
| P0 | Chốt chính sách theo giao thức | ✅ Xong |
| P1.1 | Bỏ cơ chế chặn theo giao thức ở `on_play` / HttpSession | ✅ Xong cùng P0 |
| P1.2 | Derived stream phải khớp policy của người xem | ✅ Xong: suffix `.transcode.<md5 tham số>.<tag policy>`, HLS so tag với policy người xem |
| P2 | MergedMP4Reader: DTS, thread-safety, parse start time | ✅ Xong 1–3 (`ReplayClipWindow`, lock `_mtx`, `parseReplayStartTime`); P2.4 chưa kiểm chứng |
| P3 | Planner: chấp nhận gap nhỏ, giảm đổi Main/Sub liên tục | ✅ Xong 1–2 (`record.replayMaxGapSec`=2, `record.replayMinSwitchSec`=5); P3.3 chưa làm |
| P4 | Hiệu năng mở replay và kiểm tra HLS | ✅ Xong: probe audio 1 file/stream, mở replay trên pool "replay open", cache policy HLS theo session (P4.3) |
| P5 | Dọn dẹp và đăng ký test | ✅ 1, 2, 3, 5 xong; P5.4 bỏ code chết xong, pacing theo đồng hồ thực chưa làm |

Ghi chú P4.3: làm theo `session_id`. Request có `token` (playlist) lấy policy rồi lưu theo
`camera scope + session_id`; segment/init/key (chỉ mang `session_id`) dùng lại policy đó. Việc này
sửa luôn lỗi 403 segment của người được miễn mask/watermark, vì URI segment không mang JWT (xem 6.1).

Ghi chú P5.5: Motion MJPEG phát source gốc như RTSP/RTMP/WebRTC (không còn 503).

## 4. Chi tiết từng phase

### P1.2 — Không cho dùng derived stream của policy khác

**Vấn đề.** `HttpSession::isTrustedViewOverlayDerivedSource` (`src/Http/HttpSession.cpp:565`) chỉ
kiểm tra `getOriginType() == MediaOriginType::transcode`. Nhánh HLS (`checkLiveStreamHls`, khoảng
dòng 862–890) tin mọi stream có `.transcode.` nếu origin là transcode. Hậu tố stream là
`TRANSCODE_SUFFIX + MD5(key)` (dòng 492), trong đó key gồm codec, kích thước, bitrate, fps, gop,
template, vùng mask, username, tên camera và hai cờ watermark/mask. Người xem biết hoặc đoán được
các giá trị này, nên người bắt buộc privacy mask có thể tự tính hậu tố derived stream chỉ có
watermark của người khác và xem stream đó.

**Hướng sửa.**
1. Tách phần dựng `key` / `stream_suffix` trong `applyViewOverlayPolicy` ra một hàm dùng chung
   (vd. `static string makeViewOverlaySuffix(const ViewOverlayPolicy &, const TranscodeRequest &)`).
2. Ở `checkLiveStreamHls`, khi `has_transcode_suffix`: tính suffix theo policy hiện tại của người
   xem (policy đã emit ở dòng 862, cộng tham số transcode của request). Chỉ tin khi
   `_media_info.stream` kết thúc bằng đúng suffix đó **và** origin là `transcode`.
   Nếu không khớp thì trả 403.
3. Cách khác, bền hơn nếu tham số transcode trong URL segment không đầy đủ: lưu policy key vào
   `TranscodeProcessor::Config`, cho derived source trả lại key đó, rồi so sánh với key tính từ
   policy hiện tại.
   Trước khi chọn, kiểm tra xem URL segment HLS còn giữ query `transcode*` hay không.

**Test.** Mở rộng `tests/unit/http/test_hls_overlay_guard.cpp`:
- suffix khớp → tin;
- suffix của policy khác (thiếu mask) → không tin;
- origin không phải transcode → không tin.

### P2 — MergedMP4Reader (`src/Record/MergedMP4Reader.cpp`)

1. **DTS chạy lùi khi đổi clip** (`openTimelineClip` dòng 206, seek ở dòng 232–233;
   `readTimelineTick` dòng 306).
   - Nguyên nhân: `mov_reader_seek` → `mov_stss_seek` chọn keyframe *gần nhất*, có thể nằm trước
     `clip_start`. Frame có `mapped_dts < _timeline_clip_start_ms` vẫn được đẩy vào sau clip trước,
     nên DTS lùi lại ở mỗi lần chuyển Main↔Sub giữa file.
   - Sửa: khi mở clip, bỏ mọi frame có `mapped_dts < _timeline_clip_start_ms`, giữ `_timeline_priming`
     để chờ IDR ≥ clip_start. Chấp nhận một khoảng hở nhỏ thay vì chồng lấn.
   - Sửa thêm: dùng giá trị trả về của `MP4Demuxer::seekTo` (timestamp thực tế) để log hoặc kiểm tra.
2. **Tranh chấp dữ liệu giữa hai luồng.**
   - `seekTo(uint32_t)` (dòng 508) có lock `_mtx`, còn `readTimelineTick` chạy trên `_poller` thì
     không; `pause`/`speed` cũng không lock.
   - Sửa: đưa `seekTo`/`pause`/`speed` về `_poller` qua `async` (giữ `weak_ptr`, không bắt `this`),
     hoặc lock `_mtx` trong `readTimelineTick` như `MP4Reader`.
3. **Parse giờ bắt đầu dễ vỡ**: `startTimelineReplay` (dòng 381) gọi `stoull(parts.back())` trên
   `"<ts>.replay.<md5>"`, chỉ đúng vì `stoull` dừng ở dấu `.`.
   Sửa: truyền start time riêng (vd. qua `selection_params`, hoặc cắt phần `.replay.` trước khi parse).
4. **Chưa kiểm chứng:** stream chỉ transcode, không overlay, khi độ phân giải đổi Main→Sub.
   `TranscodeProcessor::processFilteredVideoFrame` chỉ scale khi có overlay. Cần thử với file
   Main/Sub khác độ phân giải.

**Test.** Dựng hai MP4 ngắn khác độ phân giải (dùng helper MP4 sẵn có trong `tests/unit/record/`),
timeline AutoHigh chuyển giữa file, rồi assert DTS đưa vào muxer tăng đơn điệu.

### P3 — ReplayTimelinePlanner (`src/Record/ReplayTimelinePlanner.cpp`)

1. **Dừng ở mọi gap** (`if (!chosen) break;` dòng 66). Tài liệu nói replay dừng ở gap đầu tiên,
   nhưng một khoảng hở 1 s do làm tròn giữa hai segment liên tiếp cũng làm replay dừng
   (liên quan lỗi "replay dừng ở khoảng hở segment").
   Sửa: thêm config `record.replayMaxGapMS` (mặc định ~2000). Gap ≤ ngưỡng thì nối tiếp clip sau,
   `readTimelineTick` đã xử lý chờ `clip_start`. Gap lớn hơn thì vẫn dừng như tài liệu.
2. **Đổi Main/Sub liên tục** khi Main có nhiều lỗ ngắn: mỗi lần đổi đều gọi `resetTracks`.
   Sửa: khoảng dùng nguồn không ưu tiên ngắn hơn N giây thì vẫn giữ, không đổi về nguồn ưu tiên
   cho đoạn quá ngắn (hysteresis). Cân nhắc lại nếu làm vậy vi phạm luật "preferred thắng overlap".
3. (Tuỳ chọn) Thuật toán đang là O(points × blocks); với timeline 24h có thể chuyển sang sweep đã sort.

**Test.** Thêm vào `tests/unit/record/test_replay_timeline_planner.cpp`:
- gap 1 s → nối tiếp;
- gap 3 s → dừng;
- lỗ ngắn trong Main → không đổi nguồn quá nhiều lần.

### P4 — Hiệu năng

1. `configureTimelineAudio` (dòng 250) mở **mọi** file MP4 trong timeline (tới 24h) một cách đồng
   bộ trên luồng gọi `createFromMP4ByApp`, tức luồng HTTP session.
   Sửa: chỉ kiểm tra clip đầu tiên của mỗi `stream_id`, vì config audio của một stream thường không
   đổi. Hoặc kiểm tra lười khi mở từng clip, gặp clip không tương thích thì tắt audio từ đó.
2. Chuyển việc build timeline và mở replay sang work poller (`WorkThreadPool`), trả kết quả về
   session qua `async` với `weak_ptr`.
3. `checkLiveStreamHls` emit `kBroadcastMediaViewOverlay` cho **mỗi** request segment.
   Sửa: bỏ qua bước này với segment của derived stream đã được xác thực ở P1.2, hoặc cache policy
   theo `session_id`.

### P5 — Dọn dẹp

1. `src/Common/config.cpp:234-236` đổi `rtsp/rtmp/ts_demand` từ 0 sang 1, trong khi
   `conf/config.ini` vẫn là 0. Replay không cần thay đổi này vì `createFromMP4ByApp` tự đặt demand,
   nên cần **trả về 0**. Nếu đổi có chủ ý thì phải sửa cả `config.ini` và tài liệu.
2. `createFromMP4ByApp` (`src/Common/MediaSource.cpp:682-712`): hai request đồng thời có thể
   đăng ký trùng stream, `regist` ném `invalid_argument` và request thứ hai nhận 404.
   Sửa: trong `catch`, gọi `MediaSource::find(...internal_stream)` lần nữa rồi trả về nếu đã có.
3. `tests/unit/CMakeLists.txt`: `unit_replay` (dòng 48) và `unit_http` (dòng 35) link ext-codec,
   nên `gtest_discover_tests` không đăng ký test nào và không báo lỗi. Thêm:
   ```cmake
   add_test(NAME unit.replay_all COMMAND unit_replay)
   set_tests_properties(unit.replay_all PROPERTIES LABELS unit TIMEOUT 15)
   add_test(NAME unit.http_all COMMAND unit_http)
   set_tests_properties(unit.http_all PROPERTIES LABELS unit TIMEOUT 15)
   ```
   Kiểm tra bằng `ctest -N | tail -1`: tổng phải tăng lên.
4. Xoá code không dùng trong MergedMP4Reader: `getCurrentStamp`/`setCurrentStamp`/`_replay_seek_to`/
   `_replay_seek_ticker` và tham số `gop_cache` (dòng 33, 52). Cân nhắc tính tốc độ phát theo đồng
   hồ thực (như `MP4Reader`) thay vì đếm tick, để không chậm dần khi máy tải cao.
5. Motion-MJPEG trên camera bắt buộc overlay đi vào luồng transcode vốn không sinh output MJPEG,
   nên trả 503 "not ready". Cân nhắc trả source gốc (thống nhất với P0) hoặc báo lỗi rõ ràng.

## 5. Checklist trước khi commit

- [x] `cmake --build build-test -j2` sạch
- [x] `ctest -j2` pass, `ctest -N` có `unit.replay_all` và `unit.http_all` (240 test)
- [ ] Chạy thử replay HLS/fMP4 `quality=auto` trên camera có Main/Sub và overlay bật
- [ ] Chỉ stage file cụ thể, không stage `3rdpart/*`, `build-test/`;
      nhớ `git add tests/unit/record/test_merged_mp4_reader.cpp`
- [ ] Commit xong thì drop stash `overlay-auto-profile-tmp-2026-09-25`
      (tìm `stash@{n}` theo tên trước khi drop)

## 6. Kết quả thực hiện và phần chưa làm

### 6.1. Đã làm

| Mục | Thay đổi | File |
|---|---|---|
| P1.2 | Suffix derived = `.transcode.<md5 tham số transcode>.<tag policy>`. Tag = MD5 của phần overlay đang bắt buộc (watermark: template + username + tên camera; mask: vùng mask). HLS chỉ tin derived stream khi origin `transcode` **và** tên kết thúc bằng tag của chính người xem, không thì 403 | `src/Http/HttpSession.{h,cpp}` (`makeViewOverlayPolicyTag`, `isTrustedViewOverlayDerivedSource`) |
| P2.1 | `ReplayClipWindow` gom phép map timestamp; frame có DTS trước `play_from_ms` bị bỏ, chờ IDR kế tiếp. Log khi seek rơi vào keyframe khác offset yêu cầu | `src/Record/MergedMP4Reader.{h,cpp}` |
| P2.1b | Đổi Main↔Sub tại điểm giao: mở trước clip sau, seek tới keyframe ≤ điểm giao (`seekKeyframeAtOrBefore`), rút `end_ms` clip trước về keyframe đó và phát clip sau từ keyframe (`replaySwitchPoint`). Không còn hụt GOP khi đổi nguồn | như trên |
| P2.2 | `readTimelineTick`, `pause`, `speed`, `stopReplay` lock `_mtx` (cùng mutex với `seekTo`) | như trên |
| P2.3 | `parseReplayStartTime()`: cắt `.replay.` rồi yêu cầu phần cuối chỉ gồm chữ số | như trên |
| P3.1 | Khoảng hở ≤ `record.replayMaxGapSec` (mặc định 2) được bỏ qua, dài hơn thì dừng. Khoảng hở ngay tại `stamp` vẫn trả rỗng | `src/Record/ReplayTimelinePlanner.{h,cpp}`, `src/Common/config.{h,cpp}`, `conf/config.ini`, `server/Manager.cpp` |
| P3.2 | Hysteresis mode Auto: đoạn stream ưu tiên ngắn hơn `record.replayMinSwitchSec` (mặc định 5) giữ stream đang phát nếu block của stream đó còn phủ | như trên |
| P4.1 | `configureTimelineAudio` chỉ mở clip đầu tiên của mỗi `stream_id` | `src/Record/MergedMP4Reader.cpp` |
| P4.2 | `findAsyncByApp` chạy `createFromMP4ByApp` trên pool riêng `replay open` (2 thread, `toolkit::ThreadPool`), trả kết quả về poller của session qua `weak_ptr<Session>` | `src/Common/MediaSource.cpp` |
| P4.3 | `HlsViewerPolicyCache`, đặt cùng `HlsViewerSession` để quản lý tập trung (key = `HttpFileManager::getHlsViewerKey`: vhost + live/record + camera + session_id, TTL = `hls.viewerTimeoutSec`, gia hạn mỗi lần đọc). `HttpSession::resolveHlsViewOverlayPolicy`: request có `token` lấy policy và ghi cache; request không `token` đọc cache, không có thì lấy policy không token (đủ overlay). Sửa lỗi: URI segment chỉ mang `session_id`, không mang JWT, nên trước đây segment luôn nhận policy không exclusion → người miễn mask bị 403 vì lệch tag, người miễn hoàn toàn bị 403 "Original HLS segment" | `src/Http/HlsViewerSession.{h,cpp}`, `src/Http/HttpSession.{h,cpp}`, `src/Http/HttpFileManager.{h,cpp}` |
| P5.1 | Trả `rtsp/rtmp/ts_demand` về 0 | `src/Common/config.cpp` |
| P5.2 | `createFromMP4ByApp` bắt `invalid_argument` khi đăng ký trùng, trả source đã có | `src/Common/MediaSource.cpp` |
| P5.3 | Đăng ký `unit.http_all`, `unit.replay_all` | `tests/unit/CMakeLists.txt` |
| P5.4 (một phần) | Xoá `getCurrentStamp`/`setCurrentStamp`/`_replay_seek_to`/`_replay_seek_ticker` và tham số `gop_cache` | `src/Record/MergedMP4Reader.{h,cpp}` |
| P5.5 | Motion MJPEG phát source gốc khi policy yêu cầu overlay (trước là 503), thống nhất với P0 | `src/Http/HttpSession.cpp` |
| Docs | Cập nhật hành vi gap, hysteresis, tên derived stream, Motion MJPEG | `docs/api-media-playback.md`, `docs/playback-flow.md` |

Test mới: `test_hls_overlay_guard.cpp` (tag khớp / policy khác / user khác / suffix cũ),
`test_replay_timeline_planner.cpp` (gap 1s nối tiếp, gap 3s dừng, không đổi nguồn liên tục),
`test_merged_mp4_reader.cpp` (parse start time, DTS đơn điệu khi đổi clip, tìm keyframe ≤ đích,
điều kiện điểm chuyển, đổi Main→Sub không hụt frame). `test_hls_overlay_guard.cpp` thêm luồng
playlist có token → segment không token (miễn mask, miễn hoàn toàn, session lạ/hết hạn, camera khác,
token mới làm mới cache) và purge cache. `unit_replay` 17 test, `unit_http` 29 test, ctest 240/240.

### 6.2. Chưa làm

| Mục | Việc còn lại | Ghi chú |
|---|---|---|
| Chạy thật | Replay HLS/fMP4 `quality=auto` trên camera có Main/Sub, overlay bật; thử cả seek và đổi Main↔Sub giữa file | Chưa có kiểm chứng end-to-end nào cho toàn bộ nhánh |
| P2.4 | Stream chỉ transcode (không overlay) khi đổi Main→Sub khác độ phân giải | `TranscodeProcessor::processFilteredVideoFrame` chỉ scale khi có overlay; cần file ghi thật |
| P2 test | Test DTS dùng file MP4 thật qua `MergedMP4Reader` | Hiện chỉ test `ReplayClipWindow` (logic map/bỏ frame), chưa chạy reader thật vì repo chưa có helper sinh MP4 H264 |
| P3.3 | Planner O(points × blocks) → sweep đã sort | Chỉ đáng làm nếu timeline 24h có nhiều block |
| P5.4 (phần còn lại) | Tính tốc độ phát theo đồng hồ thực thay vì đếm tick | Hiện mỗi tick tiến `sampleMS × speed`; máy tải cao thì phát chậm dần |
| P4.3 đánh đổi | Admin đổi policy camera hoặc thu hồi token: segment của session đang xem vẫn dùng policy cũ tới lần reload playlist kế tiếp (reload mang token nên làm mới cache) hoặc tới khi session hết hạn; giống cache xác thực HLS hiện có | Chưa có cơ chế xoá cache chủ động theo camera/token |
| Rủi ro còn lại | `MergedMP4Reader::close()` reset `_replay_timer` không lock `_mtx` | Giống `MP4Reader`, chưa thấy lỗi |
| Rủi ro còn lại | Đăng ký trùng xảy ra ở tick sau (không phải tick đầu) sẽ ném trong timer, không qua nhánh catch của P5.2 | Hiếm: FMP4 đăng ký ngay gói đầu tiên, tức tick đầu |
| Hành vi mới cần biết | Đổi Main↔Sub giữa file không còn hụt GOP (P2.1b), nhưng clip sau khoảng hở và clip sau seek vẫn chờ IDR đầu tiên từ điểm bắt đầu nên có thể hụt tối đa một GOP. Audio đổi cấu hình giữa các file cùng stream gây `resetTracks` thay vì tắt audio | Hệ quả của P2.1 và P4.1 |
| P2.1b chưa kiểm chứng | Chuyển nguồn sớm dựa trên giả định timestamp mẫu đầu file ≈ `file_start_time` (giống phép map sẵn có); seek vào vùng `(keyframe, điểm giao)` vẫn chờ IDR như seek thường | Cần thử với file ghi thật |

## 7. Luồng replay ghép Main/Sub

### 7.1. Từ request đến reader

```text
GET /media/record/{cameraId}/vod/{stamp}.live2.mp4?quality=auto&prefered=hi&duration=...
  HttpSession (poller của session)
    -> kBroadcastMediaPlayed (auth)
    -> MediaSource::findAsyncByApp
         tìm source đã đăng ký theo (schema, vhost, app, stream)
         không có + schema là fMP4/HLS/HLS-fMP4/TS/RTMP/RTSP
           -> pool "replay open":  createFromMP4ByApp(...)
           -> poller của session:  cb(source)
    -> applyViewOverlayPolicy(source)
         policy không yêu cầu overlay/transcode -> phát thẳng source replay
         có overlay -> derived transcode "<internal>.transcode.<md5>.<tag>" rồi phát nó
```

HLS đi qua `checkLiveStreamHls`: playlist gốc bị redirect 302 sang stream nội bộ (hoặc
derived stream nếu có overlay); segment tới derived stream chỉ phục vụ khi tag khớp (P1.2).

`createFromMP4ByApp`:
1. Rút `quality` (mặc định `auto`), `prefered` (`hi`), `duration` (`0`) thành `selection_params`.
   JWT, role, session HLS **không** nằm trong đây, nên người xem cùng lựa chọn dùng chung một replay.
2. Stream nội bộ = `{stream}.replay.{MD5(selection_params)}`, ví dụ
   `cam1/vod/1758790000.replay.9f2c...`. Nếu đã có thì trả luôn.
3. `ProtocolOption` chỉ bật đúng schema được yêu cầu, mọi `*_demand = false`.
4. Tạo `MergedMP4Reader(tuple, option)` rồi `openForReplay()`; sau đó `MediaSource::find` lại.
   Nếu đăng ký trùng với request đồng thời thì trả source đã có (P5.2).

### 7.2. Dựng danh sách clip

`MergedMP4Reader::startTimelineReplay`:
1. `parseReplayStartTime` lấy `stamp` (giây) từ phần cuối stream id; `app` = phần đầu (camera).
2. `kBroadcastGetStreamQuality` → `server/Manager.cpp` đọc `stream_map` của `StatisticRecorder`
   (`info.txt`): `0` → stream_id MainStream, `1` → stream_id SubStream. Camera offline vẫn tra được.
3. `kBroadcastGetReplayTimeline` → listener trong `server/Manager.cpp`:
   - `query_end = stamp + (duration ? duration : 24h)`.
   - Với từng stream (Main = source 0, Sub = source 1): `TimeQuery::getRecordedTimePeriod(stamp, query_end)`,
     mỗi `TimeBlock` có file resolve được (`resolveRecordedBlockPath`, bỏ block rỗng) thành một
     `ReplayBlock{source, stream_id, file_path, start, end, file_start_time}`, trong đó
     `start/end` bị cắt vào `[stamp, query_end]` còn `file_start_time` là giờ bắt đầu thật của file.
   - Gọi `ReplayTimelinePlanner::build(blocks, stamp, query_end, mode, options)`, với
     `mode = resolveMode(quality, prefered)` và `options` từ `record.replayMaxGapSec` /
     `record.replayMinSwitchSec`.
   - `total = clip cuối.start + duration - stamp`.

`ReplayTimelinePlanner::build` (đơn vị giây):

| Mode | `quality`/`prefered` | Nguồn được chọn |
|---|---|---|
| OnlyHigh | `hi` | Chỉ Main, không fallback |
| OnlyLow | `lo` | Chỉ Sub, không fallback |
| AutoHigh | `auto` + `hi` | Main thắng vùng chồng lấn, Sub lấp chỗ Main thiếu |
| AutoLow | `auto` + `lo` | Sub thắng vùng chồng lấn, Main lấp chỗ Sub thiếu |

1. **Điểm cắt.** Gom `stamp`, `query_end` và biên đầu/cuối của mọi block, sort và bỏ trùng.
   Hai điểm liên tiếp tạo thành một khoảng cơ sở.
2. **Chọn block cho từng khoảng.** Chỉ xét block phủ trọn khoảng (và đúng source nếu là mode Only).
   Ở mode Auto, block của nguồn ưu tiên thắng; cùng nguồn thì lấy block gặp trước.
3. **Khoảng không có block.**
   - Ngay tại `stamp` (chưa có run nào) → dừng, kết quả rỗng → request nhận 404.
   - Sau đó: cộng dồn độ dài khoảng hở; ≤ `max_gap_s` thì bỏ qua (không sinh clip), vượt quá thì dừng.
     Khoảng hở ở cuối không sinh gì.
4. **Gộp run.** Khoảng kế tiếp liền kề và cùng `(source, stream_id, file_path)` được nối vào run trước.
5. **Hysteresis (chỉ mode Auto, `min_switch_s > 0`).** Duyệt run: run của nguồn ưu tiên, ngắn hơn
   `min_switch_s`, liền kề sau run nguồn không ưu tiên mà block của run đó còn phủ tới hết run này
   → nối vào run trước (không đổi nguồn). Run sau đó nếu cùng file cũng được nối tiếp.
6. **Xuất clip.** Mỗi run thành một `ReplayClip{source, stream_id, file_path, start_time, duration, file_start_time}`.

Ví dụ AutoHigh: Sub phủ `[100,160)`, Main có `[100,110) [112,115) [117,120) [122,160)` →
`main1 [100,110)`, `sub [110,122)` (hai đoạn Main 3s bị bỏ vì hysteresis), `main4 [122,160)`.

### 7.3. MergedMP4Reader sau khi có clip

Trạng thái chính: `_timeline_clips`, `_timeline_clip_idx`, `_timeline_demuxer` (clip đang mở),
`_timeline_window` (`start_ms`, `end_ms`, `file_offset_ms`, `play_from_ms`), `_timeline_next`
(clip sau đã mở sẵn khi đổi Main↔Sub: `index`, `demuxer` đã seek, `play_from_ms`),
`_replay_current_ms` (đồng hồ timeline, tính từ `stamp`), `_timeline_pending_frame`,
`_timeline_priming`, `_timeline_discard_before_ms`.

**Khởi động** (`startTimelineReplay`, chạy trên pool `replay open`):
1. `configureTimelineAudio`: mở clip đầu của mỗi stream_id, so codec + extra data của audio.
   Không tương thích thì tắt audio (và mute audio) cho toàn replay.
2. `openTimelineClip(0)`, tạo `Timer` chu kỳ `record.sampleMS` (mặc định 500 ms) trên `_poller`
   (poller của `WorkThreadPool`), rồi chạy một tick ngay để output đăng ký source trước khi
   `createFromMP4ByApp` gọi `find`. Timer giữ `shared_ptr` tới reader; reader sống tới khi tick trả
   `false` hoặc `close()`.

**Mở clip** (`openTimelineClip`):
1. Nếu `_timeline_next` là clip này thì dùng demuxer đã mở và đã seek sẵn, cùng `play_from_ms` của nó.
   Không thì `MP4Demuxer::openMP4(file)`.
2. `normalizeTracks`: giữ một video (index 0) và một audio (index 1).
3. `_timeline_window = makeReplayClipWindow(clip)`: `start_ms = (clip.start - stamp) × 1000`,
   `end_ms = start_ms + duration × 1000`, `file_offset_ms = (clip.start - clip.file_start_time) × 1000`,
   `play_from_ms = start_ms` (hoặc giá trị của clip đã chuẩn bị).
4. Có video thì bật priming (chờ IDR). Clip không chuẩn bị sẵn và `file_offset_ms > 0` thì
   `seekTo(file_offset_ms)`; seek rơi vào keyframe gần nhất, có thể trước hoặc sau điểm bắt đầu.
5. Muxer: chưa có thì `initOutputMuxer` (tạo `MultiMediaSourceMuxer`, thêm mute audio nếu cần,
   `setMediaListener(reader)`). Track khác clip trước (codec hoặc SPS/PPS, ví dụ Main↔Sub) thì
   `switchToMuxer` → `resetTracks` rồi thêm track mới. Derived transcode (overlay) sống qua lần reset này.
6. `prepareNextClip`: chuẩn bị clip sau nếu nó đổi nguồn (xem dưới).

**Đổi Main↔Sub tại điểm giao** (`prepareNextClip`, chạy ngay khi mở clip hiện tại):
1. Chỉ áp dụng khi clip sau khác `source`, liền kề (`next.start_ms == current.end_ms`) và bắt đầu
   giữa file (`file_offset_ms > 0`, tức stream đó đã ghi từ trước điểm giao).
2. Mở file clip sau, `seekKeyframeAtOrBefore(file_offset_ms)`: seek gần nhất; nếu rơi sau điểm giao
   thì lùi dần (bước tăng gấp đôi, tối đa 8 lần) tới keyframe ≤ điểm giao.
3. `replaySwitchPoint`: keyframe map ra timeline là `K`. Nếu `current.play_from_ms < K ≤ điểm giao`
   thì `current.end_ms = K` và clip sau `play_from_ms = K`. Ngược lại giữ ranh giới của planner.
4. Lưu demuxer (đã seek) vào `_timeline_next`. Khi clip hiện tại chạm `end_ms = K`, tick chuyển sang
   clip sau, frame đầu tiên của nó là keyframe tại `K`, nên không chồng lấn và không hụt GOP.

```text
Main   ████████████████|··········          (clip Main theo planner kết thúc ở T)
Sub    ··········|K████████████████         (file Sub có keyframe K ≤ T)
Output ██████████|█████████████████         (Main tới K, Sub từ K)
```

`_timeline_next` bị huỷ khi seek, khi `fileRepeat` quay về clip 0, khi khởi động lại hoặc `stopReplay`.

**Mỗi tick** (`readTimelineTick`, giữ `_mtx`):
1. Đang pause → không làm gì. `target = current + sampleMS × speed`.
2. Lặp:
   - Hết clip → `fileRepeat` thì quay về clip 0, không thì trả `false` (timer dừng).
   - `current < play_from` của clip (khoảng hở được bỏ qua ở planner, hoặc sau seek) → tiến
     `current` tới `min(target, play_from)` rồi chờ tick sau. `play_from` lấy từ clip đang mở, clip đã
     chuẩn bị, hoặc `start_ms` theo planner.
   - Chưa mở clip → `openTimelineClip`.
   - Đọc frame (lấy `pending` trước). EOF → đóng clip, sang clip sau.
   - `mapped = start_ms + raw - file_offset_ms` cho cả DTS và PTS.
     `mapped ≥ end_ms` → sang clip sau. `mapped < play_from_ms` → bỏ (frame thuộc vùng clip trước).
     `mapped < discard_before` (sau seek) → bỏ.
   - `mapped > target` → giữ làm `pending`, `current = target`, kết thúc tick.
   - Priming: bỏ frame cho tới IDR video đầu tiên.
   - Đẩy frame qua track clone (đặt lại index 0/1) → `inputFrame` → muxer; video cũng nuôi
     `MuteAudioMaker` nếu có. Cập nhật `current = mapped`.

**Điều khiển từ player** (`MediaSourceEvent`):
- `seekTo(stamp_ms)`: lock, tìm clip đầu tiên có `end > stamp`, đặt `current = discard_before = stamp`,
  đóng clip hiện tại; nếu `stamp` nằm trong clip thì mở ngay, nằm trong khoảng hở thì để tick chờ.
- `pause` / `speed` (0.1–20): lock rồi đổi cờ; tick sau dùng giá trị mới.
- `close`: reset timer. Không còn người xem thì `MediaSourceEvent::onReaderChanged` đóng source
  replay sau `general.streamNoneReaderDelayMS`.
