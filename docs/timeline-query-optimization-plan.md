# Plan tối ưu luồng TimeQuery / recordedTimePeriod

Trạng thái (2026-10-14, nhánh `perf/timeline-query`): **P0–P3 đã làm**, JSON giữ nguyên (72 MD5 golden), số đo ở mục 6.
P4, P5 chưa làm vì P1–P3 đã đạt mục tiêu trên bản Debug; còn chạy thật trên Release và dữ liệu production.

Hiện tượng: camera lưu 6 tháng, `GET /media/esc/recordedTimePeriod` mất hơn 10 s mới trả timeline.
Console gọi `periodType=2&detail=1&motion=1` ([playback-flow.md](playback-flow.md)).

## 1. Luồng hiện tại

```text
WebApiPlayback.cpp  /media/esc/recordedTimePeriod      (pool api())
 └ SearchEngine::findTimePeriod
    ├ VmsResourceAssignmentImp::findAssignmentsOverlappingRange      → local_ranges / remote_ranges
    └ với MỖI local_range: findTimePeriodLocal
        ├ new TimeQuery(tuple)
        │   └ MultiTimeDemuxer::openFile(dir)  → mở MỌI *.s3db (+ .idx) của camera
        ├ [motion] query->getRecordedTimePeriod(vector<TimeRange>)  → đọc toàn bộ khoảng lần 1
        │          MotionSearch(tuple) → MultiMotionDemuxer::openDir → mở MỌI *.mblk
        ├ query->getRecordedTimePeriod(stream→date→hour)              → đọc toàn bộ khoảng lần 2
        │   └ TimeQuery::query: đọc block → copy TimeBlock vào vector → sort → callback
        ├ với MỖI (stream, giờ, range): splitRangeByTier()  → 1 truy vấn SQLite + StoragePool
        └ mergeTimePeriodResult(merged, data)  → resolvePeriods cho từng giờ
 └ val.toStyledString()
```

## 2. Ước lượng chi phí (6 tháng, 1 camera, 2 stream, segment 60 s)

Số liệu là ước lượng từ code và quy mô tham chiếu ở [storage-tiering-flow.md](storage-tiering-flow.md) mục 15;
P0 sẽ đo con số thật.

| # | Điểm nóng | Vị trí | Khối lượng | Ghi chú |
|---|---|---|---|---|
| H1 | `splitRangeByTier` gọi SQLite cho từng range kết quả | [SearchEngine.cpp:134](../manager/Local/SearchEngine.cpp#L134), gọi ở [:938](../manager/Local/SearchEngine.cpp#L938) | `periodType=2,detail=1`: ~2 × 4 320 giờ = **~8 640 truy vấn**; `periodType=0`: 1 truy vấn/block = **~518 000** | `queryByCamera` lọc `camera_id = ? AND end_time >= ? AND start_time <= ?`, index `(camera_id, stream_id, start_time, end_time)` chỉ dùng được tiền tố `camera_id` → mỗi lần quét hết range của camera; cache pool chỉ sống trong một lần gọi. **Nghi là nguyên nhân chính của >10 s.** |
| H2 | Mở mọi timefile bất kể khoảng hỏi | [TimeDemuxer.cpp:144](../manager/Local/TimeDemuxer.cpp#L144), [MotionDemuxer.cpp:216](../src/Motion/MotionDemuxer.cpp#L216) | ~180 `.s3db` + 180 `.idx` + 180 `.mblk` mỗi request, kể cả khi chỉ hỏi 1 ngày | mỗi file: `acquireRead` (có thể chờ swap), `fopen`, buffer 64 KB, đọc first stamp. Trên HDD lạnh tốn hàng giây. Đây là lý do query **ngắn** cũng chậm dần theo thời gian lưu. |
| H3 | Đọc timefile hai lần khi `motion=1` | [SearchEngine.cpp:756](../manager/Local/SearchEngine.cpp#L756) | ~518 000 block × 2 | lần 1 chỉ để lọc motion theo đoạn có ghi. |
| H4 | `TimeQuery::query` copy cả `TimeBlock` (protobuf, nhiều string) vào vector rồi sort | [TimeQuery.cpp:79](../manager/Local/TimeQuery.cpp#L79) | ~518 000 bản sao, ước ~100 MB đỉnh | `readBlock` cấp `string` mới cho mỗi block. |
| H5 | Gọi `getTimeStr` (localtime + strftime) 4 lần cho mỗi chunk giờ, `minute_str`/`second_str` không dùng | [TimeQuery.cpp:259](../manager/Local/TimeQuery.cpp#L259) | ~2 triệu lần gọi | |
| H6 | Mỗi local_range tạo `TimeQuery` mới → mở lại toàn bộ file | [SearchEngine.cpp:1150](../manager/Local/SearchEngine.cpp#L1150) | số local_range = số lần camera về lại node này | nhân H2 lên. |
| H7 | Gộp kết quả local vào `merged` rỗng vẫn chạy `resolvePeriods` cho từng giờ, rồi `toStyledString()` | [SearchEngine.cpp:306](../manager/Local/SearchEngine.cpp#L306), [WebApiPlayback.cpp:61](../server/WebApiPlayback.cpp#L61) | 8 640 lần resolve; JSON thụt lề lớn gấp 2–3 lần | |

File rebuild `.<date>.s3db` không bị đọc nhầm: `File::scanDir` mặc định bỏ file ẩn
(`MultiTimeDemuxerTest.DirectoryScanIgnoresRebuildTempFile`).

## 3. Mục tiêu nghiệm thu

Trên bộ dữ liệu giả của P0 (1 camera, 2 stream, 180 ngày, segment 60 s, có tier range và motion), bản Release:

| Truy vấn | Mục tiêu |
|---|---|
| 1 ngày, `periodType=2,detail=1,motion=1` | < 100 ms, **không phụ thuộc** số ngày lưu |
| 180 ngày, `periodType=2,detail=1,motion=1`, page cache ấm | < 1 s |
| 180 ngày, cùng tham số, sau `drop_caches` | < 3 s trên SSD |
| `periodType=0` 7 ngày | < 1 s |

JSON trả về phải giống hệt bản cũ (so sánh sau khi parse) cho mọi tổ hợp `periodType` 0/1/2 × `detail` 0/1 × `motion` × `statusGaps`.

## 4. Các bước

Thứ tự theo tỉ lệ lợi ích/rủi ro. Mỗi bước build, chạy unit và so JSON với bản cũ trước khi sang bước sau.

### P0 — Đo trước khi sửa

1. Thêm `Ticker` theo pha trong `findTimePeriodLocal`/`findTimePeriod`, in một dòng `DebugL` khi tổng > 500 ms:
   assignment, open timefile, motion, đọc block, build kết quả, split tier (số lần gọi + tổng ms), merge.
   Đo `toStyledString` ở handler.
2. Sinh bộ dữ liệu giả: test GTest `unit.timeline_perf` (DISABLED_ mặc định, chạy tay bằng
   `--gtest_also_run_disabled_tests`) dùng `TimeMuxer`/`TimeRecorder` ghi 180 file ngày × 2 stream,
   vài khoảng trống và block `isReplay`, `segment_tier_ranges` 1 range/stream/giờ (HOT gần, COLD xa, 1 pool object),
   `.mblk` thưa. Cũng dùng làm **golden**: lưu JSON của bản hiện tại cho từng tổ hợp tham số để các bước sau so.
3. Ghi bảng thời gian từng pha vào mục 6 của file này. Nếu H1 không phải nguyên nhân chính thì đổi thứ tự P1/P2.

### P1 — Một lần nạp tier range cho cả request (H1)

- `findTimePeriodLocal` gọi `SegmentTierRangeImp().queryByCamera(camera, start, end)` **một lần**, cộng một lần
  `StoragePoolImp` cho các pool xuất hiện, dựng bảng nhỏ `TierRangeIndex` (vector theo stream, đã sort theo `start_time`,
  cờ `restoreRequired` đã tính sẵn).
- `splitRangeByTier(index, stream_id, start, end)` chạy trên bộ nhớ: `lower_bound` theo `end_time` rồi đi tuyến tính,
  giữ nguyên quy tắc hiện tại (terminal status bị bỏ, khoảng hở → `HOT`, gộp piece liền kề). Bản `#ifndef ENABLE_TIER_STORAGE`
  giữ chữ ký tương đương.
- Không cần migration: chỉ còn một truy vấn/request, index tiền tố `camera_id` là đủ. Chỉ thêm index `(camera_id, start_time)`
  nếu P0 sau P1 cho thấy truy vấn này > 50 ms.
- Rủi ro: tiering đổi range giữa lúc đọc và lúc dựng → timeline lệch một chu kỳ, giống như hiện tại vì vốn không có transaction bao quanh.

### P2 — Chỉ mở file của các ngày cần đọc (H2, H6)

- `TimeQuery(tuple, path, start_time, end_time)` (tham số mặc định 0 = hành vi cũ cho `getFirstBlock`/`getLastBlock`,
  [Manager.cpp](../server/Manager.cpp), [HlsPlayback.cpp](../server/HlsPlayback.cpp)).
- `MultiTimeDemuxer::openFile(dir, start, end)`: lấy ngày từ tên file `YYYY-MM-DD.s3db`
  (`StrTimeUtils::getTsFromDateStr`, giờ địa phương như `TimeRecorder::resolveTargetPath`), chỉ mở file có ngày trong
  `[startOfDay(start) - 1 ngày, startOfDay(end)]`; ngày lùi 1 để lấy block bắt đầu cuối ngày trước.
  Tên không parse được thì mở như cũ.
- `MultiMotionDemuxer::openDir(base, start_ms, end_ms)` lọc tương tự theo tên `<date>.mblk`.
- `findTimePeriod` tạo `TimeQuery` một lần cho hợp của các local_range và truyền vào `findTimePeriodLocal`
  (chỉ gọi nội bộ, không đổi API).
- Kiểm: unit cho file gap fill ghi vào ngày cũ, block vắt qua nửa đêm, tên file rebuild `.<date>.s3db`, thư mục rỗng (vẫn `NoTimeFileError`).

### P3 — Đọc timefile một lần và nhẹ hơn (H3, H4, H5)

- `TimeQuery::query` thu về struct nhỏ `{start, len, stream_idx, is_replay}` thay vì bản sao `TimeBlock`; vẫn sort vì
  gap fill có thể ghi block cũ vào cuối file. Các overload `getRecordedTimePeriod` trả `TimeBlock` (periodType 0, `queryBlocks`)
  giữ đường cũ.
- `readBlock` dùng lại một buffer `string` của demuxer thay vì cấp mới mỗi block.
- `motion=1`: tính `recorded_ranges` từ cùng kết quả đọc với timeline (thêm overload trả cả hai, hoặc dựng
  `vector<TimeRange>` từ kết quả theo stream), bỏ lần đọc thứ hai.
- Overload stream→date→hour: tính ranh giới giờ bằng `StampUtils::getStartOfHour`, cache chuỗi ngày theo `getStartOfDay`,
  bỏ `minute_str`/`second_str`; chỉ gọi `getTimeStr` khi sang ngày mới. Giữ nguyên xử lý giờ địa phương/DST hiện tại.

### P4 — Dựng và gửi JSON rẻ hơn (H7)

- `findTimePeriod`: khi chỉ có một local_range và `merged` chưa có dữ liệu, `swap` kết quả local vào `merged`
  thay vì `mergeTimePeriodResult`. Nhiều range (camera đổi node) vẫn merge như cũ.
- Handler `recordedTimePeriod`: ghi JSON không thụt lề (`Json::StreamWriterBuilder`, `indentation = ""`) thay vì
  `toStyledString()`. Kiểm FE/ESC không phụ thuộc định dạng (chỉ parse JSON) trước khi đổi.

### P5 — Cache tóm tắt theo ngày (chỉ làm nếu P1–P4 chưa đạt mục 3)

- Cache LRU trong bộ nhớ, khoá `(đường dẫn .s3db, size, mtime_ns, inode)`, giá trị là range đã gộp theo stream của cả ngày
  (struct nhỏ ở P3). Ngày đã đóng gần như bất biến; retention, rebuild, gap fill đổi size/mtime nên tự mất hiệu lực.
  File ngày hôm nay không cache.
- Giới hạn bộ nhớ qua config mới `record.timelineCacheMaxMB` (đăng ký bằng `onceToken`, mặc định nhỏ, 0 = tắt).
- Tier range **không** cache (đổi theo tiering cycle); P1 đã đủ rẻ.
- Rủi ro: mtime cùng giây khi ghi đè nhanh → dùng `st_mtim` nano giây + size.

### Ngoài phạm vi, báo team FE

- Nên hỏi theo cửa sổ đang hiển thị (ngày/tuần), và khi xem cả tháng thì dùng `periodType=2&detail=0` (bitmap giờ, không
  chạm tier range) rồi mới tải `detail=1` cho ngày được chọn. Không đổi hợp đồng API.
- Peer từ xa (`kBroadcastSyncTimeline`) hưởng lợi tự động vì owner chạy cùng code.

## 5. Kiểm thử

- Unit: các case mới của P1 (split trên bộ nhớ = split SQLite cũ trên cùng dữ liệu, gồm range terminal, khoảng hở,
  pool object/không object) và P2 (lọc file theo ngày). Bộ `unit.` đầy đủ phải pass.
- `unit.timeline_perf` (P0) so JSON golden và in thời gian từng pha trước/sau mỗi bước.
- Chạy thật: MediaServer Release trên bản copy timefile production (theo cách ở memory e2e camera thật), gọi API bằng `curl -w '%{time_total}'`
  cho 1 ngày / 30 ngày / 180 ngày, page cache ấm và sau `echo 3 > /proc/sys/vm/drop_caches`.
- Cụm 2 node: camera chuyển node giữa khoảng hỏi, kết quả gộp giống bản cũ.

## 6. Kết quả đo

`unit_timeline_query --gtest_also_run_disabled_tests --gtest_filter='*LargeRetention*'`: 1 camera, 2 stream, 180 ngày,
segment 60 s (518 400 block), 8 280 tier range, `.mblk` 12 đoạn/ngày. Bản **Debug**, page cache ấm, `/tmp` trên WSL.
Bản lạnh (`drop_caches`) chưa đo.

| Truy vấn | Gốc | Sau P1–P3 |
|---|---|---|
| console (`periodType=2,detail=1,motion=1`) 1 ngày | 210 ms | 22 ms |
| console 30 ngày | 2 672 ms | 210 ms |
| console 180 ngày | **16 807 ms** | **1 173 ms** |
| `periodType=2,detail=0` 180 ngày | 1 811 ms | 592 ms |
| `periodType=1,detail=1` 180 ngày | 3 538 ms | 604 ms |
| `periodType=0` 7 ngày | 18 192 ms | 425 ms |

Phân rã request console 180 ngày (dòng `Slow timeline` trong log):

| Pha | Gốc | Sau |
|---|---|---|
| tách tier | 12 816 ms / 10 710 lần gọi | 71 ms / 10 710 lần (1 truy vấn SQLite) |
| motion (gồm lần đọc timefile đầu) | 1 506 ms | 516 ms |
| dựng kết quả (không tính tách tier) | 2 385 ms | 458 ms |
| `toStyledString` (2,4 MB) | 66 ms | 63 ms |

Khác với plan khi làm:

- P2: điều kiện mở file là `ngày ≤ end && ngày + 2 ngày > start` (không dùng `getStartOfDay`, để không lệch khi đổi DST).
  Motion lọc trong `MotionSearch` bằng `MultiMotionDemuxer::addFile`, không đổi `openDir` ở `src/Motion`.
  Hàm dùng chung `dayFileInWindow()` nằm ở `TimeDemuxer.h`.
- P3: thay cho overload mới, `TimeQuery::queryRanges()` giữ kết quả của cửa sổ vừa đọc, nên lọc motion và dựng timeline
  của cùng request chỉ đọc timefile một lần. Sort dùng đúng `std::sort` + comparator cũ để thứ tự block trùng giờ bắt đầu
  không đổi. Các overload trả `TimeBlock` (`periodType=0`, `queryBlocks`, playback, tiering) đi đường cũ, không cache.
- P0: log `Slow timeline …` mức Info khi một lần `findTimePeriodLocal` > 500 ms hoặc mở timefile > 500 ms; giữ lại để đo trên production.
- Test: `unit.timeline_query_all` (golden 72 tổ hợp, tách tier, cửa sổ mở file, block qua nửa đêm). Test đặt `TZ=UTC` và
  gọi `toolkit::local_time_init()` vì `getTimeStr()` cache múi giờ lúc khởi tạo tĩnh.

Lỗi phát hiện khi làm, đã sửa sau P3 (đổi response `periodType=0`, golden 24 tổ hợp `t0/*` cập nhật):

- `periodType=0` luôn trả `isReplay=false`: giá trị lấy từ piece tier (`tp.isReplay`), nay lấy từ block (`p.is_replay()`).
- `periodType=0` gộp block của mọi stream vào một lần `resolvePeriods`: main và sub ghi cùng giây nên cắt nhau thành mảnh
  ~120 s, stream nào bắt đầu sau thắng phần chồng (3 ngày giả: 577 mảnh, tổng thời lượng bằng một stream). Nay resolve riêng
  từng `(cameraId, streamId)` rồi sort theo `startTime` (50 period, mỗi stream đủ thời lượng).
- `resolvePeriods` gộp đoạn liền kề không xét `isReplay`, đoạn replay sát đoạn ghi thường mất cờ. Nay giữ riêng, trừ danh sách
  cấp camera `periodType=1&detail=0` (replay của stream này chồng ghi thường của stream kia, giữ riêng chỉ ra mảnh vụn).
- `StampUtils::getStartOfDay/getStartOfHour/getStartOfMinute` và `StrTimeUtils::getWeekTime` dùng `std::localtime` (buffer
  dùng chung mọi thread) rồi sửa tại chỗ trước `mktime`; nay dùng `localtime_r`/`localtime_s` trên biến cục bộ. Lỗi rộng hơn
  timeline: tiering, rebuild, recorder, record policy cũng gọi song song.
- Test: `TimelineQueryTest.KeepsReplayPeriodsApart` (giờ replay ở cả 3 periodType, sub không bị main cắt),
  `StampUtilsTest.FloorsConsistentlyAcrossThreads`. Cần báo FE/ESC: `periodType=0` nay trả đủ từng stream và `isReplay` thật.
