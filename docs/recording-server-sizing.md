# Bảng sizing máy chủ Recording (MediaServer)

Bảng tham chiếu để chọn CPU, RAM, ổ đĩa và mạng cho một node MediaServer ghi hình 24/7. Số liệu lấy
từ báo cáo đo [recording-server-capacity-test.md](recording-server-capacity-test.md) (đợt 2, máy db3,
2026-10-19/21, binary `3388afc`, gồm các lượt G, C4, C8, C24, M16, M8). Phần nào là **số đo**, phần nào là **tính ra**, đều ghi rõ.

Cập nhật bảng này khi có kết quả các lượt còn thiếu (HDD, M8-b chạy lại với 200 camera) hoặc khi chạy lại
sau bản sửa L8 ([capacity-followup-plan.md](capacity-followup-plan.md)).

## 1. Giả định tải

| Mục | Giá trị khi đo |
|---|---|
| Camera | 2 luồng H.264, **ghi 24/7 cả hai**: chính 1080p25 4 Mbps, phụ 360p25 512 kbps (tổng ~4,5 Mbps) |
| Người xem | Mix live: HTTP-MP4 sub 50 %, HTTP-MP4 main 20 %, HLS sub 20 %, HLS main 10 %; ~1,44–1,56 Mbps/người |
| Xem lại | 20 % người xem là replay (lượt G-c) |
| Cấu hình | Mặc định trong code (như production), segment 60 s |
| Không gồm | Transcode/overlay, motion, WebRTC, tier lên NAS/S3, đồng bộ cụm, camera thật chập chờn |

Camera dùng bitrate khác (vd H.265 2 Mbps): **đĩa, dung lượng và mạng** quy đổi theo tỉ lệ bitrate.
**CPU và RAM** phụ thuộc chủ yếu vào số luồng, không giảm theo bitrate.

## 2. Hệ số trên mỗi đơn vị tải

| Tài nguyên | Mỗi camera (2 luồng ghi) | Mỗi người xem live | Nguồn |
|---|---|---|---|
| CPU | ~0,008 vCPU (HT, Xeon Silver 4208 2,1 GHz) | ~0,0006 vCPU (2 000 người ≈ 1–1,3 vCPU) | đo: G-a, C4-a, C8-a / G-b, C8-b |
| RAM tiến trình (RSS) | ~12,5–16 MB (nhiều event poller thì cao hơn: 16 poller ~14 MB, 24 poller ~16 MB) | ~0,1–0,2 MB | đo: G-a đợt 2, C8-a, C24-a, M16-a / G-b, M16-b |
| RAM `/dev/shm` (HLS) | ~15,8 MB, **có cả khi không ai xem** | – | tính từ cấu hình mặc định, khớp với OOM của M16 và M8 (mục 10.17, 10.19 báo cáo) |
| Ghi đĩa bền vững | ~0,51–0,56 MB/s (= bitrate Mbps × 0,125) | – | đo |
| Dung lượng | ~2 GB/giờ, ~48 GB/ngày | – | tính |
| Mạng vào (từ camera) | ~4,2–4,5 Mbps | – | đo (loopback) |
| Mạng ra (tới người xem) | – | ~1,44–1,56 Mbps | đo (loopback) |

Ghi chú:

- `/dev/shm` là tmpfs, **tính vào RAM** (và vào giới hạn RAM nếu chạy trong container). Mặc định
  `hls_demand=0`, `segNum=3`, `segRetain=10`, `segDur=2`: mỗi luồng giữ ~14 segment (~28 s) dù không
  ai xem. Đặt `hls_demand=1` (chỉ tạo HLS cho luồng có người xem) hoặc giảm `segRetain` thì bớt được phần
  lớn con số này; cả hai là quyết định sản phẩm, chưa đo.
- Ngoài RSS còn cần page cache cho file đang ghi và cho replay; tính dư ít nhất 2–4 GB.
- Mạng vào và ra là hai chiều riêng của NIC full-duplex. Chưa tính lưu lượng tier lên NAS/S3 (chiều ra,
  bằng lượng ghi nếu đẩy hết lên tier).

## 3. Ngưỡng đã đo (một node, SSD)

Máy db3: 2 × Xeon Silver 4208 (HT), SSD 894 GB sau RAID SMC3108, xfs. "vCPU" là luồng HT; 2 vCPU = 1
lõi vật lý.

| vCPU / RAM cho MediaServer | Ngưỡng camera (0 người xem) | Yếu tố giới hạn | Người xem trên nền camera | CPU ở ngưỡng camera | RSS ở ngưỡng camera |
|---|---|---|---|---|---|
| 4 (2 lõi) / 32 GB | **200** | lỗi L8 (ghi `info.txt` trên event poller), không phải CPU | ≥ 2 000 trên 200 camera, có bậc khựng lẻ do L8 | 42 % | 2,2 GB |
| 8 (4 lõi) / 32 GB | **400** | L8 ở 500; trần ghi đĩa ~270 MB/s ở 600 | ≥ 2 000 trên 300 camera, 2 bậc khựng lẻ do L8 | 37 % | 5,0 GB |
| 16 (8 lõi) / 32 GB | **500–700** tuỳ trạng thái SSD; **chốt ~400** | trần ghi đĩa 300–350 MB/s | ≥ 2 000 trên 300 camera, sạch; 20 % replay đạt 80/80 | 31–34 % | 7,1–10,6 GB |
| 16 (8 lõi) / **16 GB** | **500**, sát trần RAM; **chốt ~400** | RAM: RSS + HLS ~15 GB ở 500; ở 600 MediaServer bị OOM kill (cả node chết) | ≥ 2 000 trên 300 camera, sạch | 28 % | 7,1 GB (+ HLS ~7,9 GB) |
| 16 (8 lõi) / **8 GB** | **200**; chốt ~150–200 | RAM: RSS + HLS ~6,1 GB ở 200; ở 300 MediaServer bị OOM kill ngay khi ramp (RSS 3,9 GB + HLS ~4,7 GB) | chưa đo (nền 300 camera bị OOM kill; cần chạy lại với 200) | 16 % | 2,9 GB (+ HLS ~3,2 GB) |
| 24 (12 lõi, 2 NUMA node) / 32 GB | **500**; chốt ~400 | trần ghi đĩa ~280–300 MB/s ở 600 | ≥ 1 500 trên 300 camera (giới hạn ở bộ sinh tải 8 vCPU, MediaServer còn dư) | 20 % | 8,1 GB |
| 32 | chưa đo | – | – | – | – |

Điểm chính:

- **Đĩa là giới hạn đầu tiên** từ 8 vCPU trở lên. CPU còn dư > 60 % ở mọi ngưỡng đã đo. Thêm vCPU
  trên 8 không tăng số camera nếu đĩa giữ nguyên: 24 vCPU (C24) cho cùng ngưỡng 500 như 16 vCPU.
- **Cùng một SSD ghi được 270–350 MB/s tuỳ lúc** (nghi do RAID không nhận TRIM hoặc trạng thái cache
  RAID, chưa xác định). Sizing phải theo trạng thái chậm.
- **Quá trần đĩa thì sập cả node**, không suy giảm từ từ: event poller kẹt chờ ghi, mọi luồng timeout,
  hàng đợi đóng segment tăng không giới hạn, RAM/fd tăng theo.
- **Quá trần RAM thì tiến trình bị OOM kill**, mất mọi luồng cùng lúc (M16 ở 600 và 700 camera, M8 ở
  300 camera ngay trong lúc camera kết nối). Hệ số ~30 MB/camera khớp cả hai lượt: 16 GB → ~500,
  8 GB → ~200–270. RAM
  tăng thêm khi đĩa bão hoà (hàng đợi đóng segment giữ bộ đệm), nên trần đĩa và trần RAM dễ chạm cùng lúc.
- Lượt C24 từng crash khi MediaServer tự restart dưới tải (watchdog báo cáo + `exit(0)` giữa lúc tắt);
  đã sửa ở `af9fdff0` cho `/media/api/restartServer`, không ảnh hưởng ngưỡng.
- Ở 4 và 8 vCPU, ngưỡng hiện tại bị hạ bởi lỗi L8 (đã sửa trên nhánh `feature/capacity-followup`, chưa
  đo lại). Sau khi sửa, kỳ vọng 4 vCPU lên ~300 camera (theo CPU).
- Trong mọi lượt, số người xem không chạm trần MediaServer tới 2 000. Ở 2 000, HTTP-fMP4 bắt đầu trôi
  (p95 1,4 s). Thực tế giới hạn người xem là **NIC**.

## 4. Công thức sizing

Với `C` = số camera, `V` = số người xem đồng thời, `B` = bitrate mỗi camera (Mbps, cộng cả hai luồng),
`b` = bitrate trung bình mỗi người xem (Mbps, mặc định 1,5), `D` = số ngày lưu:

| Tài nguyên | Công thức | Mục tiêu sử dụng |
|---|---|---|
| vCPU | `(C × 0,008 + V × 0,0006) / 0,6 + 1`, tối thiểu 4 | ≤ 60 % để chừa chỗ cho đột biến, nạp cấu hình, API |
| RAM | `2 GB + C × 30 MB × 1,3 + V × 0,2 MB + 4 GB page cache` (30 MB = RSS ~14 MB + HLS ~15,8 MB; `hls_demand=1` thì bớt phần HLS của camera không ai xem) | dư 30 % trên RSS + HLS |
| Ghi đĩa bền vững | `C × B × 0,125 / 0,7` MB/s | util ≤ 70 % **ở trạng thái chậm** của ổ (đo bằng `fio` sau khi đã ghi/xoá nhiều) |
| Dung lượng ổ ghi (HOT) | `C × B × 10,8 × D / 0,9` GB | MediaServer xoá bản ghi cũ ở 90 % |
| NIC chiều vào | `C × B / 0,8` Mbps | ≤ 80 % tốc độ card |
| NIC chiều ra | `(V × b + tier) / 0,8` Mbps | ≤ 80 % tốc độ card |

Sau khi tính, **giữ dư thêm 20–30 % số camera** cho camera thật (mất gói, đổi độ phân giải, ONVIF
chậm), vì phép đo dùng nguồn phát đều.

## 5. Cấu hình tham chiếu

Tải theo mục 1 (B = 4,5 Mbps), V = 2 × C người xem. Đã áp công thức mục 4, chưa trừ phần dư 20–30 %
cho camera thật.

| Số camera | vCPU | RAM | Ghi đĩa bền vững cần | Dung lượng HOT 7 ngày | NIC | Trạng thái |
|---|---|---|---|---|---|---|
| 100 | 4 | 16 GB (công thức ~10 GB; 8 GB chạy được 200 camera, M8) | ≥ 80 MB/s | ~38 TB | 1 Gbps (vào ~450 Mbps, ra ~300 Mbps) | trong vùng đã đo |
| 200 | 8 | 16 GB (công thức ~14 GB) | ≥ 160 MB/s | ~76 TB | 2 × 1 Gbps bond hoặc 10 Gbps | đã đo (4 và 8 vCPU; RAM 16 GB còn dư, M16) |
| 300 | 8 | 32 GB (công thức ~18 GB; 16 GB chạy được nhưng không dư page cache) | ≥ 240 MB/s | ~113 TB | 10 Gbps | đã đo (8, 16, 24 vCPU; RAM 16 GB, M16) |
| 400 | 8 (16 nếu nhiều người xem) | 32 GB | ≥ 320 MB/s | ~151 TB | 10 Gbps | **ngưỡng của db3**; SSD RAID của db3 ở trạng thái chậm (~270–300 MB/s) không đủ dư |
| 500–600 | 16 | 32 GB (công thức ~25–29 GB, sát ở 600), khuyên 48 GB | ≥ 400–480 MB/s (NVMe hoặc nhiều ổ) | ~190–230 TB | 10 Gbps | chưa đo với ổ đủ nhanh; 16 GB RAM bị OOM ở 600 (M16), 24 vCPU không giúp (C24) |
| > 600 | – | – | – | – | – | chưa đo; chia nhiều node |

Dung lượng 7 ngày ở trên là ổ ghi trực tiếp. Lưu dài ngày nên để HOT ngắn trên ổ nhanh và đẩy lên tier
NAS/S3: khi đó phải cộng tải ghi/đọc tier vào đĩa và NIC chiều ra.

## 6. Còn thiếu và độ tin cậy

| Mục | Trạng thái |
|---|---|
| HDD (lượt H) | **Chưa đo.** Không dùng bảng này cho ổ ghi HDD; replay đọc chen với ghi trên HDD dự kiến kém hơn SSD nhiều |
| Giới hạn RAM 16 GB (M16) | **Đã đo**: 500 camera sát trần, 600 bị OOM kill; khớp hệ số ~30 MB/camera. Bundle chưa thu `memory.stat`/`dmesg` nên OOM là suy ra |
| Giới hạn RAM 8 GB (M8) | **Đã đo bước a**: 200 camera đạt, 300 bị OOM kill trong ramp (khớp hệ số); bước b chưa đo được vì nền 300 camera vượt RAM, cần chạy lại với 200 |
| 24 vCPU (C24) | **Đã đo**: ngưỡng 500 như 16 vCPU (đĩa giới hạn); trần người xem chưa đo được vì bộ sinh tải thiếu lõi |
| 32 vCPU | Chưa đo; vì đĩa giới hạn trước, không kỳ vọng tăng số camera |
| Sau sửa L8 | Chưa đo lại; ngưỡng 4/8 vCPU có thể tăng |
| NIC 10 Gbps thật | Chưa đo (mọi lượt chạy loopback); giới hạn NIC ở bảng là tính |
| Transcode, motion, WebRTC, tier, cụm | Không có trong phép đo; cần đo riêng nếu bật |
| CPU khác Xeon Silver 4208 | Quy đổi theo hiệu năng đơn luồng; hệ số CPU mục 2 lấy ở 2,1 GHz (turbo 3,2 GHz) |
