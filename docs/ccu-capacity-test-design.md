# Thiết kế test tích hợp đo ngưỡng CCU của MediaServer

Mục tiêu: xác định **số người xem đồng thời (CCU) tối đa một node MediaServer phục vụ được**
trong khi node đó vẫn gánh đủ **300 camera / 600 luồng** (ingest + ghi hình theo lịch), và chỉ
ra **tài nguyên nào chạm trần trước**.

**Mức nghiệm thu: 400 CCU trên một node** (định nghĩa đạt ở mục 1.1).

Test này mở rộng harness sẵn có `tests/integration/stress_vms/` (nguồn RTSP giả, ONVIF giả,
service API giả, cohort lịch ghi, lấy mẫu tài nguyên). Harness hiện chỉ tạo tải **đầu vào**; nó
chưa có người xem nào. Phần thiếu là tải **đầu ra** và cách kết luận ngưỡng.

---

## 1. Định nghĩa

| Thuật ngữ | Nghĩa trong test này |
|---|---|
| **Viewer** | Một phiên phát ra ngoài: một kết nối RTSP, một kết nối HTTP/WS-fMP4, một phiên HLS (theo `session_id`), hoặc một phiên replay. |
| **Nền** | 300 camera / 600 luồng của profile `full`, cohort ghi hình như `stress_vms`, cộng tải REST nhẹ của `load_driver.py`. Nền luôn chạy, không phải phần được đo. |
| **Bậc** | Một mức viewer giữ cố định trong `CCU_HOLD_SEC`. Chỉ số của bậc lấy trong cửa sổ đo = phần giữ trừ 30 s đầu. |
| **CCU đạt** | Mức viewer của **bậc cao nhất mà mọi tiêu chí ở mục 6 đều đạt**. |
| **Yếu tố giới hạn** | Tiêu chí trượt đầu tiên ở bậc kế tiếp: CPU, poller nóng, NIC, bộ nhớ, đĩa (replay), hoặc chính bộ sinh tải. |

CCU luôn công bố kèm **hỗn hợp viewer**, **phân bố viewer theo camera**, **bitrate nguồn**, **cấu
hình phần cứng**. Một con số CCU thiếu bốn thứ này không so sánh được với lần chạy khác.

### 1.1 Tiêu chí nghiệm thu 400 CCU

Node **đạt nghiệm thu** khi, trên phần cứng đích với bản Release và nền 300 camera / 600 luồng:

| # | Điều kiện | Kịch bản |
|---|---|---|
| A1 | Bậc **400** hỗn hợp `vms` đạt mọi tiêu chí mục 6, với **cả** `hot` và `uniform` | `ccu_step`, chế độ `capacity` |
| A2 | Bậc 400 đạt ở chế độ `admission` với cấu hình production: không viewer nào trong 400 bị từ chối (kể cả do ngưỡng CPU/RAM 90%) | `ccu_step`, chế độ `admission` |
| A3 | 400 viewer mở đồng loạt trong 5 s đều vào được trong 30 s, không mất đoạn ghi | `ccu_storm` |
| A4 | Giữ 400 viewer 4 giờ: không crash, RSS/fd không tăng đơn điệu, không khoảng hở ghi hình | `ccu_soak` |
| A5 | Churn quanh 400 trong 30 phút, fd/đối tượng về mốc sau khi dừng | `ccu_churn` |

**Khuyến nghị thêm (không bắt buộc cho nghiệm thu)**: CCU đạt ở `capacity` nên ≥ **500** (dư 25%).
Nếu ngưỡng thật chỉ ~400 thì mọi dao động thực tế (một camera bitrate cao hơn, replay dồn vào một
lúc, bảo trì kho) đều đẩy node qua ngưỡng; báo cáo ghi rõ mức dư này.

Lưu ý cấu hình cho A2: ngưỡng reader toàn node là critical = `(int)(min(maxStream, streamMaxOfLicense) × 1.1)`,
nên `maxStream`/license phải ≥ 400 (ví dụ 400 → nhận tới 440). Đặt thấp hơn thì A2 trượt vì cấu
hình, không phải vì server.

## 2. Điều kiện để con số có nghĩa

Đây là phần dễ làm sai nhất; mỗi mục dưới đây đều có thể làm kết quả lệch cả bậc độ lớn.

### 2.1 Bitrate nguồn phải thật

`run_stress.sh` hiện dựng `loop.h264` 176x144, 5 fps, 48 kbps — đủ cho kiểm lịch ghi nhưng
**egress khi đo CCU sẽ thấp hơn thực tế ~50–100 lần**. Test CCU cần profile media riêng:

| Luồng | Độ phân giải | fps | GOP | Bitrate |
|---|---|---|---|---|
| main | 1920x1080 | 25 | 50 (2 s) | 4 Mbps CBR |
| sub | 640x360 | 25 | 50 (2 s) | 512 kbps CBR |

Ingest của nền khi đó ≈ 300 × 4,5 Mbps ≈ **1,35 Gbps**, ~120 k gói RTP/s. `rtsp_source.py` mới đo
được 32 Mbps / 4400 pps trên một tiến trình, nên phải chia nguồn thành nhiều tiến trình (mỗi tiến
trình một cổng, `setup_stress.py` chia camera theo cổng). Trước mỗi lần chạy có một bước **tự kiểm
nguồn**: tiến trình nguồn < 70% một lõi và độ lệch nhịp gửi p99 < 100 ms; không đạt thì dừng,
không đo.

### 2.2 Bản Release, tham số chạy như production

- Đo trên **Release** (hoặc RelWithDebInfo). Số liệu CPU của bản Debug không dùng được.
- Bản Release không đọc `api-config.json` (chỉ có dưới `ENABLE_DEBUG`, [server/Manager.cpp:1123](../server/Manager.cpp#L1123)).
  Nó lấy cấu hình qua hook `on_server_load2` = `/api/media-server/configuration-2nd-gen`, và khi
  `manager.apiSecret` rỗng thì nhận JSON thô không giải mã ([server/WebHook.cpp:704](../server/WebHook.cpp#L704)).
  → `mock_api.py` trả nội dung `api-config.json` ở path đó. Không cần sửa server.
- **Không** truyền `-t 4 --affinity 0` như `start_node()` hiện làm: dùng số poller mặc định (= số
  lõi) như production. Cấu hình còn lại theo **default trong code**, chỉ ghi đè cổng/đường dẫn
  (`conf/config.ini` không phải cấu hình production). Default liên quan: HLS bật và không theo yêu
  cầu, fMP4 bật, RTMP/TS tắt (`src/Common/config.cpp:216-238`).
- Bật `manager.enableAuthorize=1` và cấp JWT như `e2e_real` (`jwt_private.pem`), vì kiểm token +
  ghi phiên người dùng là chi phí thật của mỗi lần mở luồng
  ([server/WebHook.cpp:1176](../server/WebHook.cpp#L1176)). Chạy thêm một lượt `enableAuthorize=0` để
  tách riêng chi phí xác thực.

### 2.3 Tách máy sinh tải

MediaServer chạy một mình trên máy đích. Nguồn RTSP + ONVIF + mock API chạy trên máy nguồn; viewer
chạy trên một hoặc nhiều máy client. Nối bằng mạng thật (≥ 10 GbE nếu muốn thấy trần CPU thay vì
trần NIC). Trên máy dev (WSL, 8 lõi, 7 GB RAM) chỉ chạy profile `mid` với bitrate thấp để **kiểm
công cụ**, không để công bố số.

Bộ sinh viewer phải tự chứng minh nó không phải nút cổ chai (mục 5.3). Bậc nào client quá tải thì
bậc đó **không hợp lệ**, không phải "trượt".

### 2.4 Giới hạn người xem có sẵn trong server

Server tự từ chối viewer trước khi chạm trần phần cứng:

- `GlobalMonitor::isReaderResourceLimit` — CPU hoặc RAM ≥ ngưỡng critical (mặc định 90%) thì từ
  chối viewer thường ([manager/Server/GlobalMonitor.cpp:365](../manager/Server/GlobalMonitor.cpp#L365)).
- `ReaderMonitor::isReaderCountLimit` — tổng reader của node và reader theo camera
  ([manager/Server/ReaderMonitor.cpp:142](../manager/Server/ReaderMonitor.cpp#L142)), đặt từ
  `maxStream`/`streamMaxOfLicense`/`maxStreamPerCamera`/`streamMaxCameraOfLicense`.

Vì vậy có **hai chế độ chạy**, tách bạch:

| Chế độ | Cấu hình | Trả lời câu hỏi |
|---|---|---|
| `capacity` | `unlimitedStream=true`, `unlimitedStreamPerCamera=true`, `thresholdConfig` CPU/RAM = 100 | Phần cứng + code chịu được bao nhiêu viewer trước khi chất lượng xấu đi |
| `admission` | Giới hạn như production | Server có từ chối **sạch** (HTTP 503/`MaxRequest`) mà không làm hỏng viewer đang xem và luồng đang ghi |

Xem mục 9: đọc code phần này đã thấy hai lỗi làm giới hạn production sai.

## 3. Mô hình viewer

### 3.1 Hỗn hợp mặc định (`CCU_MIX=vms`)

| Loại | URL | Tỉ lệ | Ghi chú |
|---|---|---|---|
| Lưới live, luồng phụ | `/media/live/<cam>/<cam>-sub.live.mp4` (HTTP/WS-fMP4) | 55% | Web client xem lưới 16/25 ô |
| Toàn màn hình, luồng chính | `/media/live/<cam>/<cam>-main.live.mp4` | 15% | |
| HLS | `/media/live/<cam>/<stream>/hls.m3u8?session_id=…` | 10% | Mobile; không có `session_id` sẽ nhận 302 |
| RTSP pull | `rtsp://<node>:<rtsp>/<cam>/<stream>?token=…` (TCP) | 10% | NVR/AI bên thứ ba |
| Replay | `/media/record/<cam>/<stream>/vod/<stamp>.live.mp4` | 10% | Đọc đĩa + demux mỗi phiên; `<stamp>` rải trong 10 phút ghi gần nhất |

Transcode/overlay (`?transcode=1`, policy overlay) **không** nằm trong CCU chung: nó chạy trên
pool `ffmpeg()` (2 luồng) nên trần của nó là vài phiên, và có ngưỡng từ chối riêng ở mức warning.
Đo riêng bằng hỗn hợp `CCU_MIX=transcode`.

Các hỗn hợp khác để khoanh vùng: `CCU_MIX=fmp4-sub`, `fmp4-main`, `rtsp`, `hls`, `replay` — mỗi
cái 100% một loại. Chạy các hỗn hợp thuần trước; chúng cho **chi phí biên của từng giao thức**,
từ đó dự đoán được CCU cho hỗn hợp bất kỳ và kiểm lại bằng `vms`.

### 3.2 Phân bố viewer theo camera (`CCU_DIST`)

Chi phí khác hẳn giữa "nhiều người xem ít luồng" (một ring buffer, nhiều reader) và "mỗi người một
luồng" (nhiều muxer/HLS/reader riêng, replay mỗi phiên một file). Chạy cả hai:

- `hot` — Zipf s=1,1 trên 300 camera: 20% camera nhận ~80% viewer. Gần thực tế trung tâm giám sát.
- `uniform` — chia đều. Trường hợp xấu cho replay và cho phân tán giữa các poller.

Chỉ chọn camera thuộc cohort có luồng ổn định (`always`, `office`, `motion_night`, `lowres_motion`,
`retention_auto`, `long_retention`); replay chỉ chọn cohort đang ghi liên tục luồng tương ứng.

## 4. Kịch bản

Tất cả chạy trên **1 node** (CCU là chỉ số theo node), sau phase `ramp` của nền và khi ≥ 98% luồng
đã lên.

### 4.1 `ccu_step` — tìm ngưỡng

```
bậc:  0 → 100 → 200 → 300 → 400 → 500 → 600 → 800 → 1000 → 1250 → 1500 …
mỗi bậc: tăng dần với tốc độ CCU_JOIN_RATE (mặc định 20 phiên/s), giữ CCU_HOLD_SEC (mặc định 150 s)
```

- Bậc 0 là mốc chuẩn của nền (CPU, healthcheck, ghi hình) để so sánh.
- Bậc dày quanh 400 để thấy rõ đường cong trước và sau mức nghiệm thu. Bậc 400 giữ **300 s** thay vì
  150 s để cửa sổ đo phủ trọn một chu kỳ bảo trì kho (timer 300 s).
- Trượt ở bậc < 400 thì vẫn chạy tiếp tới hết `CCU_STEPS` mặc định là không cần: dừng theo luật dưới
  và báo **không đạt nghiệm thu** kèm yếu tố giới hạn.
- Dừng khi **hai bậc liên tiếp trượt**, hoặc có crash, hoặc nền bị hỏng (mục 6.2).
- Sau khi có bậc đạt cuối `N_ok` và bậc trượt đầu `N_fail`, **chia đôi** thêm tối đa 3 lần giữa
  hai mức (mỗi lần giữ lại đủ `CCU_HOLD_SEC`). Kết quả làm tròn xuống 50.
- Viewer của bậc trước giữ nguyên sang bậc sau (không ngắt rồi nối lại), đúng như tải thật tăng dần.

### 4.2 `ccu_storm` — mở đồng loạt

Từ mức 0, mở **400** viewer trong 5 s (A3), rồi lặp với `0,8 × CCU đạt` nếu CCU đạt > 500 (tường video khởi động lại, client mất mạng rồi nối
lại). Đo: tỉ lệ vào được, TTFF p95/max, healthcheck, và **mất frame ghi hình** của nền trong 60 s
đó. Tiêu chí: mọi viewer vào được trong 30 s, không mất đoạn ghi.

### 4.3 `ccu_churn` — vào/ra liên tục

Giữ trung bình **400** (A5), mỗi viewer xem thời lượng phân phối mũ trung bình 60 s rồi thoát,
thay bằng viewer mới; chạy 30 phút. Mục đích: rò fd/bộ nhớ/`MediaSource`/phiên HLS và phiên người
dùng trong DB. Tiêu chí: sau khi dừng churn 120 s, fd và số đối tượng của `getStatistic` về mức
bậc 0 ± 5%; RSS không tăng đơn điệu theo từng 5 phút.

### 4.4 `ccu_soak` — giữ lâu

Giữ **400** viewer hỗn hợp `vms` trong 4 giờ (A4). Kiểm RSS, fd, độ trễ tích luỹ của viewer, liên tục
của bản ghi, và chu kỳ bảo trì kho (300 s) không làm viewer giật.

### 4.5 `ccu_admission` — giới hạn production

Chế độ `admission`, giới hạn đặt thấp để chạm nhanh (ví dụ tổng 200 reader, 8 reader/camera):

1. Mở tuần tự tới vượt giới hạn: viewer thứ `limit+1` nhận từ chối rõ ràng, viewer cũ không bị ảnh hưởng.
2. **Burst**: mở `2 × limit` viewer đồng loạt trong 1 s. Số phiên được nhận không được vượt ngưỡng
   critical. Số reader được cập nhật **bất đồng bộ** qua `kBroadcastPlayerCountChanged`
   ([manager/Server/GlobalMonitor.cpp:764](../manager/Server/GlobalMonitor.cpp#L764)) nên phép kiểm
   "đếm rồi nhận" có thể để lọt quá ngưỡng khi mở đồng loạt; bước này đo mức vượt thật.
3. Ép CPU ≥ 90% (bằng viewer ở chế độ `capacity` trên node thứ hai hoặc tiến trình đốt CPU cố định)
   rồi kiểm viewer mới nhận `ResourceUnavailable`.

## 5. Bộ sinh viewer — `viewer_load.py`

### 5.1 Kiến trúc

- Python 3, chỉ thư viện chuẩn (như phần còn lại của harness), asyncio.
- Một **điều phối** + `W` tiến trình worker (mặc định = số lõi của máy client − 1), có thể chạy trên
  nhiều máy (`VIEWER_HOSTS`); điều phối gửi mức mục tiêu, worker báo số liệu mỗi 5 s qua file JSON
  (cùng kiểu `load_state.json` hiện có).
- Mỗi viewer là một coroutine: mở kết nối, đo, giữ, đóng khi được yêu cầu.

### 5.2 Viewer đo gì

Không giải mã video (quá đắt ở vài nghìn phiên). Đo từ lớp đóng gói:

| Giao thức | Mốc frame đầu (TTFF) | Thời gian media |
|---|---|---|
| fMP4 (HTTP/WS) | nhận `moof` đầu tiên sau `moov` | `tfdt` + tổng `trun` sample duration |
| RTSP/TCP | gói RTP đầu tiên sau `PLAY` 200 | RTP timestamp / 90000, tính tràn 32 bit |
| HLS | segment đầu tải xong | tổng `#EXTINF`; mỗi lần refresh playlist ≈ `segDur` |
| Replay | như fMP4 | như fMP4 |

Từ đó mỗi viewer có: **TTFF**, **byte/s**, **độ trễ trôi** `lag(t) = (t − t0) − (media(t) − media0)`,
và **stall** = khoảng > 1 s media không tiến (HLS: segment tải lâu hơn `segDur`). Mã lỗi HTTP/RTSP
và lý do đóng (server đóng, timeout, reset) được đếm riêng.

Để tiết kiệm CPU client: 95% viewer là **light** (chỉ đọc header box/RTP, bỏ qua payload bằng
`readexactly` và đếm byte, không lưu), 5% là **probe** (thêm lấy mẫu `lag` từng giây để vẽ). TTFF và
stall tính cho mọi viewer.

### 5.3 Tự kiểm bộ sinh tải

Mỗi worker báo CPU của chính nó và độ trễ vòng lặp asyncio (task hẹn giờ 100 ms, đo lệch). Bậc bị
đánh dấu **client-bound** nếu bất kỳ worker nào > 85% một lõi hoặc lệch vòng lặp p99 > 200 ms.
Thêm một phép đối chiếu: số phiên client thấy mở phải khớp `getAllSession` / số reader trong
`getMediaList` phía server (± 1%).

## 6. Tiêu chí đạt của một bậc

### 6.1 Phía viewer (tính trong cửa sổ đo)

| Chỉ số | Ngưỡng |
|---|---|
| Tỉ lệ vào được (chế độ `capacity`) | ≥ 99,5% |
| TTFF p95 — fMP4/RTSP live | ≤ 2,5 s (GOP 2 s: phải chờ keyframe) |
| TTFF p95 — HLS | ≤ 8 s |
| TTFF p95 — replay | ≤ 3 s |
| Thời gian stall / thời gian xem | ≤ 0,5% |
| Phiên có ít nhất một stall > 2 s | ≤ 1% |
| Độ trễ trôi trong cửa sổ đo (p95 theo phiên) | ≤ 1 s |
| Byte/s trung bình theo phiên / bitrate nguồn | ≥ 0,97 |
| Phiên bị server đóng ngoài ý muốn | 0 |

### 6.2 Phía server và nền — viewer **không được** làm hỏng ghi hình

Dùng lại các bất biến của `check_stress.py` và thêm:

| Chỉ số | Ngưỡng |
|---|---|
| Crash (`## signal:`), `database is locked`, `Failed to apply` | 0 |
| `/media/mserver/healthcheck` p95 / max (poller có sạch không) | ≤ 200 ms / ≤ 1000 ms |
| Luồng nền còn online | ≥ 99% luồng khả dụng |
| Trạng thái ghi khớp lịch cohort | 100% camera đủ điều kiện |
| Khoảng hở ghi hình mới (timeline của camera `always`) | 0 khoảng > 2 × `fileSecond` |
| CPU tiến trình | ≤ 80% tổng lõi (chừa headroom) |
| Poller nóng nhất (`/index/api/getThreadsLoad`) | ≤ 85% |
| RSS | ≤ 70% RAM máy, không tăng đơn điệu trong cửa sổ đo |
| NIC egress | ≤ 80% băng thông liên kết — vượt thì ghi yếu tố giới hạn = NIC |

Tiêu chí "khoảng hở ghi hình" là điểm khác biệt quan trọng nhất so với benchmark streaming thông
thường: với VMS, CCU chỉ có giá trị nếu **ghi hình vẫn nguyên vẹn** ở mức CCU đó.

## 7. Thu thập số liệu

Mở rộng bộ lấy mẫu 5 s đang có trong `run_stress.sh` (`resources.jsonl`):

- **CPU theo luồng**: `/proc/<pid>/task/*/stat` gộp theo tên luồng (poller, work thread, pool
  `api`/`device`/`ffmpeg`, SQLite). Poller nóng là trần điển hình khi CPU tổng còn thấp.
- `/index/api/getThreadsLoad` và `/index/api/getWorkThreadsLoad` (độ trễ + tải mỗi poller).
- `/index/api/getStatistic` (số `MediaSource`, `TcpSession`, `Buffer`, … — phát hiện rò đối tượng).
- `/proc/net/dev` của NIC phục vụ: byte ra/vào mỗi giây.
- `ss -tni state established '( sport = :<http> or sport = :<rtsp> )'`: phân bố `Send-Q`. Send-Q đầy
  diện rộng = server đẩy không kịp (NIC hoặc client); đầy ở vài phiên = viewer chậm.
- Đọc đĩa (`/proc/<pid>/io`, `iostat`) cho replay.
- Phía client: file số liệu của mỗi worker như mục 5.

## 8. Kết quả

`<work>/ccu_report.json` và `<work>/ccu_report.md`, mỗi bậc một dòng:

```
bậc  viewer  egress   CPU%  poller_max  healthcheck_p95  TTFF_p95  stall%  lag_p95  ghi_hình  kết_luận
   0      0   0.0G    31%      22%           4 ms            -        -        -       OK       mốc
 800    800   1.1G    48%      37%           6 ms          1.3 s    0.02%   0.1 s      OK       đạt
1000   1000   1.4G    57%      45%           9 ms          1.6 s    0.05%   0.2 s      OK       đạt
1250   1250   1.8G    71%      91%         340 ms          4.9 s    2.1%    3.4 s      OK       trượt: poller nóng
```

Dòng kết luận: `CCU đạt = N (hỗn hợp, phân bố, bitrate, phần cứng); giới hạn bởi <yếu tố>`, cộng
chi phí biên mỗi loại viewer (CPU%/viewer, Mbps/viewer) từ các lượt hỗn hợp thuần.

**Ước lượng trước để đặt kỳ vọng**: hỗn hợp `vms` ở bitrate mục 2.1 trung bình ~1,3 Mbps/viewer,
nên **400 CCU ≈ 520 Mbps egress**; cộng 1,35 Gbps ingest là ~1,9 Gbps qua node. Trường hợp xấu
(400 viewer đều xem luồng chính, hỗn hợp `fmp4-main`) là 1,6 Gbps egress. Nghĩa là:

- Node dùng **một cổng 1 GbE chung** cho ingest và egress **không thể** đạt 400 CCU ở bitrate này,
  bất kể CPU — riêng ingest đã vượt. Cần 10 GbE, hoặc tách cổng ingest (camera) và egress (client)
  với egress ≥ 1 GbE cho hỗn hợp `vms` (và > 1,6 Gbps nếu phải chịu `fmp4-main`).
- Nếu phần cứng đích chỉ có 1 GbE thì phải chốt lại bitrate camera thật trước khi đo; test báo
  "giới hạn bởi NIC" là kết luận đúng chứ không phải lỗi của test.
- Phía client, 400 viewer ~520 Mbps nằm trong sức của một máy client với 2–4 tiến trình worker;
  lượt tìm ngưỡng trên 1000 mới cần thêm máy.

## 9. Lỗi giới hạn người xem phát hiện khi thiết kế — ĐÃ SỬA (1539590f)

1. Ngưỡng reader **toàn node** từng lấy nhầm `min(maxStreamPerCamera, streamMaxCameraOfLicense)`
   ([server/Manager.cpp:1033](../server/Manager.cpp#L1033)); với `maxStreamPerCamera=8` cả node
   từ chối từ viewer thứ 9. Nay dùng `min(maxStream, streamMaxOfLicense)`.
2. Giới hạn **theo camera** từng dùng `>=` nên thiếu 1 viewer
   ([manager/Server/ReaderMonitor.cpp:155](../manager/Server/ReaderMonitor.cpp#L155)); nay dùng `>`
   như ngưỡng toàn node.

Hành vi sau sửa mà `ccu_admission` phải kiểm: ngưỡng critical = `(int)(value × 1.1)`, toàn node và
theo camera đều nhận tới đúng critical reader (ví dụ `maxStream=400` → 440, `maxStreamPerCamera=8`
→ 8). Bước burst (4.5.2) vẫn cần chạy: số reader cập nhật bất đồng bộ nên mở đồng loạt có thể vượt
ngưỡng, hai bản sửa này không đụng tới phần đó.

## 10. Tích hợp vào harness

Theo nguyên tắc của harness (thêm scenario/phase, không viết script song song):

| Thay đổi | File |
|---|---|
| Scenario `ccu`, `ccu_storm`, `ccu_churn`, `ccu_soak`, `ccu_admission`; `NODES=1`; bỏ `-t 4 --affinity 0` khi `CCU_PRODUCTION_ARGS=1` | `run_stress.sh` |
| `--media-profile realistic` (dựng clip main/sub mục 2.1), chia nguồn nhiều cổng, chế độ `capacity`/`admission`, `enableAuthorize=1` + khoá JWT | `setup_stress.py` |
| Nhiều tiến trình / nhiều cổng; tự kiểm nhịp gửi | `rtsp_source.py` |
| Phục vụ `/api/media-server/configuration-2nd-gen` từ `api-config.json` (cho bản Release) | `mock_api.py` |
| Bộ sinh viewer + điều phối nhiều máy | `viewer_load.py` (mới) |
| Tiêu chí mục 6, chia đôi ngưỡng, báo cáo mục 8 | `check_ccu.py` (mới, dùng lại hàm thu thập của `check_stress.py`) |
| Lấy mẫu CPU theo luồng, NIC, `getThreadsLoad`, `getStatistic` | bộ lấy mẫu trong `run_stress.sh` |

Biến môi trường mới: `CCU_MODE` (`capacity`|`admission`), `CCU_MIX`, `CCU_DIST`, `CCU_STEPS`,
`CCU_HOLD_SEC`, `CCU_JOIN_RATE`, `VIEWER_HOSTS`, `SOURCE_HOST`, `MEDIA_PROFILE`.

```bash
# kiểm công cụ trên máy dev (không công bố số)
MEDIA_PROFILE=light CCU_STEPS=0,50,100,200 \
    tests/integration/stress_vms/run_stress.sh /tmp/s3mk_ccu mid ccu

# đo thật, máy đích + máy nguồn + 2 máy client
MEDIASERVER_DIR=/opt/s3mk-release MEDIA_PROFILE=realistic CCU_MIX=vms CCU_DIST=hot \
SOURCE_HOST=10.0.0.11 VIEWER_HOSTS=10.0.0.21,10.0.0.22 \
    tests/integration/stress_vms/run_stress.sh /data/s3mk_ccu full ccu
```

## 11. Thứ tự làm

| Bước | Nội dung | Xong khi |
|---|---|---|
| C1 | `viewer_load.py` (fMP4 + RTSP), `check_ccu.py` tối thiểu, scenario `ccu` trên profile `mid`, bitrate thấp | Chạy được thang bậc trên máy dev, số phiên client khớp server |
| C2 | HLS + replay trong `viewer_load.py`; tiêu chí khoảng hở ghi hình | Hỗn hợp `vms` chạy trọn |
| C3 | Profile media `realistic`, nguồn nhiều tiến trình, bản Release qua `mock_api` | Tự kiểm nguồn đạt ở 600 luồng thật |
| C4 | Chạy nhiều máy, lượt đo chính thức: 5 hỗn hợp thuần + `vms` × `hot`/`uniform` | Có báo cáo CCU + yếu tố giới hạn |
| C5 | `ccu_storm`, `ccu_churn`, `ccu_soak`, `ccu_admission` | Báo cáo đủ 5 kịch bản |

### 11.1 Trạng thái (2026-09-29)

C1, C2, C3 và code của C5 đã xong. C4 cần máy đích. Để chạy trên máy đích chỉ với binary Release,
`tests/integration/stress_vms/make_ccu_bundle.sh` sinh **một file** `ccu_test.sh` (≈7 MB) chứa
harness, migration, `config.ini`, `default.pem` và clip dựng sẵn:

```bash
tests/integration/stress_vms/make_ccu_bundle.sh /tmp/ccu_test.sh     # ở máy dev (cần ffmpeg)
./ccu_test.sh /opt/s3mk/MediaServer --suite standard                  # ở máy đích
# -> ccu_result_<máy>_<thời gian>.tar.gz: SUMMARY.md, báo cáo từng kịch bản, log server nén,
#    tóm tắt cảnh báo theo vị trí code, vmstat/iostat/PSI, thông tin phần cứng
```

Những điều học được khi chạy trên máy dev (WSL, 8 lõi, 7,7 GB RAM, đĩa ảo):

- **Đĩa là trần đầu tiên của máy dev.** 120 camera bitrate thật ghi ~70 MB/s; PSI `io full` 10%
  (mọi tác vụ đứng chờ IO), cả máy khựng theo đợt tới 4 s, kể cả khi không có viewer: luồng kéo
  camera timeout hàng loạt, ghi hình hở 7–14 s. `check_ccu.py` giờ đo PSI và xếp yếu tố "disk".
  Trên máy đích, 300 camera ≈ 160 MB/s — phải kiểm cột "IO full %" trước khi đọc các cột khác.
- **Một poller gánh ~50% một lõi ngay ở bậc 0** (máy dev), các poller khác 2–4%: có vẻ toàn bộ
  luồng kéo camera dồn một poller. Cần xác nhận trên máy đích (`hot_threads`, `poller_load_max`).
- Replay phải lùi ≥ 3 × `fileSecond` so với hiện tại; sát cạnh bản ghi mới nhất thì replay chờ file
  đang ghi đóng lại (đúng thiết kế "đuổi bản ghi mới") và bị đo nhầm thành khựng.
- `rtsp_source.py` từng phát file luồng chính cho cả luồng `-sub` và không bao giờ ngắt cohort
  `flaky` (khoá luồng lấy nhầm `trackID=0` của URL SETUP) — đã sửa.
- **`conf/config.ini` của repo không phải cấu hình production**: nó đặt
  `general.broadcast_player_count_changed=0` (code mặc định 1), nên `ReaderMonitor` không nhận số
  reader và giới hạn `maxStream`/`maxStreamPerCamera` không chặn gì. Kịch bản `ccu*` giờ dựng cấu hình
  từ mặc định trong code (`CONFIG_BASE=code`). Với cấu hình đó `ccu_admission` đạt đủ: toàn node dừng
  đúng ở `(int)(maxStream × 1,1)` (429 cho người thừa), burst 2× ngưỡng trong 1 s không vượt, theo
  camera dừng đúng ở `maxStreamPerCamera`, ngưỡng CPU trả 503 — hai bản sửa 1539590f đúng.
  Các số đo CCU chạy trước thay đổi này (máy dev) dùng cấu hình repo, không so sánh được.
- RTSP trả `401 Unauthorized` khi server từ chối vì tài nguyên (`ResourceUnavailable`), HTTP trả
  `503` — chưa sửa, ghi nhận.

## 12. Câu hỏi mở

- Phần cứng đích (số lõi, RAM, NIC, loại đĩa) và ingest/egress có chung cổng mạng không — quyết định
  400 CCU có khả thi về mạng hay không (mục 8).
- Bitrate camera thật (mục 2.1 giả định 4 Mbps / 512 kbps).
- Hỗn hợp viewer thực tế của khách hàng có khác mục 3.1 không (đặc biệt tỉ lệ replay — nặng nhất về đĩa).
- WebRTC có nằm trong phạm vi không (hiện harness tắt `rtc`).
