# Plan xử lý lỗi từ lần chạy E2E camera thật (2026-09-26)

Nhánh `feature/overlay-with-auto-profile`, sau hai commit `3ebc08e5` (overlay + auto profile) và
`ee287304` (harness `tests/integration/e2e_real`). Kịch bản gốc ở
[`docs/e2e-test-plan.md`](e2e-test-plan.md).

## 1. Bối cảnh lần chạy

- Camera thật: Main 1920x1080 H.264 + AAC, Sub 640x360 H.264 + AAC 8 kHz mono, 25 fps, GOP 2 s.
  Camera nằm ngoài mạng nội bộ; URL RTSP và thông tin MinIO chỉ đưa qua biến môi trường `E2E_*`.
- MinIO làm tầng COLD (bucket test, prefix `e2e-<epoch>`; đã xoá sau khi chạy).
- Thứ tự: `always` (~10 phút) → `RecordLowResAndMotion` + motion + overlay (~10 phút) → tier HOT→COLD
  → mã hoá mode 2 trên work dir thứ hai.
- Kết quả: 60 PASS / 20 FAIL / 1 SKIP. Nhóm smoke/live/auth, record, tier và mã hoá (ghi, đọc lại)
  đạt. Ghi theo `RecordLowResAndMotion` đúng: Sub ghi liên tục, Main chỉ ghi khi có motion.
- Unit test tại `ee287304`: `ctest -R unit.` 161/161 pass.

## 2. Danh sách vấn đề

Trạng thái cập nhật 2026-09-26 sau khi làm P8 + P1 (xem mục 5).

| # | Mức | Vấn đề | Kịch bản bị ảnh hưởng | Nguồn gốc | Trạng thái |
|---|---|---|---|---|---|
| 1 | P0 | Replay gộp không phát frame của clip đầu tiên, source chỉ đăng ký khi chuyển clip | RPL-05/06/22, APF-02..05, OVL-08/09, ENC-05b/c | Nhánh này (`MergedMP4Reader`) | ✅ Đã sửa |
| 1b | P0 | Sau lần chuyển Main↔Sub, `tfdt` của fMP4 bắt đầu lại từ 0 | Replay auto trên trình duyệt (MSE) | `728db0cc` (2026-04-26) | ✅ Đã sửa (P1b), chưa thử trên trình duyệt |
| 2 | P0 | `watermarkExcludedRoleIds` bị xoá khi nạp template, role loại trừ vẫn thấy watermark | OVL-04; RPL-13/14/22 chậm vì admin vẫn đi qua transcode | `a8c8b0ae` (2026-08-04) | ✅ Đã sửa, chưa chạy lại E2E |
| 3 | P1 | Overlay transcode giữ cố định độ phân giải của clip đầu khi replay chuyển Main↔Sub | APF-06 | Nhánh này | ✅ Đã sửa, chưa chạy lại E2E |
| 4 | P2 | `conf/config.ini` có `[ffmpeg] delay_close_sec=0`, code hiểu là xoá job extract ngay | EXT-01/03 (chỉ khi dùng file mẫu) | Có từ trước | Không phải lỗi production |
| 5 | P1 | Tìm bookmark theo từ khoá lỗi (`-200` / `-400`) | BKM-03a/03b | Có từ trước | ✅ Đã sửa, chưa chạy lại E2E |
| 6 | P2 | Replay `.live.mp4` với `stamp = now − 90 s`: lần đầu không kết thúc sau 240 s, với config sinh từ code thì dừng sau 15 s kèm lỗi | RPL-17 | Thiết kế có sẵn | Không phải lỗi (đúng ý đồ) |
| 7 | P2 | Request replay đầu tiên ngay sau khi khởi động trả `403` | (quan sát khi thử nghiệm) | Có từ trước | ✅ Đã sửa, chưa chạy lại E2E |
| 8 | P2 | Lỗi harness | BKM-01, ENC-05/13, config | Harness | ✅ Đã sửa |
| 9 | P1 | Tải file extract luôn `401 No permission on key` khi `enableAuthorize=1` | EXT-01/03 | Có từ trước | ✅ Đã sửa, chưa chạy lại E2E |
| 10 | P1 | Thumbnail có overlay lỗi: file SVG overlay thiếu đuôi `.svg`, ffmpeg không mở được | RPL-19, BKM-02 | `79e823d0` (2026-09-20) | ✅ Đã sửa, chưa chạy lại E2E |
| 11 | P2 | Người xem thứ hai của cùng một replay (cùng tham số) vào giữa luồng, không từ `stamp` | Replay mở song song | Thiết kế có sẵn | Giữ như hiện tại (chốt 2026-09-26) |
| 12 | P2 | `playback/speed` 2x trên `.live.mp4` không còn đổi nhịp (20 s media mất 16–23 s) với config sinh từ code | RPL-15 | Hệ quả của vấn đề 2 (admin đi qua overlay transcode) | Không phải lỗi speed; hết cùng vấn đề 2, chưa chạy lại E2E |
| 13 | P3 | Khựng ~1 s ở biên file ghi khi replay qua hai file liên tiếp cùng stream | APF (đo nhịp) | Nhánh này (`MergedMP4Reader`) | ✅ Đã sửa, chưa chạy lại E2E |
| 14 | P2 | Replay gộp (`auto`/`lo`) phát xong `duration` nhưng kết nối HTTP fMP4 còn mở thêm ~30 s | APF-02 (30 s media mất 61 s) | `8b9aff00` (2026-04-06) | ✅ Đã sửa, chưa chạy lại E2E |
| 15 | P2 | `playback/speed` với URL `live2.mp4` luôn trả `906001 Playback stream not found` | RPL-15b (mới) | Id nội bộ `.replay.<md5>` của replay gộp | ✅ Đã sửa (P9 bước 1), chưa chạy lại E2E camera thật |
| 16 | P2 | `duration` chỉ có tác dụng ở HTTP-fMP4 per-stream; RTSP/RTMP/TS/FLV/HLS per-stream bỏ qua, và người xem khác `duration` dùng chung một `MP4Reader` | RPL (per-stream) | Có từ trước | ✅ Đã sửa (P9 bước 2), chưa chạy lại E2E camera thật |
| 17 | P2 | Thumbnail (`recordedThumnail`) `504` khi `pos` nằm xa đầu file: lệnh `ffmpeg.snap` mặc định đặt `-ss` sau `-i`, giải mã từ đầu file, vượt `recorded_thumbnail_timeout_sec=5` (1080p: giây 59 > 5 s, giây 22 mất 3,7 s) | RPL-19 (tuỳ stamp) | `cfcffc00` (2026-06-16) | ✅ Đã sửa |
| 18 | P1 | File Main ghi theo motion (pre-record) tên `1970-01--4/…`: `MP4Recorder` đọc `start_time_ms` (epoch ms) bằng `atoi`, tràn `int` | REC-05, APF (tìm Main theo timeline) | `d5624d99` (2026-08-31) | ✅ Đã sửa |
| 19 | P1 | Segment HLS của mọi người xem có overlay (live và replay) trả `403 Untrusted HLS derived stream`: stream id của request segment dính đường dẫn segment nên không tìm thấy source dẫn xuất | OVL-09 (và HLS live có overlay) | Nhánh này | ✅ Đã sửa |
| 20 | P1 | Người không cần overlay (admin được loại trừ) đọc được playlist dẫn xuất của viewer khác (watermark mang tên người đó) | OVL-03 | Lộ ra sau khi sửa vấn đề 2 | ✅ Đã sửa |
| 21 | P2 | Tìm bookmark theo tag hoặc mô tả không ra: `DECLARE_ENTITY(BookmarkIndex)` thiếu cột `description`, `tags_csv` nên FTS của ESC chỉ có tên | BKM-03b | `3fe8dca4` (2026-06-02) | ✅ Đã sửa (bookmark cũ có lại tag/mô tả khi được cập nhật) |

## 3. Chi tiết và cách xử lý

### P1 — Replay gộp mất clip đầu (vấn đề 1) — ✅ đã sửa

**Hiện tượng (trước khi sửa)**

- `GET /media/record/<cam>/vod/<stamp>.live2.mp4` (mọi `quality`) và HLS replay trả `404` ngay
  (≤ 10 ms). Gọi lại liên tục thì phát được sau 20–55 s.
- `quality=auto&prefered=hi` tại stamp chỉ có Sub, 20 s sau mới có Main: luồng bỏ hẳn 20 s Sub và
  bắt đầu ở đoạn Main.

**Nguyên nhân gốc** (tái hiện bằng `MergedReplayOpenTest`, fail trước khi sửa và pass sau khi sửa)

1. `readTimelineTick` chờ IDR đầu tiên của clip (`_timeline_priming`) bằng `Frame::keyFrame()` trên
   frame thô của `MP4Demuxer`. Keyframe do recorder ghi mang SPS/PPS phía trước IDR trong cùng
   sample, nên frame thô là config frame (`keyFrame()=false`). Priming không bao giờ kết thúc và cả
   clip đầu bị bỏ; source chỉ đăng ký khi clip sau được mở.
2. Không thể chờ IDR sau bước tách NAL của track clone: `H264Track::inputFrame_l` ép slice đầu tiên
   sau một config frame thành keyframe, mà track clone kế thừa trạng thái "vừa nhận SPS/PPS" từ
   `setExtraData`, nên một P-frame sẽ kết thúc priming (đã thấy trên dữ liệu thật: luồng mở đầu bằng
   ~23 P-frame).
3. `createFromMP4ByApp` gọi `find()` ngay sau `openForReplay()`, trong khi source đăng ký bất đồng bộ;
   mỗi lần client gọi lại tạo thêm một reader.

**Đã sửa**

- [`MergedMP4Reader::readTimelineTick`](../src/Record/MergedMP4Reader.cpp): priming dùng cờ
  sync-sample của MP4 (`readFrame(key, eof)`), giữ cờ này cho `_timeline_pending_frame`; các frame
  bị bỏ trước IDR không bị tính nhịp, đồng hồ replay nhảy tới IDR nên phát ngay thay vì chờ tới 1 GOP.
- [`MediaSource.cpp`](../src/Common/MediaSource.cpp): `findAsyncByApp_l` lắng nghe
  `kBroadcastMediaChanged` trước, rồi mới mở reader trên pool `replay open`, và trả kết quả khi source
  đăng ký (timeout `general.maxStreamWaitMS`). `ReplayOpenRegistry` cho các request cùng id dùng
  chung một reader; không có clip thì trả 404 ngay. `openForReplay()` trả `bool`.
- [`MP4MuxerInterface::inputFrame`](../src/Record/MP4Muxer.cpp): giữ SPS/PPS đến trước keyframe đầu
  tiên (sau `resetTracks`) và ghi chúng cùng sample với keyframe, để lần chuyển Main↔Sub có
  SPS/PPS trong luồng. Ảnh hưởng cả recorder: sample đầu của file ghi giờ có SPS/PPS (chuẩn, vô hại).
- Unit test: `MergedReplayOpenTest.RegistersSourceWhileFirstClipPlays` (tự ghi recording bằng
  `MP4Muxer`, source phải đăng ký ≤ 500 ms), `MP4MuxerParameterSetsTest.FirstKeyframeSampleCarriesSpsAndPps`.

**Kết quả E2E sau khi sửa** (camera thật, dữ liệu `RecordLowResAndMotion`)

- APF-02: Sub từ pts 0, chuyển Main đúng thời điểm (18,9–19,9 s cho đoạn Main bắt đầu sau 20 s), về
  Sub khi hết đoạn Main. APF-05: request đầu 200 sau 0,01 s. APF-03: `quality=hi` 404, `lo` phát Sub.
  APF-04: HLS auto 302 sau 2,0 s, chuyển Sub→Main không lỗi decode.
- RPL-05a/05b/06 PASS (trước đây 404). RPL-22 với token admin còn 2,4 s (live2) / 6,9 s (HLS) vì
  admin vẫn bị áp watermark (vấn đề 2) nên đi qua transcode overlay.
- Nhịp phát replay auto đúng 1x.

### P1b — `tfdt` về 0 sau lần chuyển Main↔Sub (vấn đề 1b) — ĐÃ SỬA

- Nguyên nhân: khi đổi track, `FMP4MediaSourceMuxer::resetTracks` tạo writer libmov mới. libmov ghi
  `tfdt = samples[0].dts - start_dts` (`start_dts` = dts sample đầu của writer) nhưng `sidx` ghi pts
  tuyệt đối. `seedStampOffsets` đưa dts tiếp nối vào libmov, nhưng libmov trừ lại, nên fragment đầu
  sau lần chuyển có `tfdt=0`. MSE sẽ đặt nó ở t=0 và đứng hình.
- Thêm một lỗi đi kèm: offset được lấy bằng `getDuration()`, tức dts của sample cuối, nên IDR đầu
  sau lần chuyển trùng dts với frame cuối trước đó (ffmpeg báo `non monotonically increasing dts`).
- Đã sửa (hướng 1, không đụng submodule):
  - `MP4MuxerInterface` lưu dts sample đầu của mỗi track (`start_dts`), dts cuối và khoảng cách
    sample cuối. `MP4MuxerMemory` gọi `reviseDecodeTime` trước `onSegmentData` để cộng `start_dts`
    vào mọi `tfdt`. Khi `start_dts` bằng 0 (luồng không reset) thì không đổi gì. Audio có `start_dts`
    khác 0 do sync A/V cũng được sửa: trước đây `tfdt` của audio lệch so với `sidx`.
  - `FMP4MediaSourceMuxer::resetTracks` flush trước rồi lấy offset bằng `getNextDts()`
    (dts cuối + khoảng cách sample cuối).
- Kiểm tra:
  - unit test `FMP4DecodeTimeTest.TfdtContinuesAcrossTrackReset`: fail trên code cũ (`tfdt` 0 sau
    reset, `sidx` 2920), pass sau khi sửa.
  - Replay auto camera thật: `tfdt` của cả hai track tăng liên tục qua init segment thứ hai.
- Còn lại: thử tay trên trình duyệt với player web. Đặc điểm có sẵn của `DeltaStamp`: dts vào bằng 0
  làm frame thứ hai cũng nhận delta 0 (hai sample đầu cùng thời điểm); không thuộc lần chuyển, chưa sửa.

### P2 — Loại trừ watermark theo role (vấn đề 2) — ✅ đã sửa

- [`loadWatermarkTemplateFromMap`](../server/Manager.cpp) gán `option.watermarkExcludedRoleIds = ""`
  mỗi khi tìm thấy template, sau khi `fromJson` đã đọc danh sách từ cấu hình camera. Kết quả:
  role admin được bỏ privacy mask (đúng) nhưng vẫn có watermark (ảnh `OVL_live_admin.jpg`).
- Không cần hỏi lại VMS: dòng gán rỗng có từ `a8c8b0ae`, lúc `fromJson` chưa đọc
  `watermarkExcludedRoleIds`. `4e54a7b2` ("nhận giá trị watermarkExcludedRoleIds từ api") thêm phần
  đọc nhưng không bỏ dòng này, nên đây là phần sót lại chứ không phải quy tắc.
- Đã sửa: bỏ dòng gán rỗng trong `loadWatermarkTemplateFromMap`. Không thêm unit test vì hàm là
  `static` trong `Manager.cpp`; kiểm bằng E2E.
- Còn lại: kiểm lại OVL-01..04 bằng ảnh evidence và RPL-13/14/22 (admin không còn đi qua transcode)
  trên camera thật.

### P3 — Độ phân giải đầu ra của overlay khi replay auto (vấn đề 3) — ✅ đã sửa

- Nguyên nhân: derived transcode sống qua các lần chuyển Main↔Sub, còn `FFmpegEncoder::openEncoder`
  chốt kích thước theo frame đầu (mặc định `transcode_width/height = 0`). Replay bắt đầu ở Sub thì
  đoạn Main bị hạ xuống 640x360. Overlay tự dựng lại graph khi đổi kích thước, không lỗi.
- Đã chốt (2026-09-26): đầu ra cố định bằng độ phân giải lớn nhất của timeline, đoạn Sub được upscale.
  Không phát init segment mới; đổi lại CPU/băng thông ở mức Main cả trong đoạn Sub.
- Đã sửa (tối giản):
  - [`MergedMP4Reader::configureTimelineAudio`](../src/Record/MergedMP4Reader.cpp): khi mở clip đầu của
    mỗi stream để dò audio, lấy luôn kích thước video lớn nhất ghi vào `_option.transcode_width/height`
    (option của muxer replay). Bỏ `return` sớm khi audio không tương thích để vẫn dò hết các stream.
  - [`MultiMediaSourceMuxer::ensureViewOverlayTranscode`](../src/Common/MultiMediaSourceMuxer.cpp):
    request không chỉ định kích thước thì dùng `transcode_width/height` của source.
  - Cũng xử lý P2.4 cũ (transcode không overlay đổi Main→Sub): encoder đã cố định kích thước lớn nhất.
- Giới hạn: Main và Sub khác tỉ lệ khung hình thì đoạn Sub bị kéo giãn. Không thêm unit test.
- Harness: APF-06 giờ yêu cầu đầu ra chỉ một độ phân giải, bằng Main (trước chỉ cần ≥ 1).
- Còn lại: chạy E2E APF-06, xem tay chất lượng đoạn Sub.

### P4 — Job extract bị xoá ngay khi xong (vấn đề 4) — không phải lỗi production

- `conf/config.ini` không dùng cho production: config.ini production được sinh từ default trong
  code, và default của `ffmpeg.delay_close_sec` là 300 s. Lỗi chỉ xảy ra khi chạy bằng file mẫu. Từ nay
  harness không dùng `conf/config.ini` nữa (P8). Việc còn lại là làm cho file mẫu và comment khớp code.


- `conf/config.ini` ghi `delay_close_sec=0` kèm comment "0 là không tự đóng". Nhưng
  `FFmpegExtractor::closeAfterDelaySec()` hiểu 0 là đóng ngay: ffmpeg xong sau 0,5 s thì
  `close()` xoá file, gỡ key, và `progress` trả `909001`. Code mặc định là 300 s.
- Việc cần làm:
  1. `closeAfterDelaySec()`: `delay_close_sec == 0` thì không hẹn giờ đóng (đúng như comment). File
     được dọn khi gọi `extractArchived/delete` hoặc khi server dừng.
  2. `conf/config.ini`: đặt `delay_close_sec=300` như mặc định trong code, để đĩa không đầy với
     cấu hình mẫu.
  3. Unit test trong `unit.extract_start_all`: với 0 thì job còn tồn tại sau khi hoàn tất; với N > 0
     thì bị dọn sau N giây.
  4. Kiểm lại EXT-01/03a/03b (mp4/mkv/avi, dài 60 ± 5 s, đúng độ phân giải Main).

### P5 — Tìm bookmark theo từ khoá (vấn đề 5) — ✅ đã sửa

- [`BookmarkIndexRepository::findByTimeRange` / `countByTimeRange`](../manager/Storage/BookmarkIndex.h)
  truyền thô `search_term` vào `bookmark_index_fts MATCH ?`. FTS5 hiểu `e2e-bkm-…` là toán tử và cột
  (`no such column: bkm`, API trả `-200`).
- Với từ khoá thường, API trả `-400 basic_string: construction from null`. Câu lệnh JOIN với
  `bookmark_index_fts` và chọn cột không kèm tên bảng (`bookmark_guid` có ở cả hai bảng), nên có cột
  đọc ra NULL và `fromRow` dựng `std::string` từ con trỏ null.
- Việc cần làm:
  1. Chuyển từ khoá thành FTS5 phrase: bọc trong `"`, nhân đôi `"` bên trong, thêm `*` nếu muốn tìm
     theo tiền tố. Dùng chung một hàm cho `find` và `count`.
  2. Chọn cột kèm tên bảng (`bookmark_index.<col>`) khi có JOIN; `fromRow` chịu được NULL.
  3. `sort`: repository chỉ nhận `desc`/`asc`, còn `docs/api-bookmark.md` ghi ví dụ
     `"start_time DESC"` (bị hiểu thành `ASC`). Sửa tài liệu cho khớp hoặc chấp nhận cả hai dạng.
  4. Unit test repository: từ khoá có `-`, `:`, `"`, dấu tiếng Việt; từ khoá rỗng; đếm khớp số dòng.
  5. Kiểm lại BKM-01 sau khi sửa harness (P8).
- Đã sửa (tối giản, [`BookmarkIndex.h`](../manager/Storage/BookmarkIndex.h)):
  - `ftsPhrase`: bọc từ khoá trong `"`, nhân đôi `"` bên trong; dùng cho cả `find` và `count`.
    Tokenizer trigram nên phrase khớp theo chuỗi con, không phân biệt hoa thường.
  - Cột SELECT kèm tên bảng `bookmark_index.` (tái hiện bằng sqlite3: không kèm thì lỗi
    `ambiguous column name: bookmark_guid` vì bảng FTS cũng có cột này).
  - Kiểm bằng sqlite3 trên db dựng từ migration: từ khoá có `-`/`:`, tiếng Việt, `"`, không khớp;
    `COUNT` khớp số dòng. Không thêm unit test.
- Chưa làm: mục 3 (`sort` trong `docs/api-bookmark.md`). Từ khoá dưới 3 ký tự không khớp gì
  (giới hạn của trigram, có từ trước).
- Còn lại: E2E BKM-01, BKM-03a/03b.

### P6 — Replay đoạn đang ghi không kết thúc (vấn đề 6) — không phải lỗi

- RPL-17: `stamp = now − 90 s`, `.live.mp4` per-stream, chưa kết thúc sau 240 s.
- Đây là hành vi đúng thiết kế (xác nhận 2026-09-26): replay per-stream (`MP4Reader` →
  `MultiMP4Demuxer::findNextSegment`) cố ý nối tiếp các file ghi mới, đuổi theo bản ghi. Không sửa code.
- Kỳ vọng của RPL-17 trong `docs/e2e-test-plan.md` ("không chờ bản ghi mới") cần đổi theo thiết kế này.
- Triệu chứng "dừng sau 15 s kèm lỗi" với config sinh từ code chưa rõ; xem lại log ở lần chạy tới.

### P7 — `403` ngay sau khi khởi động (vấn đề 7) — ✅ đã sửa

- Khi chạy lại server có sẵn bản ghi, request `quality=hi` đầu tiên (vài giây sau healthcheck) trả
  `403 View Overlay Required`, các lần sau thì bình thường.
- Nguyên nhân (đọc code, chưa xác nhận bằng log): `CameraManager::loadSavedCameraInfo` nạp `info.txt`
  của các camera bất đồng bộ trên work thread, còn HTTP server nhận request ngay. Trong lúc đó
  listener `kBroadcastMediaViewOverlay` không tìm thấy camera ở `DeviceSource` lẫn `StatisticRecorder`
  nên trả `Unavailable` → `OverlayForbidden` → `403`. (`getTokenCache` luôn tạo cache nên không phải
  nguyên nhân.)
- Đã sửa (tối giản, [`WebHook.cpp`](../server/WebHook.cpp)): policy `Unavailable` khi
  `CameraManager::isReady()` còn `false` thì trả `ResourceUnavailable` (HTTP/HLS `503`), sau đó
  giữ `403` như cũ. RTSP/RTMP không có nhánh riêng cho `ResourceUnavailable` (như giới hạn tài
  nguyên hiện nay).
- Còn lại: khởi động lại server có bản ghi rồi replay ngay; phải nhận `503` rồi `200`, không `403`.

### P8 — Sửa harness (vấn đề 8) — ✅ đã sửa

- `setup_e2e.py`: `config.ini` của work dir chỉ chứa các khoá harness cần đổi, mọi khoá khác lấy
  default trong code như production; bỏ override `hook.on_play` (default đã đúng) và khoá không tồn
  tại `record.fileSecond`; `api.extractRoot` trỏ vào work dir.
- `g_bookmark`: đúng tham số `search` (`camera_id, start_time, end_time, page, size, sort=desc,
  search`), `recent` và `mostUsedTags`; tên bookmark có `-` và `:`; tách BKM-03a/03b cho tìm kiếm
  full-text.
- `g_encrypt_verify`: stamp lấy từ tên file mã hoá dài nhất (`file_start_time`).
- Nhóm mới `auto-profile` (APF-01..07, tham số `--since`), dùng JWT `overlay=false` cho APF-02..05;
  mỗi phép đo dùng `duration` riêng để có source mới (vấn đề 11).
- RPL-22 trong nhóm replay: một request duy nhất, không gọi lại.

### P9 — `playback/speed` cho `live2.mp4` và `duration` trong `MP4Reader` (vấn đề 15, 16)

Chốt 2026-09-27: làm chung một plan, hai bước, commit riêng.

**Bước 1 — vấn đề 15.** `findPlaybackStream` ([`WebApiPlayback.cpp`](../server/WebApiPlayback.cpp)) bỏ đuôi
`.live2.mp4` rồi `find` theo `cam/vod/<stamp>`, nhưng source gộp đăng ký dưới
`cam/vod/<stamp>.replay.<md5(quality, prefered, duration)>` (`replayInternalStream`, static trong
`MediaSource.cpp`). Đã tái hiện: cả URL trần lẫn URL kèm query đều trả `906001`, 20 s media vẫn 1x.
- Mở `MediaSource::replayStreamId(stream, params)` (bọc `replayInternalStream`, một chỗ định nghĩa id).
- `findPlaybackStream`: với `.live2.mp4` tính id từ query của `url` (mặc định `auto`/`hi`/`0` như lúc
  tạo source; `token`, `session_id` không ảnh hưởng). Không đoán theo tiền tố `.replay.*` để khỏi đổi
  nhầm luồng của người xem khác.
- Tài liệu API: `url` phải kèm đúng `quality`, `prefered`, `duration` của luồng đang phát.
- Kiểm: harness RPL-15b (`live2.mp4` `quality=auto`, 2x, 20 s media ≈ 11 s).
- Ngoài phạm vi: HLS replay (`/vod/<stamp>/hls.m3u8`) chưa có trong danh sách đuôi của API.
- ✅ Đã làm 2026-09-27. Camera giả: URL trần và URL kèm `quality=auto&prefered=hi` đều `code 0`, 20 s
  media mất 11,2 s; `quality=lo` khi đang phát `auto` trả `906001` (không đổi nhầm luồng). Harness
  `check replay`: RPL-15 và RPL-15b PASS. Unit test 161/161.

**Bước 2 — vấn đề 16.** Đưa `duration` vào `MP4Reader` per-stream, xử lý trong timeline như replay gộp.
- `duration` = khung thời gian `[stamp, stamp + duration]`, giống replay gộp (bản ghi có khoảng hở thì
  nhận ít hơn).
- Id chỉ đổi khi `duration > 0` (dùng `replayStreamId`); không có `duration` thì giữ
  `cam/<stream>/vod/<stamp>`, nên client cũ, RPL-17 (đuổi bản ghi mới) và `loadMP4File` /
  `seekRecordStamp` / `setRecordSpeed` không đổi. Người có `duration` dùng source riêng.
- Truyền params vào đường tạo source (`find_l(from_mp4)` → `createFromMP4`).
- `MultiMP4Demuxer`: eof khi vượt `duration`, `findNextSegment` không nối file sau điểm đó. Reader tự
  huỷ khi hết, mọi protocol nhận EOF (nhờ bản sửa vấn đề 14).
- ~~Giới hạn `duration` trong `HttpSession` giữ làm lớp bảo vệ thứ hai.~~ Đã bỏ 2026-09-28: lớp session
  tính mốc từ gói đầu của người xem và `close(false)` đóng source dùng chung; `duration` chỉ còn tác
  dụng qua stream id + reader, `?duration` trên luồng live không còn tác dụng.
- `findPlaybackStream` với `.live.mp4` dùng cùng `replayStreamId`.
- Kiểm: unit test `MultiMP4Demuxer` dừng đúng `duration`; E2E trên camera giả `.live.mp4`/RTSP/TS có
  `duration`, speed per-stream, hai người xem khác `duration` cùng stamp.
- ✅ Đã làm 2026-09-27: `MediaSource::perStreamReplayId`, `findAsync_l` → `find_l` → `createFromMP4`
  mang `duration` vào tuple params, `MP4Reader` gọi `MultiMP4Demuxer::setMaxDuration`, demuxer eof khi dts
  vượt `stamp + duration` và không nối file sau đó; `findPlaybackStream` dùng cùng id. Kiểm:
  - Unit test `PerStreamReplayDurationTest` (2 test; tắt điều kiện dừng thì fail: dts cuối 19 800 ms).
    `ctest -R unit.` 161/161.
  - Camera giả, `duration=10`: `.live.mp4` 10,3 s, `.live.ts` 10,2 s, RTSP 10,2 s tới EOF (trước đây TS và
    RTSP bỏ qua `duration`). Hai người xem cùng stamp `duration` 6 và 14: 6,1 s và 14,2 s. Không
    `duration`: như cũ. Speed kèm `?duration=20`: 20 s media mất 11,3 s. `check replay`: RPL-15/15b PASS.
  - ffmpeg báo `Input/output error` khi server đóng luồng HTTP không có độ dài (rc 0), và RTSP replay báo
    `non monotonically increasing dts` (có cả khi không `duration`, có từ trước).
  - Một lần `stop` server kẹt sau "The program exit is completed" (phải `kill -9`), không lặp lại ở lần
    sau (13 s). Chưa rõ nguyên nhân, cần theo dõi.

### Vấn đề mới phát hiện khi làm P8 + P1

- **9 — Download extract luôn 401.** `extractArchived/download` dùng `CHECK_USER_AUTHOR("key")`, tra
  `UserAuthorManager` theo `(key, jwt)`. Cache này chỉ được ghi theo `device_id` (`WebHook.cpp`), nên
  key extract không bao giờ có trong cache. Hướng sửa: kiểm quyền theo camera của job
  (`CHECK_USER_DEVICE_AUTHOR_ASYNC(ffmpeg->getCameraId(), …)`) như `progress`.
  ✅ Đã sửa trong [`WebApiExtract.cpp`](../server/WebApiExtract.cpp) đúng theo hướng này, thêm cả
  `CHECK_USER_PERMISSION(EXTRACT_PERMISSION_CODE)` như `progress`/`delete`. Còn E2E EXT-01/03.
- **10 — Thumbnail có overlay lỗi.** `recordedThumnail` ([`WebApiPlayback.cpp`](../server/WebApiPlayback.cpp))
  và `bookmark/recordThumbnail` ([`WebApiBookmark.cpp`](../server/WebApiBookmark.cpp)) đặt file SVG
  overlay là `<pos>.overlay` / `<t>.bookmark.overlay`. ffmpeg (librsvg) chỉ nhận dạng SVG theo đuôi,
  nên `movie=` lỗi (đã thử: `.svg` được, `.overlay` không). Extract đúng vì dùng `.overlay.svg`.
  Sửa: thêm đuôi `.svg`. ✅ Đã sửa: `<pos>.overlay.svg` / `<t>.bookmark.overlay.svg`, giống extract.
  Còn E2E RPL-19, BKM-02.
- **11 — Người xem sau vào giữa luồng.** Source replay dùng chung theo id (stamp + quality + prefered +
  duration). Người xem thứ hai trong lúc source còn sống sẽ nhận từ vị trí hiện tại. Cần chốt: tách
  source theo phiên (mỗi request một reader) hay chấp nhận.
  Đã chốt 2026-09-26: giữ như hiện tại, không sửa.
- **12 — RPL-15.** `playback/speed` 2x: lần đầu (config mẫu) 10,7 s, với config sinh từ code 16–23 s.
  Đi qua `MP4Reader` per-stream, không qua phần đã sửa. Cần so hai bộ config.
  Đã điều tra 2026-09-27, **không phải lỗi speed**:
  - So config mẫu với config sinh từ code (dump từ binary): không khoá nào liên quan tới nhịp đọc
    (`record.sampleMS`, `protocol.modify_stamp`, `paced_sender_ms`, `mergeWriteMS` đều giống nhau).
    Khác biệt thật nằm ở harness: lần sau có `overlay.use_watermark_asset=0` và camera bật overlay, nên
    request của admin (JWT `e2e-admin`) đi qua overlay transcode vì vấn đề 2 (role loại trừ bị xoá khi
    nạp template). `playback/speed` đặt tốc độ trên source gốc; transcode theo nhịp đó nhưng thêm độ
    trễ và tốn CPU encode 1080p ở 50 fps trên bản Debug.
  - Tái hiện tại chỗ (camera giả 1080p 25 fps bằng `stress_vms/rtsp_source.py`, config sinh từ code,
    đúng phép đo RPL-15, `-t 20`, đổi tốc độ sau 2 s):

    | Đường đi | 1x | 2x | 4x |
    |---|---|---|---|
    | Trực tiếp (admin được loại trừ, hoặc không overlay) | 20,8 s | 11,3 s | 6,8 s |
    | Overlay transcode (viewer có watermark) | 25,1 s | 14,6 s | 8,4 s |

    Cả hai đường đều đổi nhịp đúng; transcode chỉ trễ thêm ~4 s media (chia theo tốc độ). Trên camera
    thật cùng lúc ghi + motion, 16–23 s khớp với đường transcode.
  - Không sửa code: vấn đề 2 đã sửa nên admin trong RPL-15 không còn qua transcode. Còn chạy lại E2E
    RPL-15 (mong đợi ≈ 11 s).
- **13 — Khựng ~1 s ở biên file.** Đo nhịp replay auto và `lo`: 4 s media mất 5,1 s quanh biên file ghi
  kế tiếp. File kế tiếp cùng stream chỉ được mở khi file trước hết (không chuẩn bị trước như lần
  chuyển Main↔Sub).
  ✅ Đã sửa 2026-09-27 trong [`MergedMP4Reader.cpp`](../src/Record/MergedMP4Reader.cpp). Hai nguyên nhân:
  - Index ghi theo giây nguyên, nên file kế tiếp của cùng stream có thể chồng lên file trước tới 1 s.
    Planner cắt clip trước ở cuối block và cho clip sau bắt đầu ở giây thứ 1 trong file. Keyframe đầu
    file (0 ms) nằm trước điểm bắt đầu nên bị bỏ, và priming chờ keyframe kế tiếp (GOP 2 s), làm mất
    ~1 s hình. `prepareNextClip` đã có cách xử lý (mở trước, dời điểm chuyển về keyframe của file sau)
    nhưng chỉ áp dụng cho Main↔Sub. Sửa: bỏ điều kiện loại trừ cùng stream.
  - `readTimelineTick`: khi frame cuối của clip trước còn cách điểm bắt đầu clip sau vài chục ms,
    nhánh chờ khe hở `return` luôn và bỏ phí phần còn lại của tick (tới 500 ms), nên timeline tụt so
    với wall-clock ở mỗi lần chuyển clip. Sửa: chỉ `return` khi clip sau bắt đầu sau tick hiện tại.
  - Unit test `MergedReplayOpenTest.NextFileOfSameStreamPlaysWithoutGap` (fail trước khi sửa: hở
    1080 ms ở biên). Đo tại chỗ (camera giả 1080p, biên file có chồng 1 s, 12 s):

    | | Media nhận được | Wall-clock | Trễ tại biên |
    |---|---|---|---|
    | Trước | 9,88 s | 10,14 s | +0,5 s |
    | Sau (`hi` và `auto`) | 10,92 s | 10,63 s | không |

    Biên không chồng và per-stream `.live.mp4` không bị lỗi này.
- **14 — Kết nối không đóng khi hết `duration`.** Đo thời điểm nhận byte trên `live2.mp4` với
  `quality=auto` và `lo`: dữ liệu đến đúng nhịp 1x và đủ `duration` (tfdt cuối ≈ duration), nhưng EOF
  đến sau thêm ~30 s (duration 20 → EOF 49,9 s; 33 → 63 s). Player chờ vô ích, còn `ffmpeg` thử đọc
  lại và báo `Input/output error`. Cần tìm chỗ source/session chỉ đóng sau timeout thay vì khi reader
  báo hết timeline. Chưa đo với `MP4Reader` per-stream.
  ✅ Đã sửa 2026-09-27. Nguyên nhân: khi hết timeline, timer của `MergedMP4Reader` trả `false`, reader
  và muxer bị huỷ ngay, nhưng lambda `setReadCB` của session HTTP-fMP4 giữ `shared_ptr` tới
  `FMP4MediaSource` (thêm cùng tham số `duration`, `8b9aff00`). Source vì vậy còn sống và còn đăng ký,
  ring không bao giờ detach, và session chỉ đóng khi hết `http.keepAliveSecond` (harness 30 s,
  default code 15 s). Sửa trong [`HttpSession.cpp`](../src/Http/HttpSession.cpp): hai nhánh fMP4
  (`checkLiveStreamFMP4` và `checkLiveStreamFMP4ByApp`) giữ `weak_ptr`. HTTP-TS không giữ source nên
  không bị. `MP4Reader` per-stream không bị vì reader đọc tiếp quá `duration` và đóng theo giới hạn.
  Đo tại chỗ (camera giả 1080p, `duration=20`):

  | Luồng | Trước | Sau |
  |---|---|---|
  | `live2.mp4` `quality=auto`, admin | EOF 49,0 s (session timeout) | EOF 18,6 s (ring detached) |
  | `live2.mp4` `quality=auto`, viewer có watermark (transcode) | EOF 49,9 s | EOF 18,6 s |
  | `live.mp4` per-stream, admin / viewer | — | EOF 20,2 s / 23,3 s (giới hạn `duration`) |

  Còn lại: viewer đi qua overlay transcode chỉ nhận ~16 s video của 20 s. Transcode bắt đầu trễ ~2,5 s
  và bị huỷ cùng source khi hết timeline, nên đoạn cuối còn trong transcode bị mất (trước khi sửa cũng
  vậy: 15,6 s). Chưa sửa; E2E APF-02.

### Lần chạy E2E camera giả 2026-09-27 (vấn đề 17–21)

Camera giả hai nguồn RTSP (`stress_vms/rtsp_source.py`): Main 1920x1080, Sub 640x360, 25 fps, GOP 2 s,
chu kỳ 60 s gồm 30 s tĩnh, 15 s chuyển động, 15 s tĩnh; không có âm thanh. Không có MinIO nên không
chạy tier và mã hoá.

- **17 — thumbnail 504.** Default `ffmpeg.snap` đưa `-ss` lên trước `-i`; `FFmpegSnap::snap` chọn thứ tự
  tham số theo vị trí `-ss` và thay template cũ (default của `cfcffc00`, đã nằm trong `config.ini` các
  máy) bằng dạng mới. Giây 59 của file 1080p: 504 → 0,32 s.
- **18 — tên file 1970.** `strtoll` thay `atoi`. Cũng sửa luôn `start_time_ms` của `StreamSource` và
  `MediaGapFillManager` (cùng chỗ đọc).
- **19 — segment HLS có overlay 403.** Khi tìm source dẫn xuất, cắt stream id tại `/` đầu tiên sau
  `.transcode.`. Segment live và replay 200; segment gọi bằng session lạ vẫn 401.
- **20 — playlist dẫn xuất của người khác.** Stream dẫn xuất có tag khác policy người gọi luôn `403`,
  không phụ thuộc người gọi có cần overlay.
- **21 — tìm bookmark theo tag.** Thêm `description`, `tags_csv` vào `DECLARE_ENTITY(BookmarkIndex)`
  (bảng và migration đã có cột).
- Harness: `rtsp_source.py` bỏ qua RTCP interleaved (`$`) thay vì đóng phiên (camera giả tự rớt sau
  ~70 s); PRX-01 chờ tới 30 s cho source HLS của proxy được gỡ (giữ ~10–15 s là đúng thiết kế).

Kết quả: `auto-profile` 11/11 PASS (APF-01..07), `overlay` 8/8, `bookmark` + `proxy` 6/6, replay/extract
PASS trừ RPL-17 (vấn đề 6, đúng thiết kế) và RPL-04 (kiểm tay). Unit test 161/161.

## 4. Thứ tự làm và kiểm

| Bước | Nội dung | Kiểm |
|---|---|---|
| ✅ 1 | P8 (harness) | smoke/live/auth 28/28 với config sinh từ code |
| ✅ 2 | P1 | `unit.replay_all` 19/19; E2E APF-02..05, RPL-05/06 |
| 3 | P1b (✅, còn thử trình duyệt), P2 (✅, còn E2E) | unit test `tfdt`; thử trên trình duyệt; E2E overlay, RPL-13/14/22 |
| 4 | 9, 10, P5 | unit test tương ứng; E2E extract, thumbnail, bookmark |
| 5 | P3 (✅, còn E2E) | E2E APF-06, xem tay |
| 6 | P6, P7, 11, 12, 13 | E2E RPL-15/17; khởi động lại rồi replay ngay |
| 7 | Chạy lại toàn bộ trên camera thật | ≥ 10 phút `always` + ≥ 15 phút `RecordLowResAndMotion` (cần ≥ 2 sự kiện motion), tier, mã hoá |

Mỗi bước build `build-test` bằng `-j2` và chạy `ctest --test-dir build-test -R unit.` trước khi
chạy E2E.
