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
| 5 | P1 | Tìm bookmark theo từ khoá lỗi (`-200` / `-400`) | BKM-03a/03b | Có từ trước | Mở |
| 6 | P2 | Replay `.live.mp4` với `stamp = now − 90 s`: lần đầu không kết thúc sau 240 s, với config sinh từ code thì dừng sau 15 s kèm lỗi | RPL-17 | Chưa rõ | Mở |
| 7 | P2 | Request replay đầu tiên ngay sau khi khởi động trả `403` | (quan sát khi thử nghiệm) | Chưa rõ | Mở |
| 8 | P2 | Lỗi harness | BKM-01, ENC-05/13, config | Harness | ✅ Đã sửa |
| 9 | P1 | Tải file extract luôn `401 No permission on key` khi `enableAuthorize=1` | EXT-01/03 | Có từ trước | ✅ Đã sửa, chưa chạy lại E2E |
| 10 | P1 | Thumbnail có overlay lỗi: file SVG overlay thiếu đuôi `.svg`, ffmpeg không mở được | RPL-19, BKM-02 | `79e823d0` (2026-09-20) | ✅ Đã sửa, chưa chạy lại E2E |
| 11 | P2 | Người xem thứ hai của cùng một replay (cùng tham số) vào giữa luồng, không từ `stamp` | Replay mở song song | Thiết kế có sẵn | Mới, cần chốt |
| 12 | P2 | `playback/speed` 2x trên `.live.mp4` không còn đổi nhịp (20 s media mất 16–23 s) với config sinh từ code | RPL-15 | Chưa rõ | Mới, mở |
| 13 | P3 | Khựng ~1 s ở biên file ghi khi replay qua hai file liên tiếp cùng stream | APF (đo nhịp) | Chưa rõ | Mới, mở |
| 14 | P2 | Replay gộp (`auto`/`lo`) phát xong `duration` nhưng kết nối HTTP fMP4 còn mở thêm ~30 s | APF-02 (30 s media mất 61 s) | Chưa rõ | Mới, mở |

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
- Còn lại: E2E APF-06 (độ phân giải đầu ra không đổi, bằng Main), xem tay chất lượng đoạn Sub.

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

### P5 — Tìm bookmark theo từ khoá (vấn đề 5)

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

### P6 — Replay đoạn đang ghi không kết thúc (vấn đề 6)

- RPL-17: `stamp = now − 90 s`, `.live.mp4` per-stream, vẫn chưa kết thúc sau 240 s. Kỳ vọng: phát
  hết dữ liệu hiện có (~90 s) rồi kết thúc.
- Việc cần làm: ghi log tại thời điểm mở replay (danh sách file, `end_time` của file đang ghi) và khi
  replay đọc tới cuối file đang mở. Xác định replay đang đọc theo file đang lớn dần hay timeline lấy
  `end = now` rồi đợi. Sửa để lấy `end` cố định tại lúc mở.

### P7 — `403` ngay sau khi khởi động (vấn đề 7)

- Khi chạy lại server có sẵn bản ghi, request `quality=hi` đầu tiên (vài giây sau healthcheck) trả
  `403 View Overlay Required`, các lần sau thì bình thường. Nhiều khả năng `ViewOverlayPolicy` trả
  `Unavailable` vì cấu hình camera từ hook chưa nạp xong.
- Việc cần làm: xác nhận bằng log. Nếu đúng thì trả `503` (thử lại được) thay vì `403` khi cấu hình
  camera chưa sẵn sàng, để không lẫn với trường hợp thiếu quyền overlay thật.

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
- **12 — RPL-15.** `playback/speed` 2x: lần đầu (config mẫu) 10,7 s, với config sinh từ code 16–23 s.
  Đi qua `MP4Reader` per-stream, không qua phần đã sửa. Cần so hai bộ config.
- **13 — Khựng ~1 s ở biên file.** Đo nhịp replay auto và `lo`: 4 s media mất 5,1 s quanh biên file ghi
  kế tiếp. File kế tiếp cùng stream chỉ được mở khi file trước hết (không chuẩn bị trước như lần
  chuyển Main↔Sub).
- **14 — Kết nối không đóng khi hết `duration`.** Đo thời điểm nhận byte trên `live2.mp4` với
  `quality=auto` và `lo`: dữ liệu đến đúng nhịp 1x và đủ `duration` (tfdt cuối ≈ duration), nhưng EOF
  đến sau thêm ~30 s (duration 20 → EOF 49,9 s; 33 → 63 s). Player chờ vô ích, còn `ffmpeg` thử đọc
  lại và báo `Input/output error`. Cần tìm chỗ source/session chỉ đóng sau timeout thay vì khi reader
  báo hết timeline. Chưa đo với `MP4Reader` per-stream.

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
