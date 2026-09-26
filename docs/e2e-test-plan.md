# Kịch bản kiểm thử E2E MediaServer (trừ ONVIF)

Tài liệu này là danh mục kịch bản kiểm thử toàn bộ tính năng của MediaServer trên **một
camera thật có 2 luồng RTSP (Main + Sub)** và **một MinIO dành riêng cho test**. Mục tiêu:
xác nhận live, ghi hình theo mọi chế độ, replay, lưu trữ đa tầng và mã hoá hoạt động đúng
end-to-end, không chỉ ở mức unit test.

Harness chạy kèm: [`tests/integration/e2e_real/`](../tests/integration/e2e_real/README.md).
Các kịch bản có cột **Auto = A** được `e2e_check.py` tự kiểm; **M** là kiểm tay (xem mắt,
đo đồng hồ, thao tác mạng) theo đúng bước ghi trong bảng.

## 0. Phạm vi

**Trong phạm vi:** ingest RTSP, live mọi giao thức, xác thực JWT/quyền camera, overlay
(watermark + privacy mask), ghi hình theo lịch/chế độ, retention, replay (per-stream, auto
profile, HLS), timeline/thumbnail/motion search/bookmark, trích xuất video, storage tiering
HOT/WARM/COLD với MinIO, restore, mã hoá bản ghi, proxy/push/RTP, API vận hành, đồng bộ
cluster, độ bền (mất nguồn, restart, đầy đĩa, soak).

**Ngoài phạm vi (ONVIF):** PTZ và preset, `device/discovery`, `subnetScan`,
`onvifGetConfigProfiles`, `onvifSetVideoConfigs`, video config rule, `imageMoveControl`,
`relayOutputControl`, `validateCredential`, đồng bộ thẻ SD (`device/historical/*`, dựa
trên ONVIF Profile G), speaker ONVIF. Camera được nạp với `onvif.mainStream/subStream=AUTO`
và URL RTSP trực tiếp nên không cần ONVIF để có luồng.

**Không kiểm được với bản build hiện tại:** WebRTC/WHIP/WHEP (`ENABLE_WEBRTC=OFF`), SRT
(`ENABLE_SRT=OFF`), GB28181 RTP proxy (`ENABLE_RTPPROXY=OFF`). Các kịch bản tương ứng
được ghi `N/A-build`; muốn chạy phải build lại với cờ đó.

## 1. Rà soát test hiện có

### 1.1 Tổng quan

| Nhóm | Vị trí | Số case | Phủ gì | Không phủ |
|---|---|---|---|---|
| Unit giao thức | `tests/unit/{rtsp,rtmp,rtcp,http,codec,extension,common}` | ~130 | parser RTSP/RTMP/AMF/FLV, RTCP, HTTP splitter/cookie/body, H26x, frame factory, stamp | không có session thật, không có mạng |
| Unit HLS/record | `tests/unit/record` | ~35 | HlsMaker, HlsMediaSource, MP4 IO, MPEG muxer, recorder, planner replay, MergedMP4Reader, overlay policy | file MP4 sinh tổng hợp; không có camera thật, không đo pacing |
| Unit overlay/HLS guard | `tests/unit/http/test_hls_overlay_guard.cpp`, `server/test_ffmpeg_overlay_filter.cpp` | 14 | tag policy, filter FFmpeg | không render thật, không kiểm hình |
| Unit manager | `tests/unit/manager` | ~130 | CameraController, sync apply, timefile, record/storage policy, resource monitor, motion search, dispatcher | StreamSink với luồng thật, lịch ghi theo giờ thật |
| Unit storage | `tests/unit/storage`, `server/test_storage_pool_api.cpp` | ~50 | schema, entity, extract job, tier txn lock, pool API | — |
| Integration tier | `tests/integration/tier_storage` | 79 | TierStorageManager HOT→NAS/MinIO, restore, retention, concurrency, recovery | chỉ trên dữ liệu giả; không có recorder chạy thật, không có replay qua HTTP |
| Integration cluster | `tests/integration/sync_cluster` | 5 phase | sync ESC DB 2 node, failover | camera giả, không ghi hình thật |
| Stress | `tests/integration/stress_vms` | 9 phase | 300 cam/600 luồng, lịch ghi, retention, failover, fd/RSS | nguồn giả 1 profile, không kiểm nội dung replay, không overlay, không mã hoá |
| Demo cũ | `tests/*.cpp` (test_server, test_pusher, test_bench_*…) | — | chương trình tay kiểu ZLMediaKit | không có assert, không chạy trong ctest |

### 1.2 Lỗ hổng chính mà kịch bản E2E phải lấp

1. **Không có test nào chạy MediaServer với camera thật 2 profile khác độ phân giải.** Auto
   profile (Main↔Sub) chỉ được kiểm trên file tổng hợp; chưa ai xác nhận chuyển profile giữa
   hai file thật không hụt GOP, không nhảy timestamp (P2.4 trong
   `overlay-auto-profile-fix-plan.md` còn mở).
2. **Replay chưa kiểm pacing thời gian thực** (1x phải phát ~1 giây media mỗi giây) và
   chưa kiểm điểm bắt đầu đúng `stamp`.
3. **Overlay chưa kiểm bằng mắt / bằng khung hình** trên HLS/fMP4 live và replay.
4. **Tier + mã hoá + replay chưa từng chạy cùng nhau** trên luồng recorder thật: `.mp4.enc`
   lên MinIO → restore → giải mã → phát.
5. **Không có test cho ghi theo lịch/chế độ với chuyển động thật** (RecordOnlyMotion,
   RecordLowResAndMotion, pre/post record).
6. **Độ bền:** mất nguồn giữa chừng, restart MediaServer khi đang ghi/đang replay, MinIO
   rớt giữa lúc upload — chưa có kịch bản tự động.

### 1.3 Vấn đề phát hiện khi rà soát

- `tests/unit/record/test_merged_mp4_reader.cpp` **chưa được `git add`** nhưng
  `tests/unit/CMakeLists.txt` (đã stage) đã tham chiếu tới nó trong `unit_replay`. Commit
  hiện tại mà thiếu file này sẽ làm hỏng configure của mọi build `ENABLE_TESTS=ON`.
- Case `integration.tier-manager.RejectsUnreadablePlaybackFile` tự `Skipped` trong user
  namespace không đổi được UID; Skipped không được tính là pass.
- Các demo ở `tests/*.cpp` không có kết luận đạt/trượt, không dùng làm bằng chứng.
- **Lỗi — excluded role của watermark bị bỏ (đã tái hiện):** `loadWatermarkTemplateFromMap`
  (`server/Manager.cpp`, từ commit `a8c8b0ae`) gán `option.watermarkExcludedRoleIds = ""`
  ngay sau khi `fromJson` đã đọc `watermarkExcludedRoleIds` của camera. Chạy thử harness:
  role được miễn cả watermark lẫn mask vẫn bị chuyển sang derived transcode và ảnh vẫn có
  watermark (mask thì được miễn đúng). OVL-04 FAIL.
- **Lỗi — replay merged 404 ở request đầu (đã tái hiện trên nguồn giả):**
  `/media/record/<cam>/vod/<stamp>.live2.mp4` (mọi `quality`) và HLS replay
  `/vod/<stamp>/hls.m3u8` trả 404. `MergedMP4Reader` mở clip ngay nhưng log
  `Audio track index 1 codec mpeg4-generic receive no data for long 45062ms`, và source
  `…/vod/<stamp>.replay.<md5>` chỉ đăng ký sau ~45 s (request 23:22:58 → đăng ký 23:23:43),
  trong khi `createFromMP4ByApp` gọi `MediaSource::find` ngay sau `openForReplay`. File ghi
  có track AAC có dữ liệu (audio câm do `protocol.add_mute_audio=1`). Replay per-stream
  `.live.mp4` không bị. RPL-05/06/13/14 và OVL-08/09 FAIL vì lỗi này; cần kiểm lại trên
  camera thật (có/không audio) — xem RPL-22.
- **Cần xác minh:** replay per-stream với `stamp=now-90s` không tự kết thúc sau 240 s
  (RPL-17). Hoặc server nối thêm bản ghi mới (trái với tài liệu "không chờ recording
  mới"), hoặc không đóng kết nối khi hết dữ liệu.
- **`/media/mserver/playback/speed` chỉ nhận URL tuyệt đối** (`http://host:port/media/...`);
  path trần trả `906001 Playback stream not found`. Tài liệu `playback-flow.md` không nói.
- **Đổi cấu hình camera làm cắt segment:** mỗi lần `api-config.json` đổi (ví dụ bật overlay),
  cả Main và Sub bị dừng/khởi động lại 2 lần, sinh các file 2 s. Cần đánh giá khi VMS đẩy cấu
  hình thường xuyên (REC-12).
- **Mặc định `overlay.use_watermark_asset=1`:** template watermark không có `overlayAssetId`
  thì mọi HTTP output trả `403 Watermark config does not contain overlayAssetId`, kể cả role
  được miễn (vì lỗi excluded role ở trên). Mask `regions` phải là mảng JSON và `maskType`
  chỉ nhận `SOLID|BLUR|PIXELATE`; sai thì `403 Privacy mask configuration is invalid`.
- **Cấu hình mẫu chặn media khi bật xác thực:** `conf/config.ini` để `hook.on_play=` rỗng,
  ghi đè giá trị mặc định `/api/camera/check-camera-of-user`. Bật
  `manager.enableAuthorize=1` với file này thì mọi phiên media trả "Unauthorized" mà không
  gọi hook nào (đã tái hiện khi chạy thử harness). Môi trường thật phải đặt `on_play`.
- **Hook bỏ qua `code` trong body:** `parse_http_response` (`server/WebHook.cpp`) để phần
  kiểm `code` trong `#if 0`; service API chỉ từ chối được bằng HTTP status khác 2xx.
- **Tài liệu timeline lệch code:** `docs/api-timeline-thumbnail.md` mô tả
  `data=[{start,end}]`, còn `/media/esc/recordedTimePeriod` trả
  `data.periods[{streamId,startTime,timeLen,tier,...}]` (`periodType=0`) hoặc bucket theo giờ
  (`periodType=2`).

## 2. Môi trường và dữ liệu đầu vào

### 2.1 Cần người dùng cung cấp

| Biến | Ví dụ | Dùng cho |
|---|---|---|
| `E2E_RTSP_MAIN` | `rtsp://user:pass@10.0.0.5:554/Streaming/Channels/101` | luồng Main |
| `E2E_RTSP_SUB` | `rtsp://user:pass@10.0.0.5:554/Streaming/Channels/102` | luồng Sub |
| `E2E_S3_ENDPOINT` | `http://10.0.0.10:9000` | pool COLD MinIO |
| `E2E_S3_BUCKET` | `s3mk-e2e` (tạo sẵn, riêng cho test) | |
| `E2E_S3_ACCESS_KEY` / `E2E_S3_SECRET_KEY` | — | nhập bằng `read -s`, không ghi vào file |
| `E2E_NAS_PATH` (tuỳ chọn) | `/mnt/nas-test` | pool WARM kiểu NAS; bỏ trống thì dùng thư mục local làm WARM |

Yêu cầu với camera: Main và Sub **khác độ phân giải** (để phân biệt Main/Sub khi replay
auto), GOP ≤ 4 s, có audio thì kiểm thêm nhóm audio. Ghi lại codec (H.264/H.265), độ phân
giải, fps, GOP của từng luồng vào báo cáo trước khi chạy.

Credential không được đưa vào `config.ini`, log, báo cáo hay commit. Harness ghi chúng vào
`<work>/secrets.env` (quyền 600) và chỉ đọc lại khi gọi API.

### 2.2 Build

```bash
cmake --build build-test --target MediaServer -j2   # Debug: đọc được ./api-config.json
```

Build phải Debug (chỉ Debug đọc `api-config.json`), `ENABLE_TIER_STORAGE=ON`,
`ENABLE_AWS_SDK=ON`, `ENABLE_FFMPEG=ON` (overlay/thumbnail/extract), `ENABLE_MOTION=ON`.
Ghi `git rev-parse HEAD` + `git status --short` vào báo cáo.

### 2.3 Cấu hình harness đặt sẵn (khác mặc định)

| Khoá | Giá trị test | Lý do |
|---|---|---|
| `protocol.mp4_max_second` / `record.fileSecond` | 60 | nhiều segment trong thời gian ngắn |
| `protocol.enable_rtmp/enable_ts/enable_hls_fmp4` | 1 | kiểm mọi giao thức live |
| `manager.enableAuthorize` | 1 | bật xác thực JWT thật |
| `hook.on_play` | `/api/camera/check-camera-of-user` | file mẫu để rỗng, xem 1.3 |
| `manager.jwtPublicKey` | khoá RSA sinh trong `<work>` | harness tự ký JWT RS256 |
| `storage.tiering_split_granularity_seconds` | 60 | move tier trong vài phút thay vì 1 giờ |
| `storage.restore_ttl_seconds` | 600 | kiểm hết hạn cache restore trong 1 buổi |
| `storage.pool_health_check_interval_seconds` | 10 | phát hiện MinIO rớt nhanh |
| `record.timefileIdleCloseSec` | 60 | |

Chu kỳ tiering cố định **300 s** (`TierStorageManager::start`), không cấu hình được: mọi
kịch bản tier phải chờ tối thiểu 1–2 chu kỳ.

## 3. Quy ước

- **Mức độ:** P0 = chặn release; P1 = quan trọng; P2 = nên có.
- **Kiểm media bằng ffprobe/ffmpeg:** một URL "phát được" khi trong 15 s đọc được ≥ 3 s
  video, codec và độ phân giải khớp luồng nguồn, `ffmpeg -f null` không in lỗi giải mã,
  pts tăng đơn điệu.
- **Main/Sub nhận diện bằng độ phân giải** (đo ở bước SMK-02).
- **Mỗi kịch bản** ghi: kết quả (PASS/FAIL/BLOCKED/N/A), bằng chứng (lệnh + output rút
  gọn / ảnh), commit, thời điểm. FAIL phải kèm log `MediaServer` quanh thời điểm lỗi.
- Không tính `SKIPPED` là PASS.

## 4. Danh mục kịch bản

### SMK — Khởi động và smoke

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| SMK-01 | P0 | A | Khởi động MediaServer với `api-config.json` chứa 1 camera (Main+Sub) | `/media/mserver/healthcheck` 200 trong 30 s; log không có `## signal:` |
| SMK-02 | P0 | A | `getMediaList` sau ≤ 20 s | 2 luồng `app=<cam>`, stream `main`,`sub` online; ghi codec/độ phân giải/fps từng luồng |
| SMK-03 | P1 | A | `version`, `getServerConfig`, `getThreadsLoad`, `getWorkThreadsLoad`, `getStatistic`, `/media/mserver/systemStatistic` | code 0, không treo > 2 s |
| SMK-04 | P1 | A | Healthcheck p95 trong suốt các nhóm test | ≤ 1000 ms (poller không bị việc blocking chiếm) |

### ING — Ingest luồng camera

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| ING-01 | P0 | A | Pull Main+Sub qua TCP (`rtpTransport=TCP`) | cả 2 online, bitrate > 0 trong `getMediaInfo` |
| ING-02 | P1 | M | Đổi `rtpTransport=UDP`, reload config | online lại; không mất gói nhiều (`loss` trong getMediaInfo < 1%) |
| ING-03 | P0 | M | Rút mạng camera 30 s rồi cắm lại | luồng offline → tự reconnect ≤ 30 s sau khi có mạng; bản ghi có khoảng hở đúng bằng thời gian mất, không segment hỏng |
| ING-04 | P1 | A | `disableSubStream=true` | chỉ Main online; replay `quality=lo` trả 404, `auto` phát Main |
| ING-05 | P1 | A | `disableMainStream=true` | ngược lại ING-04 |
| ING-06 | P2 | A | `disableAudio=true` (khi camera có audio) | track audio biến mất khỏi getMediaInfo và bản ghi |
| ING-07 | P1 | M | Sai mật khẩu RTSP trong URL | luồng không online, log báo 401, không crash, retry có backoff |
| ING-08 | P2 | M | Camera đổi độ phân giải giữa chừng (đổi trên web camera) | luồng reconnect/cập nhật track; bản ghi segment mới có độ phân giải mới |

### LIV — Live đa giao thức

Mọi URL live gắn `token=<JWT>` khi `enableAuthorize=1`.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| LIV-01 | P0 | A | RTSP `rtsp://host:rtsp/<cam>/main` và `/sub` | phát được; độ phân giải khớp nguồn |
| LIV-02 | P1 | A | RTMP `rtmp://host:rtmp/<cam>/main` | phát được (H.264; H.265 cần enhanced RTMP) |
| LIV-03 | P0 | A | HTTP-FLV `/media/live/<cam>/main.live.flv` | phát được |
| LIV-04 | P1 | A | HTTP-TS `/media/live/<cam>/main.live.ts` | phát được |
| LIV-05 | P0 | A | HTTP-fMP4 `/media/live/<cam>/main.live.mp4` | phát được, bắt đầu bằng init segment |
| LIV-06 | P0 | A | WS-fMP4 `ws://…/main.live.mp4` | nhận `ftyp`+`moov` rồi `moof` liên tục |
| LIV-07 | P0 | A | HLS `/media/live/<cam>/main/hls.m3u8` không `session_id` | 302 sang URL có `session_id`; playlist hợp lệ, segment tải được |
| LIV-08 | P1 | A | HLS fMP4 `/main/hls.fmp4.m3u8` | playlist có `#EXT-X-MAP`, phát được |
| LIV-09 | P1 | A | HLS master `/media/live/<cam>/hls.master.m3u8` | liệt kê cả main/sub, URI con giữ `session_id` |
| LIV-10 | P0 | A | `/media/live/<cam>.live2.mp4?quality=hi` / `lo` / `auto&prefered=hi` | hi = độ phân giải Main, lo = Sub, auto = Main |
| LIV-11 | P1 | A | `quality=auto&prefered=hi` khi Main bị disable | fallback sang Sub |
| LIV-12 | P1 | M | 2 tab HLS khác `session_id` cùng camera | `getMediaList.readerCount` / viewer count = 2; hết request 60 s thì giảm |
| LIV-13 | P1 | A | `session_id` > 128 ký tự hoặc ký tự lạ | HTTP 400 |
| LIV-14 | P1 | A | `kick_sessions` / `close_streams` | player bị ngắt; camera stream tự lên lại (luồng camera không bị xoá) |
| LIV-15 | P2 | M | Độ trễ live: đồng hồ trên camera vs màn hình | RTSP/FLV/fMP4 < 1.5 s; HLS < 3×segDur + 2 s |
| LIV-16 | P1 | A | Snapshot `getSnap?url=rtsp://127.0.0.1/<cam>/main` | trả JPEG hợp lệ đúng độ phân giải |
| LIV-17 | P2 | M | Reverse proxy có prefix (`http.original_url_header`) | redirect HLS và cookie path mang prefix gốc |
| LIV-18 | — | — | WebRTC WHEP, SRT | N/A-build |

### AUTH — Xác thực và quyền

Harness ký JWT RS256 bằng khoá riêng của lần chạy. Hook `on_play`
(`/api/camera/check-camera-of-user`) của mock API từ chối các `user_id` trong
`<work>/deny_users.json`.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| AUTH-01 | P0 | A | Live fMP4 không token | 401/403, không trả byte media |
| AUTH-02 | P0 | A | Token chữ ký sai / hết hạn (exp quá 5 phút leeway) | từ chối |
| AUTH-03 | P0 | A | Token đúng, `project_id` khác `media_server.projectId`, `level≠0` | từ chối |
| AUTH-04 | P0 | A | Token đúng nhưng hook `on_play` từ chối user | từ chối trên mọi giao thức (HTTP, RTSP, RTMP) |
| AUTH-05 | P1 | A | Token qua header `Authorization: Bearer` thay cho query | chấp nhận |
| AUTH-06 | P1 | M | Đang xem live rồi thêm user vào deny list | trong ≤ 10 phút + 60 s (TTL cache quyền + timer) session bị đóng |
| AUTH-07 | P1 | A | API storage với JWT thiếu `150301`/`150302` | `901006`/`901007` |
| AUTH-08 | P1 | A | Replay/timeline với JWT thiếu `1003` | `901004` |
| AUTH-09 | P1 | A | `/index/api/*` sai `secret` | từ chối |

### OVL — Overlay (watermark + privacy mask)

Camera được nạp `watermarkTemplateId` trỏ tới một template trong `watermark_configs` và
`privacyConfig.enabled=true` với một vùng mask. Role test: `admin` (excluded khỏi cả hai),
`viewer` (không excluded), `wm-only` (excluded chỉ khỏi privacy mask).

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| OVL-01 | P0 | A+M | Live HTTP-fMP4 role `viewer` | stream thực tế là derived `.transcode.`; ảnh chụp có watermark + vùng mask bị che |
| OVL-02 | P0 | A+M | Live HLS role `viewer` | redirect sang playlist derived có tag policy; segment có overlay |
| OVL-03 | P0 | A | HLS derived playlist gọi với JWT của role khác tag | 403 |
| OVL-04 | P0 | A+M | Role `admin` (excluded cả hai) | phát source gốc, không transcode |
| OVL-05 | P1 | M | Role `wm-only` | chỉ có watermark, không mask |
| OVL-06 | P1 | A | JWT `overlay=false` | phát source gốc |
| OVL-07 | P0 | A | RTSP/RTMP role `viewer` | phát source gốc (không overlay theo thiết kế P0), không 403 |
| OVL-08 | P0 | A+M | Replay HTTP-fMP4 `live2.mp4?quality=auto` role `viewer` | overlay giữ nguyên qua lần chuyển Main↔Sub |
| OVL-09 | P0 | A+M | Replay HLS `/media/record/<cam>/vod/<stamp>/hls.m3u8` role `viewer` | overlay có; playlist không lộ đường dẫn file |
| OVL-10 | P1 | M | HLS: segment không mang token, reload playlist có token đổi role | policy segment theo lần reload playlist gần nhất |
| OVL-11 | P1 | A | Xoá cấu hình camera rồi replay | 403 (không xác định được policy) |
| OVL-12 | P2 | M | Template watermark có IMAGE (asset tải từ `/api/media-server/watermark/assets`) | ảnh hiện đúng vị trí/opacity |
| OVL-13 | P1 | M | CPU khi 3 viewer khác policy xem cùng camera | ≤ 1 transcode cho mỗi tag policy (không nhân theo viewer) |

### REC — Ghi hình theo chế độ và lịch

`dh="<ngày>,<giờ>"`, ngày `0` = Thứ Hai (`RecordPolicy.cpp:40`). Kịch bản lịch dùng giờ
hiện tại ±1 để thấy chuyển mốc trong buổi test.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| REC-01 | P0 | A | `RecordAlways` 24/7 trong 10 phút | Main và Sub đều có file; `isRecordingMP4=true` cả hai; segment ≈ 60 s; ffprobe mọi file đóng đều hợp lệ |
| REC-02 | P0 | A | `NoRecord` | không file mới; `isRecordingMP4=false` |
| REC-03 | P0 | A | `enableRecording=false` | như REC-02 bất kể lịch |
| REC-04 | P0 | A+M | `RecordLowResAndMotion`, không chuyển động | Sub ghi liên tục, Main không ghi |
| REC-05 | P0 | M | `RecordLowResAndMotion`, vẫy tay trước camera 20 s | Main có đoạn từ `t_motion − pre` đến `t_end + post` (pre/post ≥ 5 s) |
| REC-06 | P0 | M | `RecordOnlyMotion` | Main chỉ ghi theo sự kiện, Sub không bao giờ ghi |
| REC-07 | P1 | M | Chuyển động kéo dài > 1 segment, nhiều sự kiện chồng nhau | 1 đoạn liên tục, không cắt/khởi động lại recorder giữa chừng |
| REC-08 | P0 | A | Lịch: giờ hiện tại `RecordAlways`, giờ kế tiếp `NoRecord` | tại mốc giờ recorder dừng ≤ 60 s sau mốc |
| REC-09 | P1 | A | Lịch khác ngày trong tuần (ngày hiện tại không có ô nào) | không ghi |
| REC-10 | P0 | A | `notRecordSubStream=true` + `RecordAlways` | chỉ Main ghi |
| REC-11 | P1 | A | `notRecordMainStream=true` + `RecordAlways` | chỉ Sub ghi |
| REC-12 | P1 | A | Đổi chế độ khi đang ghi (Always → NoRecord → Always) qua reload `api-config.json` | segment đang mở được đóng hợp lệ, không file 0 byte |
| REC-13 | P0 | M | `kill -9` MediaServer giữa segment, khởi động lại | segment dở được xử lý (bỏ hoặc sửa), timeline không có range ảo, ghi tiếp bình thường |
| REC-14 | P1 | A | `startRecord`/`stopRecord`/`isRecording` qua `/index/api` | đúng trạng thái |
| REC-15 | P1 | A | Tên file/thư mục bản ghi và timefile (`.s3db`/`.idx`) | đúng layout `<root>/record/<cam>/<stream>/…`; timeline khớp file thực tế |
| REC-16 | P1 | M | Ghi liên tục 24 h | số segment ≈ 1440/luồng, không khoảng hở ngoài lúc mất nguồn, RSS/fd phẳng |
| REC-17 | P2 | A | Camera có audio: file có track audio AAC/G711 đúng codec | |

### RET — Retention (tier bật)

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| RET-01 | P0 | A | `keepArchivedMaxFor=1` (giờ), có bản ghi > 1 h | sau ≤ 2 chu kỳ tiering phần > 1 h bị xoá; timeline cập nhật |
| RET-02 | P1 | A | `keepArchivedMinFor` > tuổi dữ liệu khi áp lực dung lượng | phần trong Min không bị xoá |
| RET-03 | P1 | M | `keepArchivedMaxForAuto=true` | mốc lấy từ `archiveStartTime`, không xoá quá tay |
| RET-04 | P1 | A | `deleteRecordDirectory` | thư mục/ngày bị xoá, timeline không còn |
| RET-05 | P1 | M | Build `ENABLE_TIER_STORAGE=OFF`: hạ `storage.limitPercentUsage` dưới mức dùng đĩa | `enforceStoragePolicy` xoá bản ghi cũ nhất (xem stress `storage`) |

### RPL — Replay

`stamp` luôn lấy từ `/media/esc/recordedTimePeriod`. Pacing: đọc 30 s media phải tốn
28–35 s đồng hồ ở 1x.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| RPL-01 | P0 | A | Timeline `recordedTimePeriod` (detail 0/1) | khoảng thời gian khớp file thực tế ± 1 segment |
| RPL-02 | P0 | A | `/media/record/<cam>/main/vod/<stamp>.live.mp4` | phát được, độ phân giải Main, pacing 1x |
| RPL-03 | P0 | A | Như trên cho `sub` | độ phân giải Sub |
| RPL-04 | P0 | A+M | Điểm bắt đầu: `stamp` ở giữa segment | khung đầu tiên cách `stamp` ≤ 1 GOP (so đồng hồ OSD camera với `stamp`) |
| RPL-05 | P0 | A | `live2.mp4?quality=hi` / `lo` | chỉ Main / chỉ Sub; thiếu dữ liệu tại stamp → 404 |
| RPL-06 | P0 | A | `quality=auto&prefered=hi` trên đoạn chỉ có Sub (REC-04) | phát Sub; khi tới đoạn có Main (sự kiện REC-05) chuyển Main |
| RPL-07 | P0 | M | Chuyển Main↔Sub: không hụt GOP, không đứng hình > 1 s, pts liên tục | |
| RPL-08 | P1 | A | Đoạn Main ngắn hơn `replayMinSwitchSec` (5 s) | không chuyển qua lại |
| RPL-09 | P0 | A | Khoảng hở ≤ `replayMaxGapSec` (2 s) giữa 2 file | phát tiếp |
| RPL-10 | P0 | A | Khoảng hở > 2 s (tạo bằng ING-03) | replay kết thúc tại khoảng hở, không treo |
| RPL-11 | P1 | A | `stamp` ngoài timeline | 404 |
| RPL-12 | P0 | A | WS-fMP4 replay | như RPL-02 |
| RPL-13 | P0 | A | HLS replay `/media/record/<cam>/vod/<stamp>/hls.m3u8?quality=auto` | 302 sang opaque id; playlist không chứa đường dẫn `.mp4`; segment phát được |
| RPL-14 | P1 | A | HLS fMP4 replay `hls.fmp4.m3u8` | như trên |
| RPL-15 | P1 | A | `/media/mserver/playback/speed` 2x, 4x, 0.5x | pacing đổi tương ứng (30 s media trong ~15 s / ~7.5 s / ~60 s) |
| RPL-16 | P1 | M | Seek: đóng kết nối, mở `stamp` mới | phát từ stamp mới, không rò session (getMediaList giảm sau ≤ 30 s) |
| RPL-17 | P1 | A | Replay đoạn đang ghi (stamp = now − 90 s) | phát tới hết dữ liệu hiện có rồi kết thúc, không chờ bản ghi mới |
| RPL-18 | P1 | A | 5 replay song song cùng camera khác stamp | tất cả đúng pacing, CPU hợp lý |
| RPL-19 | P1 | A | Thumbnail `recordedThumnail?pos=<stamp>` và `pos=latest` | JPEG hợp lệ; lần 2 trong 60 s lấy từ cache |
| RPL-20 | P2 | A | `getMP4RecordFile`, `loadMP4File` + `seekRecordStamp` + `setRecordSpeed` | code 0, luồng phát lại được |
| RPL-21 | P1 | M | Audio không tương thích giữa block Main/Sub | replay video-only, không lỗi |
| RPL-22 | P0 | A | Request đầu tiên `live2.mp4` / HLS replay, camera có audio và camera không audio (`add_mute_audio=1`) | trả 200/302 ngay, không 404, thời gian tới byte đầu ≤ 3 s |

### MOT / BKM — Motion search và bookmark

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| MOT-01 | P1 | M | `motionDetectConfig.clientEnabled=true`, `chooseStream=SECONDARY`, tạo chuyển động | có sự kiện trong timeline `motion=1` |
| MOT-02 | P1 | M | `/media/esc/searchMotion` với ROI phủ vùng chuyển động / ROI ngoài vùng | có kết quả / rỗng |
| BKM-01 | P1 | A | bookmark `create → detail → update → search → recent → mostUsedTags → delete` | code 0, dữ liệu khớp |
| BKM-02 | P1 | A | `bookmark/recordThumbnail` | JPEG tại thời điểm bookmark |
| BKM-03 | P2 | A | Bookmark nằm trong vùng bị retention xoá (RET-01) | bookmark bị dọn hoặc trả lỗi rõ ràng |

### EXT — Trích xuất video

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| EXT-01 | P0 | A | `extractArchived/previews` rồi `create` (mp4, 60 s) → poll `progress` → `download` | file tải về dài 60 s ± 1 GOP, đúng độ phân giải stream chọn |
| EXT-02 | P1 | A | `create` lặp lại cùng tham số | cùng `key` |
| EXT-03 | P1 | A | Định dạng `mkv`, `avi` | file hợp lệ |
| EXT-04 | P1 | M | `stampSource=true`, camera có overlay | video có dấu nguồn + watermark + mask |
| EXT-05 | P0 | A | Cửa sổ extract nằm ở COLD (sau TIR-05) | tự restore, rồi xuất đúng nội dung |
| EXT-06 | P1 | A | Cửa sổ extract trên bản ghi mã hoá | file xuất ra là plaintext phát được |
| EXT-07 | P1 | A/N/A | Job service `extractArchived/job` với `uploadUrl` presigned HTTPS của MinIO | chỉ chạy được khi MinIO có HTTPS; callback `video-extractions/result` tới mock |
| EXT-08 | P2 | A | `delete` và hết `ffmpeg.delay_close_sec` | file bị xoá |

### TIR — Lưu trữ đa tầng (MinIO)

Chuẩn bị: pool WARM (NAS hoặc thư mục local), pool COLD MinIO. Policy test:
HOT `retain_until_days=0` → WARM `1` → COLD, `min_segment_age_minutes_before_move=1`.
Với `retain_until_days=0` và độ mịn 60 s, segment được move sau ~1–2 chu kỳ 300 s.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| TIR-01 | P0 | A | `pool/options`, `mountpoint/available` | HOT/WARM: LOCAL_DISK/NAS; COLD: NAS/MINIO |
| TIR-02 | P0 | A | `pool/testConnection` MinIO đúng / sai secret / sai bucket | `OK can_read=can_write=true` / `914005` |
| TIR-03 | P0 | A | `pool/create` MinIO COLD, `pool/detail` | credential bị mask; `capacity_status` hợp lệ (AVAILABLE hoặc UNAVAILABLE khi thiếu quyền metrics — không được hiển thị 0 như thật) |
| TIR-04 | P0 | A | Policy HOT→COLD (WARM tắt), assign camera, `effectivePolicy` | `policy_source=CAMERA` |
| TIR-05 | P0 | A | Chờ 2 chu kỳ: `tieringJob/list` có job `DONE`; object tồn tại trên MinIO; file HOT đã xoá; `camera/timeline` báo `COLD` | |
| TIR-06 | P0 | A | `playback/resolve` range COLD | `914009 RESTORE_REQUIRED` |
| TIR-07 | P0 | A | `restoreJob/create` → poll `detail` tới `READY` → replay `.live.mp4` range đó | phát được, nội dung/độ dài khớp trước khi move |
| TIR-08 | P0 | A | `storage.auto_restore_on_record_access=1`: replay thẳng range COLD | server tự tạo restore job; retry sau khi READY thì phát được |
| TIR-09 | P1 | A | `restoreJob/create` lặp lại cùng tham số | `job_created=false`, cùng `job_id` |
| TIR-10 | P1 | A | Chờ quá `restore_ttl_seconds` | job `EXPIRED`, cache bị dọn, resolve trở lại `RESTORE_REQUIRED` |
| TIR-11 | P0 | A | Policy HOT→WARM→COLD đủ 3 tầng | dữ liệu qua WARM rồi COLD; replay đọc được ở WARM không cần restore |
| TIR-12 | P0 | M | Chặn MinIO (firewall) khi có job PENDING | pool `OFFLINE` sau ngưỡng health; job giữ `PENDING` (không FAILED); resolve `914004 STORAGE_OFFLINE`; mở lại → job tự chạy tiếp |
| TIR-13 | P1 | M | Chặn MinIO giữa lúc upload file lớn | job không `DONE` giả; HOT không bị xoá trước khi object tồn tại |
| TIR-14 | P1 | A | Validation policy: HOT disabled, `retain_until_days` giảm dần, `high ≥ critical`, `delete_after_days` < max retain | lỗi `914007/914008/902008` tương ứng |
| TIR-15 | P1 | A | Xoá pool đang được policy dùng / xoá default HOT / xoá policy default | `914003` / `914029` / `914030` |
| TIR-16 | P1 | A | `policy/clone`, `removeCamera` → fallback `policy-system-default` | |
| TIR-17 | P0 | A | `delete_policy.require_approval_before_delete=true`, `delete_after_days` nhỏ | range `EXPIRED` trong `expiredSegment/list`; `extend` đưa về `AVAILABLE`; `approve` xoá vật lý (object MinIO biến mất) |
| TIR-18 | P0 | A | `protected/create` trên range sắp hết hạn | không bị xoá tới khi xoá protected |
| TIR-19 | P1 | M | Move sớm theo áp lực: hạ `high_watermark_percent` HOT dưới mức dùng hiện tại | job `job_trigger=PRESSURE` |
| TIR-20 | P1 | M | Restart MediaServer khi có job `RUNNING` | job về `PENDING` rồi hoàn tất; không object/range mồ côi |
| TIR-21 | P1 | M | Xoá tay 1 object trên MinIO | reconcile đánh dấu `MISSING`; replay range đó báo rõ, không crash |
| TIR-22 | P2 | A | `dashboard/summary`, `dashboard/detail`, `alert/list`, `alert/ack` | số liệu khớp pool/job |
| TIR-23 | P1 | A | `storage.object_read_mode` presigned: thumbnail/extract range COLD không restore | đọc trực tiếp qua URL presigned |
| TIR-24 | P2 | A | Bản `/media/api/storage/*` (secret) cho cùng thao tác | payload giống bản JWT |

### ENC — Mã hoá bản ghi

Mã hoá chỉ áp dụng cho pool HOT và cho bản ghi **sau** khi bật. Chạy với cả `encryptMode=1`
(Light) và `2` (Strong).

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| ENC-01 | P0 | A | Tạo pool HOT `encryptMode=1`, `encryptPassword` | segment mới là `*.mp4.enc`; đuôi file có magic `SENC`; Light giữ `ftyp` ở đầu |
| ENC-02 | P0 | A | `encryptMode=2` | như trên, **không** còn `ftyp` ở đầu file |
| ENC-03 | P0 | A | `ffprobe` trực tiếp file `.enc` (Strong) | không đọc được (không lộ nội dung) |
| ENC-04 | P0 | A | Live không bị ảnh hưởng khi bật mã hoá | LIV-01..10 vẫn đạt |
| ENC-05 | P0 | A | Replay `.live.mp4`, `live2.mp4?quality=auto`, HLS replay trên bản ghi mã hoá | phát được; `storage.decrypt_save_path` có plaintext tạm và bị dọn sau chu kỳ |
| ENC-06 | P0 | A | Tier: `.enc` lên MinIO (TIR-05) | object key là `.enc`; object không đọc được bằng ffprobe |
| ENC-07 | P0 | A | Restore + replay range mã hoá từ MinIO (TIR-07 trên `.enc`) | phát được |
| ENC-08 | P0 | A | Reconcile object không xoá range `.enc` (lỗi cũ 41b9dfc9) | range vẫn `AVAILABLE` sau ≥ 2 chu kỳ |
| ENC-09 | P1 | A | Presigned read với key `.enc` (lỗi cũ 602eb070) | rơi về restore, không trả dữ liệu mã hoá thô |
| ENC-10 | P1 | A | `pool/encrypt/changePassword` đúng/sai `old_password` | đúng: bản ghi cũ và mới đều replay được; sai: từ chối |
| ENC-11 | P1 | A | Đổi `encryptMode` 1→2 bằng `pool/update` | job mã hoá lại toàn kho chạy; replay vẫn được trong và sau job |
| ENC-12 | P2 | A | `encryptOldVideo=true` khi mode đang `none` | không tạo job (hành vi hiện tại) — ghi nhận |
| ENC-13 | P1 | A | Thumbnail, bookmark thumbnail, extract trên bản ghi mã hoá | đều ra plaintext hợp lệ |
| ENC-14 | P1 | M | Restart MediaServer | vẫn giải mã được bản ghi cũ (khoá theo key era còn) |

### PRX — Proxy, push, RTP, FFmpeg source

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| PRX-01 | P1 | A | `addStreamProxy` từ `E2E_RTSP_SUB` vào `app=proxy` → `listStreamProxy` → `delStreamProxy` | online rồi biến mất |
| PRX-02 | P1 | A | Publish RTSP/RTMP vào server bằng ffmpeg (`-f rtsp` / `-f flv`) | luồng online, phát lại được qua HTTP-FLV |
| PRX-03 | P2 | A | `addStreamPusherProxy` đẩy `<cam>/sub` sang `rtmp://127.0.0.1/push/x` | luồng `push/x` online |
| PRX-04 | P2 | A | `addFFmpegSource` / `delFFmpegSource` | online / offline |
| PRX-05 | P2 | A | `openRtpServer` + `startSendRtp` loopback + `listRtpServer` + `closeRtpServer` | luồng rtp online (cần `ENABLE_RTPPROXY`, hiện N/A-build) |

### CLU — Cluster 2 node

Dựa trên `tests/integration/sync_cluster` nhưng camera là camera thật.

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| CLU-01 | P1 | A | Camera trên node-a; timeline/thumbnail gọi vào node-b | node-b forward tới owner, kết quả giống node-a |
| CLU-02 | P1 | M | Failover: node-a nhả camera | node-b nhận, ghi tiếp; replay qua biên chuyển node đọc được cả hai phần |
| CLU-03 | P1 | A | So sánh DB ESC 2 node sau mỗi bước | `compare_db.py` khớp; `database is locked`=0 |

### RBS — Độ bền và hiệu năng

| ID | P | Auto | Kịch bản | Kỳ vọng |
|---|---|---|---|---|
| RBS-01 | P0 | M | Restart MediaServer (SIGINT) khi đang live + replay + ghi | dừng sạch ≤ 10 s, khởi động lại mọi thứ tự phục hồi |
| RBS-02 | P1 | M | Đĩa HOT đầy (loop device nhỏ) | pressure move/retention chạy; không crash; alert CRITICAL |
| RBS-03 | P1 | M | 20 viewer HTTP-fMP4 + 5 replay trong 30 phút | không lỗi, CPU/RSS ổn định |
| RBS-04 | P1 | M | Soak 24 h: ghi + tier + 1 viewer | RSS/fd/threads không tăng đơn điệu; healthcheck p95 ≤ 1 s |
| RBS-05 | P1 | M | Sai giờ hệ thống / đổi NTP (nhảy 1 phút) | timeline không có segment âm, replay không vỡ |

## 5. Thứ tự chạy

Dữ liệu của bước sau phụ thuộc bước trước, nên chạy theo thứ tự:

1. **Unit + integration hiện có** (`ctest -R 'unit\.'`, tier với `TIER_TEST_S3_*`). Không
   xanh thì dừng.
2. **SMK, ING-01, LIV, AUTH** (~15 phút).
3. **REC-01** chạy nền ít nhất 30 phút làm dữ liệu cho replay; trong lúc đó chạy **OVL**.
4. **RPL, MOT/BKM, EXT-01..04** trên dữ liệu REC-01.
5. **REC-04..REC-13** (đổi chế độ, cần thao tác chuyển động/mất mạng) → tạo khoảng hở và
   đoạn Main/Sub xen kẽ cho RPL-06..10.
6. **TIR** (≥ 30 phút, gồm chờ chu kỳ) rồi **EXT-05**.
7. **ENC** trên một work dir mới (pool HOT mã hoá từ đầu), lặp lại lõi LIV/RPL/TIR.
8. **PRX, CLU, RBS**; soak 24 h chạy qua đêm.

## 6. Mẫu báo cáo

```text
Commit: <sha> (+ dirty files)       Build: Debug, flags ...
Camera: <hãng/model>  Main: H.264 2560x1440 25fps GOP 50  Sub: H.264 640x360 15fps
MinIO: <phiên bản>, bucket test riêng
Kết quả: PASS x / FAIL y / BLOCKED z / N/A w
FAIL:
  RPL-07  chuyển Sub→Main đứng hình 2.3 s tại 10:32:15   log: <đoạn>   ảnh: <file>
```

`e2e_check.py` ghi `<work>/report.json` cho các kịch bản tự động; kịch bản M điền tay vào
cùng file báo cáo.
