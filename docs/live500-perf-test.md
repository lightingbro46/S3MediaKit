# Đo hiệu năng để tinh chỉnh: 300 camera ghi 24/7 + 500 người xem HLS/HTTP-MP4

Mục tiêu: một node MediaServer kéo **300 camera / 600 luồng**, **ghi liên tục cả luồng chính
và luồng phụ**, và phục vụ **500 người xem trực tiếp** qua HLS và HTTP-MP4 (fMP4), mà người xem
không khựng và bản ghi không hở. Công cụ đo ở mức đó và các mức xung quanh, rồi chỉ ra **tài
nguyên nào chạm trần trước và tiêu vào đâu**. Người vận hành sửa cấu hình rồi chạy lại cùng tải
để so sánh.

Công cụ là suite `live500` của harness CCU (`tests/integration/stress_vms/`, thiết kế chung ở
[ccu-capacity-test-design.md](ccu-capacity-test-design.md)). Tài liệu này chỉ mô tả phần khác.

## 1. Tải

| Thành phần | Giá trị |
|---|---|
| Camera | 300, nguồn RTSP/TCP giả, main 1080p25 4 Mbps + sub 640x360 25 fps 512 kbps, GOP 2 s |
| Ghi hình | Mọi camera cohort `always` (`STRESS_COHORT=always`): RecordAlways 24/7, ghi cả 2 luồng ≈ 160 MB/s |
| Người xem (`CCU_MIX=live`) | HTTP-MP4 sub 50%, HTTP-MP4 main 20%, HLS sub 20%, HLS main 10% |
| Phân bố theo camera | `hot` (Zipf, 20% camera nhận ~80% người xem) và `uniform` |
| Cấu hình | Mặc định trong code (`CONFIG_BASE=code`), số poller = số lõi, bản Release |

Ước lượng băng thông: trung bình ~1,56 Mbps/người xem → 500 người ≈ **780 Mbps ra**, cộng
**1,35 Gbps vào** từ camera. Một cổng 1 GbE chung không đủ; cần 10 GbE hoặc tách cổng camera và
cổng người xem. Nếu báo cáo ghi "giới hạn bởi nic" thì đó là kết luận đúng về phần cứng.

## 2. Kịch bản

| Kịch bản | Làm gì | Thời gian |
|---|---|---|
| `live_step` | Bậc 0 → 100 → … → 500 → 600 → 750 → 1000 (`hot`), dừng sau 2 bậc trượt. Bậc 500 giữ 300 s và chạy `perf record` 120 s | ~50 phút |
| `live_uniform` | Bậc 0, 250, 500, 600, 750 với phân bố `uniform` | ~25 phút |
| `live_storm` | Mở 500 người xem trong 5 s | ~10 phút |
| `live_replay` | 500 người xem, **đúng 20% (100) là xem lại** (mục 2.1) | ~25 phút |
| `live_soak` | Giữ 500 người xem `--soak-hours` giờ (không nằm trong suite, thêm bằng `--only`) | tuỳ chọn |

### 2.1 `live_replay` — 20% xem lại

- Chờ **15 phút ghi hình** (`CCU_REPLAY_WAIT_SEC=900`, tính từ lúc luồng lên) rồi mới mở người xem.
- Hỗn hợp `live_replay`: cứ 5 người xem có đúng 1 replay (bậc 500 = 400 live theo tỉ lệ `live` +
  100 replay luồng chính). Replay chọn camera theo phân bố `hot` nên nhiều phiên có thể xem lại
  cùng một camera ở các mốc khác nhau.
- Mỗi phiên replay mở `/media/record/<cam>/<cam>-main/vod/<mốc>.live.mp4` với mốc = lúc mở trừ một
  số giây **ngẫu nhiên từ 5 đến 15 phút trước đó** (`CCU_REPLAY_BACK=300,900`), xem **5 phút**
  (`CCU_REPLAY_WATCH=300`) tính từ frame đầu rồi tự đóng, slot không mở lại. Mất kết nối giữa chừng thì
  mở lại từ chỗ đã xem tới và tính là "kết nối lại".
- Một phiên replay **đạt** khi: TTFF ≤ 10 s, xem đủ 5 phút, nhận ≥ 95% media (285 s), không khựng
  > 2 s, không kết nối lại. Kịch bản đạt khi ≥ 99% phiên replay đạt **và** bậc 500 đạt các tiêu chí
  chung (live, nền, ghi hình).
- Báo cáo `ccu_replay_report.md`: bảng **từng phiên replay** (camera, mốc lùi, TTFF, thời gian đã
  xem, media nhận, số lần / tổng / lần khựng dài nhất, kết nối lại, kbps, lý do trượt), bảng gộp theo
  nhóm mốc lùi, và tóm tắt các phiên live chạy cùng lúc. `ccu_replay_sessions.csv` có từng phiên
  của cả live lẫn replay.
- Bảng gộp chia khoảng mốc lùi làm 3 nhóm (mặc định 300–499, 500–699, ≥ 700 s) để so chất lượng
  theo vị trí mốc.
- **Vì sao mốc lùi tối thiểu 5 phút:** đoạn ghi dài 60 s (`protocol.mp4_max_second`). Ở 1x, replay
  luôn đi sau hiện tại đúng bằng mốc lùi. Với mốc lùi dưới khoảng 1–2 phút, replay chạm tới file
  đang ghi: lúc chạy thử 2026-10-08 (mốc lùi 17 s), server đóng luồng sau 14 s rồi trả 404 khi mở
  lại cho tới khi file đó đóng. Mốc ≥ 300 s không chạm vùng này, nên kịch bản đo đúng phần đọc bản
  ghi. Muốn đo cả vùng sát hiện tại thì đặt `CCU_REPLAY_BACK=0,900`.

Tiêu chí đạt/trượt của từng bậc giống thiết kế CCU mục 6: TTFF max ≤ 5 s, phiên khựng > 2 s
≤ 1%, phiên đứt ≤ 0,5%, healthcheck p95 ≤ 200 ms, luồng nền ≥ 99%, **không khoảng hở ghi hình
ở cả 300 camera**.

## 3. Chạy trên máy đích

```bash
# máy dev (cần ffmpeg): đóng gói một file
tests/integration/stress_vms/make_ccu_bundle.sh /tmp/ccu_test.sh

# máy đích: lần đo gốc với cấu hình mặc định
./ccu_test.sh /opt/s3mk/MediaServer --suite live500 --tag base

# chạy lại với một thay đổi, cùng tải — chỉ bậc cần so
./ccu_test.sh /opt/s3mk/MediaServer --suite live500 --only live_step \
    --tag seg4 --set hls.segDur=4 --set hls.segNum=3
./ccu_test.sh /opt/s3mk/MediaServer --suite live500 --only live_step --tag t32 --threads 32
# chỉ kịch bản xem lại; đổi thời lượng/mốc qua biến môi trường
CCU_REPLAY_BACK=0,900 ./ccu_test.sh /opt/s3mk/MediaServer --suite live500 --only live_replay --tag rp0
```

- `--set muc.khoa=gia_tri` ghi đè `config.ini` của node (lặp lại được), `--threads N` truyền
  `-t N`. Cả hai được ghi vào `SUMMARY.md` và `system.txt`; `config.ini` thật của node nằm trong
  thư mục kết quả của từng kịch bản.
- `--tag` gắn vào tên tarball `ccu_result_<máy>_<tag>_<thời gian>.tar.gz` để so các biến thể.
- Nên dùng bản **RelWithDebInfo** (hoặc Release không strip) và cài `perf`
  (`linux-tools-$(uname -r)` / `perf`), chạy bằng root. Thiếu perf thì báo cáo ghi lý do, các số
  khác vẫn đủ. Bản Debug cho số CPU không dùng được.
- Viewer nên chạy trên máy khác (`--viewer-hosts`, `--nic`) khi đo thật; cùng máy thì CPU của
  bộ sinh tải tính chung với server.
- `/dev/shm` cần ~4,5 GB (HLS của 600 luồng nằm ở đó như production); docker cần `--shm-size`.

Trên harness trực tiếp (máy dev, không qua bundle):

```bash
STRESS_COHORT=always CCU_MIX=live CCU_ACCEPT=500 CCU_PERF_SEC=120 \
CCU_STEPS=0,100,200,300,400,500,600,750,1000 CCU_ARGS="--bisect 0" \
STRESS_INI="hls.segDur=4" MS_EXTRA_ARGS="-t 16" MEDIA_PROFILE=realistic \
    tests/integration/stress_vms/run_stress.sh /data/live500 full ccu
```

## 4. Đọc kết quả

`SUMMARY.md` → mục `live_step`:

1. **Bậc 0 phải đạt.** Không có người xem mà đã trượt thì nền (ghi 600 luồng) là vấn đề; đọc
   cột `ghi đĩa MB/s`, `IO full %` trước.
2. Dòng **CCU đạt** và **Yếu tố giới hạn** (crash → disk → nic → memory → cpu → poller →
   recording → server → viewer).
3. Dòng **Tài nguyên ở bậc 500**: CPU tổng, poller nóng nhất, NIC, ghi đĩa, RSS và ba nhóm thread
   nặng nhất — đây là mức dư để so giữa các biến thể.
4. **CPU theo nhóm thread** ở mọi bậc: nhóm nào tăng theo số người xem (thường `event poller`) và
   nhóm nào cố định (ghi hình, SQLite, pool `api`/`device`).
5. **Hàm nóng ở bậc 500** (`perf_500.txt`, self time theo thread/thư viện/hàm). Phân tích sâu hơn:
   `perf report -i perf_500.data` (file `.data` nằm trong `ccu_work/live_step/`, không vào tarball).

## 5. Từ nút thắt tới tham số

Các khoá dưới đây đều có trong `src/Common/config.cpp`; giá trị là mặc định của code.

| Dấu hiệu | Đọc thêm | Thử |
|---|---|---|
| Một poller nóng (> 85%) trong khi CPU tổng thấp | `hot_threads`, `poller_load_max` | Phân bố poller của camera/phiên (đã sửa dồn 1 poller, xem `DeviceSource::nextDevicePoller`); `--threads` không giúp nếu tải dồn một luồng |
| Mọi poller cùng cao, CPU tổng cao | Hàm nóng: `memcpy`/mux/`send` | `--threads` = số lõi thật (không tính HT); `http.sendBufSize` (65536) lớn hơn để giảm số lần ghi socket; `general.mergeWriteMS` (0) = 50–300 gộp gói, đổi lấy độ trễ |
| CPU tăng theo **số luồng** chứ không theo người xem | Bậc 0 đã cao; nhóm poller ở bậc 0 | HLS/fMP4 đang mux cho cả 600 luồng (`protocol.hls_demand=0`, `fmp4_demand=0`). Đặt `protocol.hls_demand=1` chỉ mux HLS khi có người xem — đổi lại TTFF của người xem HLS đầu tiên dài hơn |
| TTFF HLS cao, HTTP-MP4 bình thường | TTFF theo loại, `hls_skipped_segments` | `hls.segDur` (2), `hls.segNum` (3); segment dài hơn giảm số file/playlist nhưng tăng TTFF và độ trễ |
| Ghi hình hở, `IO full %` / iowait cao | `ghi đĩa MB/s`, `iostat.log` | Đĩa không đủ cho ~160 MB/s ghi liên tục: tách đĩa ghi, RAID; `record.fileBufSize` (65536) lớn hơn để ghi khối lớn; kiểm HLS có nằm trên `/dev/shm` không |
| NIC > 80% | `nic_tx_mbps` | Phần cứng (mục 1), không phải tham số |
| RSS tăng theo người xem | `rss_mb` từng bậc | Bộ đệm gửi theo phiên (`http.sendBufSize`), số segment HLS giữ (`hls.segNum`, `hls.segRetain`) |
| Người xem bị đứt hàng loạt, server còn dư | `joins.reasons`, `closes` | Giới hạn admission: chạy ở chế độ capacity (mặc định của harness); `ulimit -n` |

Mỗi lần chỉ đổi **một** thứ, chạy `--only live_step` với `--tag` riêng, rồi so dòng "Tài nguyên ở
bậc 500" và "CCU đạt" giữa các tarball. Hai lần chạy cùng cấu hình trên cùng máy nên lệch nhau
không quá vài phần trăm CPU; chênh nhỏ hơn mức đó thì không kết luận.
