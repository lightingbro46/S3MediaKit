# Đo ngưỡng máy chủ Recording: số camera và số luồng xem theo CPU, RAM, ổ đĩa

Tài liệu này hướng dẫn đo trên **một máy chủ Recording** (32 vCPU, 32 GB RAM, SSD 500 GB,
NIC 10 Gbps, có Docker) để trả lời hai câu hỏi cho từng điều kiện phần cứng:

1. **Ngưỡng camera**: một node MediaServer kéo và **ghi 24/7 cả luồng chính lẫn luồng phụ**
   được tối đa bao nhiêu camera mà luồng không rớt và bản ghi không hở.
2. **Ngưỡng luồng xem**: với một số camera nền cố định đang ghi, node phục vụ được tối đa bao
   nhiêu người xem đồng thời (HLS + HTTP-MP4) mà người xem không khựng và bản ghi vẫn nguyên vẹn.

"Điều kiện" là số lõi CPU dành cho MediaServer, dung lượng RAM, và loại ổ ghi hình (SSD/HDD).
Người chạy điền kết quả vào các bảng ở [mục 8](#8-bảng-điền-kết-quả) và gửi lại cùng các file
`ccu_result_*.tar.gz` để đánh giá.

Công cụ là bundle `ccu_test.sh` (harness `tests/integration/stress_vms/`, thiết kế ở
[ccu-capacity-test-design.md](ccu-capacity-test-design.md) và
[live500-perf-test.md](live500-perf-test.md)). Không cần camera thật, không cần repo hay ffmpeg
trên máy đích.

---

## 1. Tải mô phỏng và cách chấm

### 1.1 Camera giả

| Luồng | Mã hoá | Độ phân giải | fps | GOP | Bitrate |
|---|---|---|---|---|---|
| Chính (main) | H.264 | 1920x1080 | 25 | 2 s | 4 Mbps |
| Phụ (sub) | H.264 | 640x360 | 25 | 2 s | 512 kbps |

Mỗi camera là một nguồn RTSP/TCP giả do `rtsp_source.py` phát. Mọi camera ở chế độ ghi 24/7 cả
hai luồng (`STRESS_COHORT=always`). Cấu hình MediaServer là **mặc định trong code** như production,
không dùng `conf/config.ini` của repo.

Chi phí một camera theo bitrate trên:

| Tài nguyên | Mỗi camera | 100 camera | 300 camera | 1000 camera |
|---|---|---|---|---|
| Băng thông vào | 4,5 Mbps | 450 Mbps | 1,35 Gbps | 4,5 Gbps |
| Ghi đĩa | ~0,56 MB/s | ~56 MB/s | ~170 MB/s | ~560 MB/s |
| Dung lượng mỗi giờ | ~2 GB | ~200 GB | ~610 GB | ~2 TB |
| `/dev/shm` (HLS) | ~15 MB | 1,5 GB | 4,5 GB | 15 GB |

Camera thật dùng bitrate khác (ví dụ H.265 main 2 Mbps) thì băng thông, ghi đĩa và dung lượng đổi
theo tỉ lệ bitrate. CPU phụ thuộc chủ yếu vào số gói/giây và số luồng, không tỉ lệ thuận với bitrate.
Ghi lại bitrate thật của camera vào [Bảng 0](#bảng-0--thông-tin-máy-và-bản-build) để quy đổi kết quả.

### 1.2 Người xem giả

Hỗn hợp `live`: HTTP-MP4 luồng phụ 50%, HTTP-MP4 luồng chính 20%, HLS luồng phụ 20%, HLS luồng
chính 10%. Trung bình ~1,56 Mbps/người xem. Người xem chọn camera theo phân bố `hot` (20% camera
nhận ~80% người xem). Bộ sinh viewer không giải mã video; nó đo TTFF, khựng, độ trôi và số byte từ
lớp đóng gói.

Lượt tuỳ chọn `live_replay` thêm 20% người xem là xem lại bản ghi (đọc đĩa), quan trọng khi ổ là HDD.

### 1.3 Một bậc đạt khi nào

Một bậc (số camera hoặc số người xem) **đạt** khi mọi điều kiện sau đúng trong cửa sổ đo:

| Nhóm | Điều kiện |
|---|---|
| MediaServer | Không crash; `/media/mserver/healthcheck` p95 ≤ 200 ms |
| Camera nền | ≥ 99% luồng online; trạng thái ghi khớp lịch; **0 khoảng hở ghi hình** mới |
| Người xem (chỉ ngưỡng luồng xem) | Vào được ≥ 99,5%; TTFF max ≤ 5 s (live) / 10 s (replay); phiên khựng > 2 s ≤ 1%; phiên bị đóng ngoài ý muốn ≤ 0,5% |

CPU, poller nóng nhất, RAM, NIC, IO đĩa vẫn được đo, nhưng chỉ dùng để **gọi tên yếu tố giới hạn**
khi bậc đã trượt (theo thứ tự crash → đĩa → NIC → bộ nhớ → CPU → poller → ghi hình).

Một bậc **không hợp lệ** khi chính bộ sinh tải quá tải (nguồn RTSP tụt nhịp, worker viewer > 85%
CPU). Bậc không hợp lệ không nói gì về MediaServer: phải cho bộ sinh tải thêm lõi hoặc máy khác.

- **Ngưỡng camera** = bậc camera cao nhất đạt (lượt `cam_step`, không có người xem).
- **Ngưỡng luồng xem** = dòng `CCU đạt` của lượt `live_step`, ghi kèm số camera nền.

---

## 2. Thiết kế phép đo

### 2.1 Một máy, chia lõi giữa MediaServer và bộ sinh tải

Chỉ có một máy, nên MediaServer và bộ sinh tải (nguồn RTSP, viewer, mock API) chạy chung. Để kết
quả của MediaServer không lẫn với bộ sinh tải, bundle ghim chúng vào **hai nhóm lõi riêng**:

- `--ms-cpus 0-15`: MediaServer chạy bằng `taskset -c 0-15`, số poller tự đặt bằng số lõi (`-t 16`).
  CPU% trong báo cáo được tính trên chính số lõi này.
- `--gen-cpus 16-31`: runner và mọi tiến trình con (nguồn RTSP, viewer, kiểm tra) bị ghim vào các
  lõi còn lại.

Hệ quả là **không đo được trực tiếp MediaServer dùng cả 32 vCPU** trên một máy: bộ sinh tải luôn
cần một phần lõi. Cấp cho MediaServer 4, 8, 16, 24 lõi rồi đọc đường cong theo số lõi; mức 32 lõi
suy ra từ đường cong đó. Nếu mượn được một máy thứ hai trong cùng mạng 10 Gbps, đặt viewer lên đó
bằng `--viewer-hosts` (xem [mục 5.4](#54-có-máy-thứ-hai)).

### 2.2 RAM: giới hạn của container

`docker run --memory 16g --memory-swap 16g` giới hạn RAM của **cả container**. Trong container đó
có MediaServer, bộ sinh tải (thường 1–3 GB), page cache của file đang ghi và **`/dev/shm` (HLS)**:
tmpfs cũng tính vào giới hạn RAM. Ở 300 camera, riêng HLS đã chiếm ~4,5 GB. Đây cũng là điều xảy ra
trên production nếu container MediaServer bị giới hạn RAM, nên vẫn là phép đo có nghĩa.
`check_ccu.py` đọc `memory.max` của cgroup để tính tỉ lệ RSS theo đúng giới hạn này.

### 2.3 Ổ đĩa

Thư mục làm việc (`--work`) chứa bản ghi, DB và log, nên **ổ chứa `--work` là ổ được đo**.

| Điều kiện | Cách làm | Độ tin cậy |
|---|---|---|
| SSD | `--work` trên SSD 500 GB có sẵn | Chuẩn |
| HDD thật | Gắn một ổ HDD (khuyến nghị 7200 rpm, cùng loại dự định dùng), mount ví dụ `/mnt/hdd`, đặt `--work` ở đó | Chuẩn |
| HDD giả lập | Không có HDD: giới hạn IO của container trên SSD bằng `--device-write-bps/iops` (mục 5.3) | **Chỉ tham khảo**: chặn được băng thông và IOPS, nhưng không tái hiện độ trễ tìm (seek) khi hàng trăm file ghi song song và replay đọc cùng lúc |

Hai lưu ý về SSD 500 GB:

- **Ghi bền vững**: nhiều SSD ghi nhanh trong vùng cache SLC rồi tụt mạnh. Mỗi bậc camera giữ
  10 phút (ở 300 camera ≈ 100 GB) nên thường vượt cache và đo được tốc độ bền vững.
- **Dung lượng**: 300 camera ghi đầy 450 GB trong khoảng 45 phút. MediaServer tự xoá bản ghi cũ khi ổ
  dùng tới 90% (`storage.limitPercentUsage`), bundle dừng kịch bản nếu ổ còn < 5% hoặc < 10 GB, và
  xoá bản ghi sau mỗi kịch bản. Vì vậy test vẫn chạy được, và đo luôn cả trường hợp **vừa ghi vừa xoá**.
  Xem [Bảng 7](#bảng-7--dung-lượng-lưu-trữ-tính-không-đo) để thấy 500 GB giữ được bao lâu.

### 2.4 NIC 10 Gbps

Khi mọi thứ chạy trên một máy, camera và người xem đi qua loopback, **không đi qua NIC**. Giới hạn
NIC vì thế được **tính**, không đo:

```
băng thông cần (Mbps) ≈ số camera × 4,5 + số người xem × 1,56   (bitrate mục 1.1)
dùng an toàn ≤ 80% × 10 000 = 8 000 Mbps
```

Ví dụ: 300 camera (1 350 Mbps) để lại ~6 650 Mbps, tức ~4 200 người xem trước khi chạm NIC. Với tải
này NIC hiếm khi là giới hạn đầu tiên, trừ khi vượt ~1 500 camera. Cột `NIC Mbps` trong báo cáo là
lưu lượng loopback, dùng để kiểm tải thật đã được tạo, không phải tải lên card 10 Gbps.

### 2.5 Ma trận lượt đo

Không chạy mọi tổ hợp CPU × RAM × đĩa (quá lâu). Mỗi lượt chỉ đổi **một** yếu tố so với cấu hình gốc
G (MediaServer 16 lõi, RAM không giới hạn, SSD).

| Lượt | MediaServer | Bộ sinh tải | RAM container | Ổ ghi hình | Ưu tiên |
|---|---|---|---|---|---|
| **G** | 16 lõi (`0-15`) | 16 lõi (`16-31`) | không giới hạn (32 GB) | SSD | 1 |
| C4 | 4 lõi (`0-3`) | `16-31` | không giới hạn | SSD | 2 |
| C8 | 8 lõi (`0-7`) | `16-31` | không giới hạn | SSD | 2 |
| C24 | 24 lõi (`0-23`) | 8 lõi (`24-31`) | không giới hạn | SSD | 3 (bộ sinh tải có thể không đủ ở bậc cao) |
| M16 | 16 lõi | `16-31` | 16 GB | SSD | 2 |
| M8 | 16 lõi | `16-31` | 8 GB | SSD | 3 |
| H | 16 lõi | `16-31` | không giới hạn | HDD thật | 1 nếu có HDD |
| Hs | 16 lõi | `16-31` | không giới hạn | HDD giả lập | 3, chỉ khi không có HDD |

Ở C4 và C8, bộ sinh tải vẫn giữ 16 lõi (`16-31`); các lõi ở giữa để trống. Như vậy sức sinh tải
giống G và chỉ số lõi của MediaServer thay đổi.

Mỗi lượt gồm:

| Bước | Lệnh (`--only`) | Làm gì | Thời gian |
|---|---|---|---|
| a | `cam_step` | Bậc camera `--cam-steps` (mặc định 100, 200, 300, 400, 500, 600, 700, 800, 1000), 0 người xem, mỗi bậc khởi động lại MediaServer, chờ camera lên 180 s, giữ 600 s. Dừng sau 2 bậc trượt liên tiếp hoặc khi bậc không hợp lệ | ~14 phút/bậc, ~1–2 giờ |
| b | `live_step` với `--cameras N` | Người xem 0 → 100 → … → 2000 trên N camera nền, dừng sau 2 bậc trượt | ~60–80 phút |
| c (tuỳ chọn) | `live_replay` với `--cameras N` | 500 người xem, 20% là xem lại; chờ 15 phút ghi trước khi mở | ~40 phút |

**Chọn N cho bước b**: N = 300 nếu ngưỡng camera của lượt đó ≥ 300, nếu không thì N = ngưỡng camera
làm tròn xuống bội của 50. Mọi lượt dùng chung N = 300 thì các ngưỡng luồng xem so sánh trực tiếp
được với nhau.

Bước c nên chạy ít nhất ở **G** và **H** (HDD chịu replay đọc chen với ghi kém hơn SSD nhiều).

Tổng thời gian: khoảng 3 giờ mỗi lượt. Thứ tự đề xuất: G → H → C8 → M16 → C4 → C24 → M8 → Hs.

---

## 3. Chuẩn bị (một lần)

### 3.1 Bản build

- Dùng bản **Release** hoặc **RelWithDebInfo** của MediaServer. Bundle cảnh báo nếu binary là bản
  Debug; số CPU của bản Debug không dùng được.
- Image production (`dockerfile`, `CMAKE_BUILD_TYPE=${MODEL}`) chứa `/opt/media/bin/MediaServer`.
  Ghi commit/tag của image vào Bảng 0.
- Bundle `ccu_test.sh` dựng ở máy dev (cần ffmpeg), từ cùng commit với binary nếu được:

  ```bash
  tests/integration/stress_vms/make_ccu_bundle.sh /tmp/ccu_test.sh
  # chép /tmp/ccu_test.sh sang máy đích
  ```

### 3.2 Image chạy test

Image production thiếu python3, `ss`/`ip`, `vmstat`, `iostat`. Dựng image test từ image production:

```dockerfile
# Dockerfile.captest
FROM 3spro/3spro-mserver:<tag>
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 iproute2 procps sysstat util-linux linux-tools-generic \
    && rm -rf /var/lib/apt/lists/*
```

```bash
docker build -f Dockerfile.captest -t s3mk-captest .
docker run --rm s3mk-captest bash -c 'python3 --version; taskset -V; ls /opt/media/bin/MediaServer'
```

`perf` (gói `linux-tools-generic`) dùng để lấy hàm nóng ở bậc 500 của `live_step`. Nếu `perf` trong
container không chạy với kernel của host thì bỏ qua; các số khác vẫn đủ.

### 3.3 Máy host

```bash
# 1. Dừng MediaServer production trên máy này (chiếm cổng, CPU, đĩa)
docker stop 3spro-mserver

# 2. Bố trí lõi: xem vCPU nào thuộc cùng một lõi vật lý và cùng NUMA node
lscpu -e=CPU,CORE,SOCKET,NODE
#   Nếu vCPU 0 và 16 là hai luồng HT của cùng một lõi thì "0-15" chia đôi các lõi vật lý.
#   Khi đó dùng cặp liền nhau theo CORE (vd "0-7,16-23" cho MediaServer). Ghi cách chia vào Bảng 0.

# 3. cgroup v2 (cần cho HDD giả lập và cho memory.max)
stat -fc %T /sys/fs/cgroup        # cgroup2fs = v2

# 4. Ổ đĩa
lsblk -d -o NAME,ROTA,SIZE,MODEL  # ROTA=1 là HDD
df -h /data                       # thư mục work trên SSD, cần > 100 GB trống
# HDD thật (nếu có): mount vào /mnt/hdd, ext4 hoặc xfs, như dự định cho production

# 5. Thư mục kết quả
mkdir -p /data/cap /mnt/hdd/cap
cp ccu_test.sh /data/cap/ && chmod +x /data/cap/ccu_test.sh
cp ccu_test.sh /mnt/hdd/cap/ && chmod +x /mnt/hdd/cap/ccu_test.sh   # nếu có HDD
```

Không cần chỉnh sysctl; bundle tự nâng `ulimit -n` và ghi lại `vm.dirty_*`, `somaxconn` vào
`system.txt`. Nếu production đã chỉnh sysctl thì giữ nguyên như production.

---

## 4. Chạy thử (10 phút, trước lượt đầu)

Lần chạy nhỏ để kiểm image, quyền và cách chia lõi trước khi chạy lượt dài:

```bash
docker run --rm -it --name s3mk-cap --network host --privileged \
  --shm-size=20g --ulimit nofile=1048576:1048576 \
  -v /data/cap:/work -w /work s3mk-captest \
  env CAM_HOLD_SEC=60 PHASE_SEC=60 ./ccu_test.sh /opt/media/bin/MediaServer \
      --only cam_step --cam-steps 20,50 --ms-cpus 0-15 --gen-cpus 16-31 --tag smoke --yes
```

Kết quả đúng: hai dòng `cam_20: DAT`, `cam_50: DAT`, và `SUMMARY.md` có bảng bậc camera. Lỗi
`các cổng sau đang bị chiếm` nghĩa là còn MediaServer hoặc test cũ đang chạy. Nếu cổng 18554 bị dịch
vụ khác dùng, thêm `env STRESS_RTSP_PORT=18654`.

---

## 5. Lệnh cho từng lượt

Mọi lượt dùng chung phần đầu dưới đây và chỉ khác phần trong `<…>`. **Tên tag** theo mẫu
`<lượt>_<đĩa>_c<lõi MS>_m<RAM>` để đọc được từ tên tarball.

```bash
D="docker run --rm -it --name s3mk-cap --network host --privileged \
   --shm-size=20g --ulimit nofile=1048576:1048576"
B=/opt/media/bin/MediaServer
```

### 5.1 Lượt G (gốc)

```bash
# a. ngưỡng camera
$D -v /data/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only cam_step --ms-cpus 0-15 --gen-cpus 16-31 --tag G_ssd_c16_m32 --yes

# b. ngưỡng luồng xem trên N camera (N theo mục 2.5, thường 300)
$D -v /data/cap:/work -w /work s3mk-captest \
   env CCU_STEPS=0,100,200,300,400,500,600,750,1000,1250,1500,2000 \
   ./ccu_test.sh $B --only live_step --cameras 300 --accept 500 \
      --ms-cpus 0-15 --gen-cpus 16-31 --tag G_ssd_c16_m32_view --yes

# c. (tuỳ chọn) 20% xem lại
$D -v /data/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only live_replay --cameras 300 \
      --ms-cpus 0-15 --gen-cpus 16-31 --tag G_ssd_c16_m32_replay --yes
```

### 5.2 Lượt CPU và RAM

| Lượt | Thay trong lệnh của G |
|---|---|
| C4 | `--ms-cpus 0-3`, tag `C4_ssd_c4_m32…` |
| C8 | `--ms-cpus 0-7`, tag `C8_ssd_c8_m32…` |
| C24 | `--ms-cpus 0-23 --gen-cpus 24-31`, tag `C24_ssd_c24_m32…` |
| M16 | thêm `--memory 16g --memory-swap 16g` vào `docker run`, tag `M16_ssd_c16_m16…` |
| M8 | thêm `--memory 8g --memory-swap 8g --shm-size=6g`, tag `M8_ssd_c16_m8…` |

Ở C4/C8 ngưỡng camera có thể thấp hơn 300; khi đó bước b dùng `--cameras` = ngưỡng camera của lượt
đó (mục 2.5) và ghi lại N vào Bảng 3.

### 5.3 Lượt ổ đĩa

```bash
# H — HDD thật: chỉ đổi thư mục work sang ổ HDD
$D -v /mnt/hdd/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only cam_step --ms-cpus 0-15 --gen-cpus 16-31 --tag H_hdd_c16_m32 --yes
#   rồi bước b, c như G với tag H_hdd_…; nên chạy cả c (replay) ở lượt này

# Hs — HDD giả lập trên SSD (chỉ khi không có HDD; cần cgroup v2)
#   DEV là thiết bị nguyên ổ chứa /data (vd /dev/nvme0n1 hoặc /dev/sda), không phải phân vùng
DEV=/dev/nvme0n1
$D --device-write-bps $DEV:160mb --device-read-bps $DEV:160mb \
   --device-write-iops $DEV:250 --device-read-iops $DEV:250 \
   -v /data/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only cam_step --ms-cpus 0-15 --gen-cpus 16-31 --tag Hs_hddsim_c16_m32 --yes
```

Giá trị 160 MB/s và 250 IOPS gần với một HDD 7200 rpm. Khi gửi kết quả Hs, ghi rõ là giả lập.

### 5.4 Có máy thứ hai

Nếu có máy client nối cùng mạng 10 Gbps (có python3, SSH không mật khẩu từ container), thêm vào
bước b:

```bash
   --viewer-hosts 10.0.0.21 --nic <card 10G>
```

Viewer khi đó đi qua NIC thật nên cột `NIC Mbps` là lưu lượng ra card thật, và bộ sinh tải trên máy
này nhẹ hơn. Có thể cho MediaServer 24–28 lõi.

### 5.5 Khi lượt bị ngắt

Ctrl+C vẫn đóng gói phần đã chạy. Chạy lại một bước bằng cùng lệnh với tag mới (thêm `_r2`). Với
`cam_step` có thể chạy tiếp từ bậc chưa xong: `--cam-steps 600,700,800,1000`.

---

## 6. Đọc kết quả ngay trên máy (để điền bảng)

Mỗi lệnh in ra `GUI LAI FILE NAY: …/ccu_result_<máy>_<tag>_<thời gian>.tar.gz`. Mở `SUMMARY.md`
trong thư mục `result_*` cạnh tarball (hoặc giải nén tarball).

**`cam_step`** — ở đầu `SUMMARY.md`:

```
| camera | luong online | CPU % (loi MS) | poller max % | RSS MB | ghi dia MB/s | IO full % | NIC Mbps | healthcheck p95 ms | ket luan |
| 100 | 200 | ... | đạt |
| 200 | 400 | ... | đạt |
| 300 | 597 | ... | trượt: luồng online 99.5% ...; ghi hình hở ... |

- **Nguong camera (0 viewer, ghi 24/7 2 luong): 200** (bac truot dau tien: 300; ...)
```

Chép dòng ngưỡng sang Bảng 2 và cả bảng bậc sang Bảng 4. `CPU %` tính trên số lõi của MediaServer
(100% = dùng hết các lõi được ghim). `IO full %` là PSI: phần trăm thời gian mọi tác vụ đều đứng chờ
IO. Kernel không có PSI (cũ hơn 4.20) thì cột này là `None`; khi đó xem `vmstat.log` cột `wa`.

**`live_step`** — mục `live_step` của `SUMMARY.md`: dòng **CCU đạt**, **Yếu tố giới hạn**, bảng theo
bậc người xem (TTFF max, khựng %, CPU %, poller max, ghi hình), dòng **Tài nguyên ở bậc 500** và
**CPU theo nhóm thread**. Chép sang Bảng 3.

**`live_replay`** — `ccu_replay_report.md`: tỉ lệ phiên replay đạt, TTFF replay. Chép sang Bảng 5.

Không cần tự kết luận yếu tố giới hạn; điền số rồi gửi, phần đánh giá sẽ đọc thêm log và chuỗi
mẫu tài nguyên trong tarball.

---

## 7. Gửi lại

1. **Tất cả** file `ccu_result_*.tar.gz`, kể cả lượt trượt, bị ngắt hoặc không hợp lệ.
2. File này (hoặc chỉ mục 8) đã điền.
3. Kết quả của `lscpu -e=CPU,CORE,SOCKET,NODE`, `lsblk -d -o NAME,ROTA,SIZE,MODEL`, `docker info |
   grep -iE 'cgroup|storage driver'` nếu chưa có trong Bảng 0.
4. Ghi chú bất thường: lượt phải chạy lại, máy có tải khác chạy cùng, cảnh báo của bundle lúc khởi
   động (Debug build, thiếu `/dev/shm`, thiếu dung lượng).

---

## 8. Bảng điền kết quả

### Bảng 0 — Thông tin máy và bản build

| Mục | Giá trị |
|---|---|
| CPU (model, `lscpu`: socket × lõi × luồng, NUMA node) | |
| Cách chia lõi: vCPU nào là HT của cùng lõi | |
| RAM (`free -g`) | |
| SSD (model, NVMe/SATA, dung lượng) | |
| HDD (model, rpm, dung lượng) hoặc "không có" | |
| Hệ thống file của `--work` (ext4/xfs, tuỳ chọn mount) | |
| Kernel (`uname -r`), PSI có/không | |
| Docker version, cgroup v1/v2 | |
| NIC (model, tốc độ) | |
| Image / commit MediaServer, kiểu build (Release/RelWithDebInfo) | |
| Commit của bundle (`BUNDLE_INFO` in ra lúc chạy) | |
| Bitrate camera thật dự kiến (main / sub, codec) | |

### Bảng 1 — Tiến độ các lượt

| Lượt | `--ms-cpus` | `--gen-cpus` | RAM | Ổ | a `cam_step` (tag) | b `live_step` (tag, N camera) | c `live_replay` (tag) | Ghi chú |
|---|---|---|---|---|---|---|---|---|
| G | 0-15 | 16-31 | 32 GB | SSD | | | | |
| C4 | 0-3 | 16-31 | 32 GB | SSD | | | — | |
| C8 | 0-7 | 16-31 | 32 GB | SSD | | | — | |
| C24 | 0-23 | 24-31 | 32 GB | SSD | | | — | |
| M16 | 0-15 | 16-31 | 16 GB | SSD | | | — | |
| M8 | 0-15 | 16-31 | 8 GB | SSD | | | — | |
| H | 0-15 | 16-31 | 32 GB | HDD | | | | |
| Hs | 0-15 | 16-31 | 32 GB | HDD giả lập | | | — | |

### Bảng 2 — Ngưỡng camera (bước a, 0 người xem, ghi 24/7 cả hai luồng)

Các cột tài nguyên lấy ở **bậc đạt cao nhất** trong bảng bậc camera của `SUMMARY.md`.

| Lượt | Ngưỡng camera | Bậc trượt đầu tiên | Lý do trượt (cột "ket luan") | Không hợp lệ từ bậc | CPU % (lõi MS) | Poller max % | RSS MB | Ghi đĩa MB/s | IO full % / iowait | Healthcheck p95 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| G | | | | | | | | | | |
| C4 | | | | | | | | | | |
| C8 | | | | | | | | | | |
| C24 | | | | | | | | | | |
| M16 | | | | | | | | | | |
| M8 | | | | | | | | | | |
| H | | | | | | | | | | |
| Hs | | | | | | | | | | |

### Bảng 3 — Ngưỡng luồng xem (bước b)

Các cột tài nguyên lấy ở **bậc CCU đạt**.

| Lượt | Số camera nền N | CCU đạt | Yếu tố giới hạn | Bậc trượt đầu + lý do | CPU % (lõi MS) | Poller max % | RSS MB | NIC Mbps (loopback) | TTFF max ms | Khựng % | Ghi hình |
|---|---|---|---|---|---|---|---|---|---|---|---|
| G | | | | | | | | | | | |
| C4 | | | | | | | | | | | |
| C8 | | | | | | | | | | | |
| C24 | | | | | | | | | | | |
| M16 | | | | | | | | | | | |
| M8 | | | | | | | | | | | |
| H | | | | | | | | | | | |
| Hs | | | | | | | | | | | |

### Bảng 4 — Chi tiết bậc camera (một bảng cho mỗi lượt)

Dán nguyên bảng bậc camera đầu `SUMMARY.md` của `cam_step`. Mẫu:

**Lượt: ____ (tag ____)**

| camera | luồng online | CPU % (lõi MS) | poller max % | RSS MB | ghi đĩa MB/s | IO full % | NIC Mbps | healthcheck p95 ms | kết luận |
|---|---|---|---|---|---|---|---|---|---|
| 100 | | | | | | | | | |
| 200 | | | | | | | | | |
| 300 | | | | | | | | | |
| 400 | | | | | | | | | |
| 500 | | | | | | | | | |
| 600 | | | | | | | | | |
| 700 | | | | | | | | | |
| 800 | | | | | | | | | |
| 1000 | | | | | | | | | |

### Bảng 5 — Xem lại 20% (bước c, tuỳ chọn)

| Lượt | Số camera nền | Người xem (live + replay) | Phiên replay đạt / tổng | TTFF replay max ms | Bậc live + nền | Ghi hình | Kết luận |
|---|---|---|---|---|---|---|---|
| G | | 400 + 100 | | | | | |
| H | | 400 + 100 | | | | | |

### Bảng 6 — Quan sát thêm

| Lượt | Quan sát (crash, cảnh báo lúc khởi động, OOM, ổ đầy, `database is locked`, lượt chạy lại…) |
|---|---|
| | |

### Bảng 7 — Dung lượng lưu trữ (tính, không đo)

Với bitrate mục 1.1 (~2 GB/camera/giờ), dùng 90% dung lượng ổ trước khi MediaServer xoá bản ghi cũ:

| Ổ | 100 camera | 200 camera | 300 camera |
|---|---|---|---|
| SSD 500 GB (450 GB dùng được) | ~2,2 giờ | ~1,1 giờ | ~45 phút |
| Mỗi 1 TB dùng được | ~5 giờ | ~2,5 giờ | ~1,6 giờ |
| Giữ 30 ngày cần | ~146 TB | ~292 TB | ~437 TB |

SSD 500 GB chỉ đủ làm vùng ghi nóng; lưu trữ dài ngày cần tier NAS/S3 hoặc ổ lớn hơn. Nếu bitrate
camera thật khác, nhân các con số trên theo tỉ lệ bitrate. Điền bitrate thật vào Bảng 0, phần đánh
giá sẽ tính lại.

---

## 9. Giới hạn của phép đo

- **Một máy**: MediaServer không được dùng cả 32 vCPU; mức 32 lõi là ngoại suy từ C4/C8/G/C24. Ở
  C24, bộ sinh tải chỉ có 8 lõi nên các bậc cao dễ "không hợp lệ".
- **Loopback**: NIC 10 Gbps không được đo trực tiếp (mục 2.4), trừ khi có máy thứ hai.
- **Camera giả**: nguồn phát đều, không có camera chập chờn, mất gói, đổi độ phân giải hay ONVIF
  chậm. Ngưỡng thật với camera thật thường thấp hơn; nên giữ dư 20–30% so với ngưỡng đo được.
- **Bitrate cố định** H.264 4 Mbps + 512 kbps. Camera H.265 hoặc bitrate khác: đĩa và mạng quy đổi
  theo tỉ lệ; CPU cần đo lại nếu khác nhiều.
- **HDD giả lập** chỉ chặn băng thông và IOPS, không có độ trễ seek: kết quả Hs thường lạc quan hơn
  HDD thật, nhất là khi có replay.
- **Không gồm**: transcode/overlay, motion, WebRTC, tier lên NAS/S3, đồng bộ cụm. Các tính năng này
  tốn thêm CPU/IO trên production và cần đo riêng nếu bật.
