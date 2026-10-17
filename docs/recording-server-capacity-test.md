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

- `--ms-cpus 0-7,16-23`: MediaServer chạy bằng `taskset -c 0-7,16-23`, số poller tự đặt bằng số
  vCPU (`-t 16`). CPU% trong báo cáo được tính trên chính số vCPU này.
- `--gen-cpus 8-15,24-31`: runner và mọi tiến trình con (nguồn RTSP, viewer, kiểm tra) bị ghim vào
  các lõi còn lại.

Trên db3, vCPU N và N+16 là hai luồng HT của cùng một lõi vật lý (Bảng 0). Vì vậy mỗi nhóm lõi phải
gồm **trọn cặp HT**: chia `0-15` / `16-31` sẽ làm MediaServer và bộ sinh tải tranh nhau cùng lõi vật
lý, và MediaServer trải trên cả hai NUMA node. Số "lõi" trong tên lượt (C4, C8, G = 16, C24) và trong
tag (`c16`) là **số vCPU**, tức gấp đôi số lõi vật lý.

Hệ quả là **không đo được trực tiếp MediaServer dùng cả 32 vCPU** trên một máy: bộ sinh tải luôn
cần một phần lõi. Cấp cho MediaServer 4, 8, 16, 24 vCPU rồi đọc đường cong theo số vCPU; mức 32 vCPU
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
G (MediaServer 16 vCPU, RAM không giới hạn, SSD).

| Lượt | MediaServer | Bộ sinh tải | RAM container | Ổ ghi hình | Ưu tiên |
|---|---|---|---|---|---|
| **G** | 16 vCPU = 8 lõi (`0-7,16-23`, node0) | 16 vCPU (`8-15,24-31`, node1) | không giới hạn (32 GB) | SSD | 1 |
| C4 | 4 vCPU = 2 lõi (`0-1,16-17`) | `8-15,24-31` | không giới hạn | SSD | 2 |
| C8 | 8 vCPU = 4 lõi (`0-3,16-19`) | `8-15,24-31` | không giới hạn | SSD | 2 |
| C24 | 24 vCPU = 12 lõi (`0-11,16-27`, cả 2 node) | 8 vCPU (`12-15,28-31`) | không giới hạn | SSD | 3 (bộ sinh tải có thể không đủ ở bậc cao) |
| M16 | 16 vCPU như G | `8-15,24-31` | 16 GB | SSD | 2 |
| M8 | 16 vCPU như G | `8-15,24-31` | 8 GB | SSD | 3 |
| H | 16 vCPU như G | `8-15,24-31` | không giới hạn | HDD (`sdb`) | 1 |

Ở C4 và C8, bộ sinh tải vẫn giữ trọn node1 (`8-15,24-31`); các lõi còn lại của node0 để trống. Như
vậy sức sinh tải giống G và chỉ số vCPU của MediaServer thay đổi. Riêng C24, MediaServer lấn sang
node1 nên kết quả gồm cả chi phí truy cập bộ nhớ chéo NUMA, như khi chạy không ghim trên production.

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

Tổng thời gian: khoảng 3 giờ mỗi lượt. Thứ tự đề xuất: G → H → C8 → M16 → C4 → C24 → M8.

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

# 3. cgroup (ghi v1/v2 vào Bảng 0; giới hạn RAM của lượt M16/M8 đọc được ở cả hai)
stat -fc %T /sys/fs/cgroup        # cgroup2fs = v2, tmpfs = v1

# 4. Ổ đĩa
lsblk -d -o NAME,ROTA,SIZE,MODEL  # ROTA=1 là HDD; ổ sau RAID (vd SMC3108 trên db3) báo ROTA=1 cả với SSD
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
      --only cam_step --cam-steps 20,50 --ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31 --tag smoke --yes
```

Kết quả đúng: hai dòng `cam_20: DAT`, `cam_50: DAT`, và `SUMMARY.md` có bảng bậc camera. Lỗi
`các cổng sau đang bị chiếm` nghĩa là còn MediaServer hoặc test cũ đang chạy. Nếu cổng 18554 bị dịch
vụ khác dùng, thêm `env STRESS_RTSP_PORT=18654`.

### 4.1 Kết quả chạy thử trên db3 (2026-10-16)

Thông số máy ở [Bảng 0](#bảng-0--thông-tin-máy-và-bản-build).

**Lần 1 (10:08, `result_db3_smoke_20261016_100812`): trượt, không đo được.** Cả `cam_20` và `cam_50`
`rc=1` sau ~7 s. MediaServer abort ngay khi khởi động:
`SqliteException: no such column: currentSessionId` (signal 6, ngay sau `TierStorageManager started`).
Nguyên nhân: binary `e331cbe` (nhánh `feature/anhnq48-replay-timeline`) chạy với migration của bundle
`b74ea144` (develop), thiếu thay đổi schema của nhánh kia. Lần này còn ghim lõi `--ms-cpus 0-15
--gen-cpus 16-31`: MediaServer trải trên hai NUMA node, còn bộ sinh tải dùng chung lõi vật lý (HT anh
em) với MediaServer.

**Bài học:** binary và bundle phải cùng nhánh/commit. Kiểm dòng `git hash:` trong `stdout.log` khớp
với `commit:` trong `runner.log`. Trên máy có HT, ghim theo cặp `CORE` và NUMA node.

**Lần 2 (13:56, `result_db3_smoke_20261016_135642`): đạt.** Bundle `3aca28e8` dựng lại từ nhánh của
binary, ghim `--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31` (`-t 16`), `CAM_HOLD_SEC=60 PHASE_SEC=60`,
profile full (bitrate thật), 0 viewer, ghi 24/7 cả hai luồng, tổng 4 phút.

| camera | luồng online | CPU % (lõi MS) | poller max % | event poller tổng (% 1 lõi) | RSS MB | ghi đĩa MB/s | `wa` vmstat | NIC vào Mbps | healthcheck p95 ms | bản ghi | kết luận |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 20 | 40 | 3,5 | 2,0 | 32 | 297 | 12,4 | 0,0 | 83,2 | 2,0 | 980 MB, OK | đạt |
| 50 | 100 | 6,2 | 4,7 | 77 | 635 | 27,5 | 0,1 | 207,5 | 2,1 | 2,5 GB, OK | đạt |

- **CPU theo nhóm thread** (% một lõi): bậc 20: event poller 32 % (16 thread), tier 19 % (3), còn
  lại ≤ 2 %. Bậc 50: event poller 77 % (16), tier 19 % (3), còn lại ≤ 1 %. Nhóm `tier` không đổi theo
  số camera; cần xem lại ở bậc lớn.
- **RSS** tăng ~11 MB mỗi camera (2 luồng). Trong 60 s giữ bậc, RSS phẳng ở cuối bậc (50 cam: 627 →
  622 MB). fd 205, thread 57 ở bậc 50.
- **Đĩa** (`sda`/`dm-3`): `%util` trung bình 2,4 %, max 17,5 %. Ghi dồn theo đợt flush page cache
  (đỉnh 77 MB/s, `w_await` 14,6 ms, ngoài đợt flush ~0,1 ms). `vmstat wa` = 0, `b` ≈ 0.
- **Log**: không crash, không dòng `E`. Cảnh báo đều vô hại: `rtsp play success` (log mức W, mỗi luồng
  một dòng), ONVIF mock `ActionNotSupported`/không có home preset, `Server has not reported started`
  lúc khởi động, sync DB tắt theo config.
- **Ngoại suy tuyến tính lên 300 camera** (chỉ tham khảo, cần lượt `cam_step` thật): CPU ~25–30 % của
  16 luồng MS, event poller ~4–5 lõi, RSS ~3,5 GB, ghi đĩa ~165 MB/s, mạng vào ~1,2 Gbps (gần trần
  `bond0` 2 Gbps nếu camera thật đi qua NIC này).
- **Giới hạn của lần chạy**: giữ mỗi bậc chỉ 60 s nên chưa thấy rò rỉ bộ nhớ, retention hay tier;
  không có viewer.

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
   ./ccu_test.sh $B --only cam_step --ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31 --tag G_ssd_c16_m32 --yes

# b. ngưỡng luồng xem trên N camera (N theo mục 2.5, thường 300)
$D -v /data/cap:/work -w /work s3mk-captest \
   env CCU_STEPS=0,100,200,300,400,500,600,750,1000,1250,1500,2000 \
   ./ccu_test.sh $B --only live_step --cameras 300 --accept 500 \
      --ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31 --tag G_ssd_c16_m32_view --yes

# c. (tuỳ chọn) 20% xem lại
$D -v /data/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only live_replay --cameras 300 \
      --ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31 --tag G_ssd_c16_m32_replay --yes
```

### 5.2 Lượt CPU và RAM

| Lượt | Thay trong lệnh của G |
|---|---|
| C4 | `--ms-cpus 0-1,16-17`, tag `C4_ssd_c4_m32…` |
| C8 | `--ms-cpus 0-3,16-19`, tag `C8_ssd_c8_m32…` |
| C24 | `--ms-cpus 0-11,16-27 --gen-cpus 12-15,28-31`, tag `C24_ssd_c24_m32…` |
| M16 | thêm `--memory 16g --memory-swap 16g` vào `docker run`, tag `M16_ssd_c16_m16…` |
| M8 | thêm `--memory 8g --memory-swap 8g --shm-size=6g`, tag `M8_ssd_c16_m8…` |

Ở C4/C8 ngưỡng camera có thể thấp hơn 300; khi đó bước b dùng `--cameras` = ngưỡng camera của lượt
đó (mục 2.5) và ghi lại N vào Bảng 3.

### 5.3 Lượt ổ đĩa

```bash
# H — HDD thật: chỉ đổi thư mục work sang ổ HDD
$D -v /mnt/hdd/cap:/work -w /work s3mk-captest \
   ./ccu_test.sh $B --only cam_step --ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31 --tag H_hdd_c16_m32 --yes
#   rồi bước b, c như G với tag H_hdd_…; nên chạy cả c (replay) ở lượt này
```

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
| Máy | db3 (host CentOS 7, test chạy trong container Ubuntu 22.04, `--network host`) |
| CPU (model, `lscpu`: socket × lõi × luồng, NUMA node) | 2 × Intel Xeon Silver 4208 @ 2,1 GHz (max 3,2 GHz), 2 socket × 8 lõi × 2 luồng = 32 vCPU; NUMA node0 = `0-7,16-23`, node1 = `8-15,24-31`; L3 11 MB/socket, có AVX-512 |
| Cách chia lõi: vCPU nào là HT của cùng lõi | vCPU N và N+16 là hai luồng HT của cùng lõi (0↔16 … 15↔31). MediaServer `0-7,16-23` (8 lõi vật lý / 16 luồng, node0), bộ sinh tải `8-15,24-31` (node1) |
| RAM (`free -g`) | 31 GB (31639 MB), swap 48 GB |
| SSD (model, NVMe/SATA, dung lượng) | `sda` 893,8 GB sau RAID SMC3108 (lsblk báo `ROTA=1` vì qua RAID; xác nhận là SSD). `/work` = LVM `centos-home` 736 GB trên ổ này |
| HDD (model, rpm, dung lượng) hoặc "không có" | `sdb` 4,4 TB sau RAID SMC3108 (xác nhận là HDD); dùng cho lượt H. Còn thiếu: rpm, mức RAID, điểm mount, hệ thống file |
| Hệ thống file của `--work` (ext4/xfs, tuỳ chọn mount) | xfs `rw,relatime,attr2,inode64,sunit=512,swidth=512,noquota` |
| Kernel (`uname -r`), PSI có/không | `3.10.0-1160.105.1.el7.x86_64`, **không có PSI**: cột "IO full %" luôn `None`, đánh giá đĩa bằng `vmstat wa` và `iostat %util`/`w_await` |
| Docker version, cgroup v1/v2 | cgroup v1 (giới hạn RAM đọc qua `memory.limit_in_bytes`); docker version chưa ghi |
| NIC (model, tốc độ) | `bond0` = `eno1` + `eno2` 1 Gbps, tổng 2 Gbps (**không phải 10 Gbps** như giả định mục 2.4); viewer chạy loopback |
| Image / commit MediaServer, kiểu build (Release/RelWithDebInfo) | `/opt/media/bin/MediaServer` Release (`debug_build=0`), git `e331cbe` nhánh `feature/anhnq48-replay-timeline`, build 2026-10-15 13:34, sha256 `392de394…94135`; FFmpeg 4.4.2 của hệ thống, không có GPU (cuda/vaapi/qsv đều không dùng được) |
| Commit của bundle (`BUNDLE_INFO` in ra lúc chạy) | `3aca28e8` (dựng từ nhánh của binary, đóng gói 2026-10-16 11:36). Bundle `b74ea144` (develop) **không chạy được** với binary này, xem mục 4.1 |
| Bitrate camera thật dự kiến (main / sub, codec) | |

### Bảng 1 — Tiến độ các lượt

Theo cách chia lõi của db3 (vCPU N và N+16 cùng lõi vật lý, NUMA node0 = `0-7,16-23`); xem mục 2.5.

| Lượt | `--ms-cpus` | `--gen-cpus` | RAM | Ổ | a `cam_step` (tag) | b `live_step` (tag, N camera) | c `live_replay` (tag) | Ghi chú |
|---|---|---|---|---|---|---|---|---|
| G | 0-7,16-23 | 8-15,24-31 | 32 GB | SSD | `G_ssd_c16_m32` (2026-10-16 14:47): **chạy với `0-15`/`16-31`, giữ 60 s mỗi bậc**, cần chạy lại (mục 10.1) | `G_ssd_c16_m32_view`, N = 300 (2026-10-16 15:11, cũng chạy `0-15`/`16-31`): ≥ 2 000 đạt (mục 10.2) | `G_ssd_c16_m32_replay` (2026-10-16 16:12, `0-15`/`16-31`): **trượt**, replay 64/80 (mục 10.3) | 8 lõi vật lý / 16 luồng, node0; smoke 20/50 cam đạt (mục 4.1) |
| C4 | 0-1,16-17 | 8-15,24-31 | 32 GB | SSD | | | — | 2 lõi / 4 luồng |
| C8 | 0-3,16-19 | 8-15,24-31 | 32 GB | SSD | | | — | 4 lõi / 8 luồng |
| C24 | 0-11,16-27 | 12-15,28-31 | 32 GB | SSD | | | — | 12 lõi / 24 luồng, trải 2 NUMA node; bộ sinh tải 4 lõi / 8 luồng |
| M16 | 0-7,16-23 | 8-15,24-31 | 16 GB | SSD | | | — | |
| M8 | 0-7,16-23 | 8-15,24-31 | 8 GB | SSD | | | — | |
| H | 0-7,16-23 | 8-15,24-31 | 32 GB | HDD (`sdb`) | | | | |

### Bảng 2 — Ngưỡng camera (bước a, 0 người xem, ghi 24/7 cả hai luồng)

Các cột tài nguyên lấy ở **bậc đạt cao nhất** trong bảng bậc camera của `SUMMARY.md`.

| Lượt | Ngưỡng camera | Bậc trượt đầu tiên | Lý do trượt (cột "ket luan") | Không hợp lệ từ bậc | CPU % (lõi MS) | Poller max % | RSS MB | Ghi đĩa MB/s | IO full % / iowait | Healthcheck p95 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| G (sơ bộ, mục 10.1) | 600 theo bundle; **400** nếu tính cả luồng bị rớt mà bundle không bắt | 700 | healthcheck p95 231 ms > 200 ms | – | 28,4 | 41,8 | 6 863 | 305,1 | – / wa 0,6 | 8,7 |
| C4 | | | | | | | | | | |
| C8 | | | | | | | | | | |
| C24 | | | | | | | | | | |
| M16 | | | | | | | | | | |
| M8 | | | | | | | | | | |
| H | | | | | | | | | | |

### Bảng 3 — Ngưỡng luồng xem (bước b)

Các cột tài nguyên lấy ở **bậc CCU đạt**.

| Lượt | Số camera nền N | CCU đạt | Yếu tố giới hạn | Bậc trượt đầu + lý do | CPU % (lõi MS) | Poller max % | RSS MB | NIC Mbps (loopback) | TTFF max ms | Khựng % | Ghi hình |
|---|---|---|---|---|---|---|---|---|---|---|---|
| G (sơ bộ, mục 10.2) | 300 | ≥ 2 000 (mọi bậc đạt) | chưa chạm (trên db3: NIC TX ~1 100 viewer, mục 10.2) | – | 26,9 | 26,7 | 4 259 | 4 199,5 | 159 (hls-sub) | 0,006 | OK |
| C4 | | | | | | | | | | | |
| C8 | | | | | | | | | | | |
| C24 | | | | | | | | | | | |
| M16 | | | | | | | | | | | |
| M8 | | | | | | | | | | | |
| H | | | | | | | | | | | |

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

**Lượt: G sơ bộ (tag `G_ssd_c16_m32`, 2026-10-16 14:47, MS `0-15`, bộ sinh tải `16-31`, giữ 60 s)**

| camera | luồng online | CPU % (lõi MS) | poller max % | RSS MB | ghi đĩa MB/s | IO full % | NIC Mbps | healthcheck p95 ms | kết luận |
|---|---|---|---|---|---|---|---|---|---|
| 100 | 200 | 9,8 | 7,8 | 1 165 | 52,6 | – | 414,8 | 2,5 | đạt |
| 200 | 400 | 15,8 | 14,7 | 2 257 | 103,3 | – | 829,0 | 3,4 | đạt |
| 300 | 600 | 18,9 | 57,3 | 3 371 | 153,4 | – | 1 243,4 | 2,1 | đạt |
| 400 | 800 | 22,2 | 15,0 | 4 417 | 203,9 | – | 1 658,0 | 3,8 | đạt |
| 500 | 1000 | 28,8 | 21,0 | 5 412 | 254,5 | – | 2 074,1 | 5,5 | đạt (64 luồng rớt ngoài cửa sổ đo) |
| 600 | 1200 | 28,4 | 41,8 | 6 863 | 305,1 | – | 2 490,1 | 8,7 | đạt (53 luồng rớt ngoài cửa sổ đo) |
| 700 | 1400 | 32,3 | 18,3 | 7 798 | 354,9 | – | 2 913,5 | 230,8 | trượt: healthcheck |
| 800 | 1600 | 37,3 | 42,0 | 8 975 | 407,7 | – | 3 340,4 | 280,5 | trượt: healthcheck (100 luồng rớt ngoài cửa sổ đo) |

### Bảng 5 — Xem lại 20% (bước c, tuỳ chọn)

| Lượt | Số camera nền | Người xem (live + replay) | Phiên replay đạt / tổng | TTFF replay max ms | Bậc live + nền | Ghi hình | Kết luận |
|---|---|---|---|---|---|---|---|
| G (sơ bộ, mục 10.3) | 300 | 320 live + 80 replay (bậc 400) | 64/80 (80 %, cần ≥ 99 %) | 8 568 | đạt (live 320/320) | OK | **trượt**: 16 phiên khựng 2–7,8 s do đóng segment chặn work poller |
| H | | 400 + 100 | | | | | |

### Bảng 6 — Quan sát thêm

| Lượt | Quan sát (crash, cảnh báo lúc khởi động, OOM, ổ đầy, `database is locked`, lượt chạy lại…) |
|---|---|
| smoke (2026-10-16) | Lần 1 MediaServer abort lúc khởi động (`no such column: currentSessionId`) do binary `e331cbe` lệch migration với bundle `b74ea144`; chạy lại với bundle `3aca28e8` cùng nhánh thì đạt (mục 4.1). Máy có HT, `--ms-cpus 0-15` làm MS và bộ sinh tải dùng chung lõi vật lý, đổi sang `0-7,16-23` / `8-15,24-31` |
| G-a sơ bộ (2026-10-16) | Không crash, không `database is locked`. Ở bậc 500/600/800, `loadServerConfigJson` (nạp cấu hình mỗi 10 s) chặn một event poller 3–7 s, toàn bộ luồng kéo camera trên poller đó `receive rtp timeout` rồi kết nối lại, mất ~10–25 s ghi hình; bundle không bắt vì xảy ra trước cửa sổ đo. Lượt chạy nhầm cách ghim lõi cũ và giữ bậc 60 s (mục 10.1) |
| G-b sơ bộ (2026-10-16) | Không crash, không lỗi join, không rớt luồng (nạp cấu hình ở 300 camera ≤ 1,1 s). **SSD chậm dần khi ghi liên tục**: cùng ~148 MB/s, sau ~25 phút util trung bình tăng 21 % → 48 %, đỉnh thông lượng tụt 510 → 330 MB/s, `w_await` 32 → 81 ms (đỉnh 120 ms); `DiskIoMonitor` cảnh báo await ≥ 50 ms 4 lần. Healthcheck có mẫu lẻ 0,9–2 s ở bậc 750/1250/1500 (p95 vẫn ≤ 3,2 ms) trùng lúc đĩa bão hoà. Vẫn chạy `0-15`/`16-31` |
| G-c sơ bộ (2026-10-16) | Live sạch, replay trượt 16/80. Mỗi phút 600 segment đóng dồn trong ~10 s; binary `e331cbe` chạy việc đóng segment + listener `kBroadcastRecordMP4` (timefile, SQLite tier, archive size) trên **work poller**, cùng chỗ với `MP4Reader` của replay → work poller bị chiếm 2–8,5 s, mọi replay trên poller đó khựng cùng lúc. Binary thiếu commit `489f98e3` của develop (pool "rec close"). `DiskIoMonitor` cảnh báo await 7 lần; đọc đĩa không chậm (`r_await` TB 4,9 ms) |

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
- **Không gồm**: transcode/overlay, motion, WebRTC, tier lên NAS/S3, đồng bộ cụm. Các tính năng này
  tốn thêm CPU/IO trên production và cần đo riêng nếu bật.

---

## 10. Đánh giá kết quả

### 10.1 Lượt G, bước a — ngưỡng camera (sơ bộ, 2026-10-16)

Tarball `result_G_a.tar.gz` (`result_db3_G_ssd_c16_m32_20261016_144730`): binary `e331cbe`, bundle
`3aca28e8`, profile full (H.264 1080p 4 Mbps + 360p 512 kbps), 0 viewer, ghi 24/7 cả hai luồng, SSD,
bậc 100 → 800 (dừng sau 2 bậc trượt 700, 800), tổng 20 phút. Số liệu từng bậc ở
[Bảng 4](#bảng-4--chi-tiết-bậc-camera-một-bảng-cho-mỗi-lượt).

**Lượt này chưa đúng điều kiện G, chỉ dùng làm số sơ bộ:**

1. **Ghim lõi theo cách cũ** `--ms-cpus 0-15 --gen-cpus 16-31` (`system.txt`: `ms_cpus=0-15`).
   MediaServer có một luồng HT trên mỗi lõi trong 16 lõi vật lý, chia lõi với bộ sinh tải và trải
   trên hai NUMA node; G theo ma trận là `0-7,16-23` (8 lõi vật lý trọn vẹn).
2. **Giữ mỗi bậc 60 s** (ramp 62 s + đo 62 s, cửa sổ chấm 30 s) thay vì chờ 180 s + giữ 600 s:
   lệnh còn `CAM_HOLD_SEC=60 PHASE_SEC=60` từ lần chạy thử. Chưa thấy rò rỉ bộ nhớ dài hạn, tốc độ
   ghi bền vững của SSD (mỗi bậc chỉ ghi 5–40 GB) hay việc xoá bản ghi khi ổ gần đầy.

**Tài nguyên còn dư nhiều ở mọi bậc:**

| Yếu tố | 600 camera (bậc đạt cao nhất) | 800 camera | Nhận xét |
|---|---|---|---|
| CPU | 28 % của 16 vCPU | 37 % | tăng ~0,045 %/camera, còn xa ngưỡng 80 % |
| Event poller | tổng 441 % một lõi, poller bận nhất 42 % | 580 %, max 42 % | chia đều 16 poller (top 4 cùng ~30–39 %) |
| RAM | RSS 6,9 GB (~11 MB/camera) | 9,0 GB | ở 800 camera là 28 % RAM; ngưỡng 70 % ứng với ~2 000 camera nếu tuyến tính |
| SSD | 305 MB/s trung bình, `%util` TB 26 %, đỉnh 66 %, `w_await` TB 15 ms | 408 MB/s, util TB 36 %, đỉnh 71 % | `vmstat wa` ≤ 1 %, ghi theo đợt flush page cache (đỉnh 650 MB/s) |
| Mạng (loopback) | 2,5 Gbps vào | 3,3 Gbps | **vượt `bond0` 2 Gbps của db3**, xem dưới |
| Ghi hình | 1 200 luồng, 0 hở trong cửa sổ đo | 1 600 luồng, 0 hở trong cửa sổ | nhưng có luồng rớt ngoài cửa sổ, xem dưới |

**Vì sao bậc 700, 800 trượt: healthcheck p95 231 / 280 ms > 200 ms.** Bundle gán yếu tố giới hạn
là "poller", nhưng poller bận nhất chỉ 18 % (700) và 42 % (800), CPU 32–37 %. Các mẫu chậm nằm
trong dải hẹp: p95 231 ms, max 263 ms ở 700; p95 280 ms, max 484 ms ở 800. Dải này khớp với một lần
truyền lại TCP (RTO tối thiểu 200 ms) trên loopback khi lưu lượng ~3 Gbps gói RTP nhỏ làm tràn hàng
đợi (`net.core.netdev_max_backlog`), hơn là poller bị nghẽn. Đây là **giả thuyết**: tarball không có
số đếm TCP retransmit/drop để xác nhận. Nếu đúng thì đây là giới hạn của cách đo (loopback), không
phải của MediaServer.

**Lỗi thật phát hiện được: nạp cấu hình chặn event poller, làm rớt camera.** Cứ mỗi
`hook.report_interval` (10 s), `loadServerConfigJson` chạy trên **một event poller đang kéo camera**
(log: `event poller N`) và gọi `addCamera` cho toàn bộ camera. Thời gian (log
`Server configuration for camera loaded completed, took … ms`):

| Bậc | Lần nạp chậm nhất | Hệ quả |
|---|---|---|
| 100–300 | 0,24–0,57 s | không |
| 400 | 2,9 s | không |
| 500 | 4,7 s trên poller 1 (14:58:18) | cùng giây, 64 dòng `receive rtp timeout` trên poller 1, kết nối lại xong sau ~11 s |
| 600 | 3,0 s (15:00:18), trước đó poller 7 im 17,9 s lúc khởi động | 15:00:29, 53 dòng `rtp timeout` trên poller 7, kết nối lại tới 15:00:41 |
| 700 | 3,3 s | không |
| 800 | 6,9 s trên poller 7 (15:06:13 → 15:06:20) | 15:06:21, 100 dòng `rtp timeout` trên poller 7 (~1/16 số luồng), kết nối lại tới 15:06:32 |

Mỗi luồng bị rớt mất khoảng 10–25 s ghi hình. **Bundle không bắt được** vì các lần rớt rơi vào pha
ramp, trước cửa sổ chấm (ví dụ bậc 800 chấm từ 15:06:35). Kiểm tra `record_gaps` chỉ xét khoảng hở
giao với cửa sổ, `ingest_timeouts` chỉ đếm dòng log mới trong cửa sổ. Lần nạp chậm nhất thường là
1–3 lần đầu sau khởi động (camera đang lên, tier/DB bận), nhưng 0,3–1,5 s mỗi 10 s ở trạng thái ổn
định vẫn là thời gian một poller ingest không xử lý gói.

Code hiện tại (`server/WebHook.cpp`, `WorkThreadPool::Instance().getPoller()->async(...)` rồi
`do_http_hook`) có ý định chạy việc này ngoài event poller, nhưng log của binary cho thấy callback vẫn
chạy trên event poller. Cần kiểm lại trên develop. Việc nạp cấu hình là blocking (gọi `addCamera`
cho hàng trăm camera, đụng tier/SQLite) nên đúng chỗ là pool `BlockingExecutors`, không phải event
poller hay `WorkThreadPool`.

**Kết luận sơ bộ cho G-a:**

- Bundle báo ngưỡng **600 camera** (1 200 luồng). Nếu tính cả luồng bị rớt ngoài cửa sổ đo thì bậc
  sạch cao nhất là **400 camera**, và giới hạn đến từ lỗi chặn poller ở trên, không phải từ CPU, RAM
  hay SSD.
- Tài nguyên tại 800 camera vẫn dư: CPU 37 %, RAM 28 %, SSD util trung bình 36 %. Sau khi sửa lỗi nạp
  cấu hình, ngưỡng theo phần cứng có thể cao hơn 800; cần đo lại.
- **Mạng là giới hạn thực tế của db3**: mỗi camera cần ~4,15 Mbps, `bond0` 2 Gbps dùng an toàn 80 %
  = 1 600 Mbps ứng với **~385 camera khi chưa có người xem**. Camera thật đi qua NIC, nên trên db3
  số camera bị giới hạn bởi mạng trước khi chạm CPU/RAM/SSD. Mục 2.4 tính theo 10 Gbps không áp dụng
  cho máy này.

**Việc cần làm trước khi chốt lượt G:**

1. Chạy lại G-a đúng điều kiện: `--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31`, **bỏ**
   `CAM_HOLD_SEC=60 PHASE_SEC=60` (dùng mặc định chờ 180 s + giữ 600 s), tag `G_ssd_c16_m32_r2`.
2. Trong lúc chạy, ghi lại `nstat -az TcpRetransSegs TcpExtTCPLossProbes TcpExtListenOverflows` và
   `/proc/net/softnet_stat` trước và sau mỗi bậc, để xác nhận hay loại giả thuyết RTO loopback.
3. Sửa harness: tính cả `receive rtp timeout` và khoảng hở ghi hình trong pha ramp (từ lúc camera đã
   online), không chỉ trong cửa sổ chấm.
4. Sửa MediaServer: đưa `loadServerConfigJson` khỏi event poller (pool blocking), rồi đo lại bậc
   500–800.

### 10.2 Lượt G, bước b — ngưỡng luồng xem trên 300 camera (sơ bộ, 2026-10-16)

Tarball `result_G_b.tar.gz` (`result_db3_G_ssd_c16_m32_view_20261016_151139`): cùng binary/bundle
với G-a, 300 camera nền (600 luồng, ghi 24/7), viewer 0 → 2 000 theo mix `live` (HTTP-MP4/HLS luồng
chính và phụ), mỗi bậc đo 120 s (bậc 500 đo 270 s), tổng 38 phút. Lượt này **cũng chạy với
`--ms-cpus 0-15 --gen-cpus 16-31`**, nên có cùng hạn chế ghim lõi như G-a; thời gian giữ bậc thì đúng.

**Kết quả: mọi bậc đều đạt, CCU ≥ 2 000, chưa chạm trần; nghiệm thu A1 (bậc 500) đạt, dư 300 %.**

| viewer | CPU % | event poller (% 1 lõi) | poller max % | RSS MB | TX loopback Mbps | TTFF max ms | khựng % | healthcheck p95 / max ms |
|---|---|---|---|---|---|---|---|---|
| 0 | 19,6 | 288 | 15,6 | 3 775 | 1 244 | – | – | 2,8 / 468 |
| 500 | 19,8 | 291 | 24,6 | 4 076 | 1 999 | 7 | 0,007 | 2,2 / 57 |
| 1 000 | 21,9 | 329 | 23,8 | 4 215 | 2 706 | 14 | 0,018 | 3,2 / 61 |
| 1 500 | 24,3 | 370 | 30,9 | 4 231 | 3 474 | 112 | 0,019 | 2,6 / 1 982 |
| 2 000 | 26,9 | 412 | 26,7 | 4 259 | 4 200 | 159 | 0,006 | 3,2 / 72 |

- **Phía người xem sạch**: 2 450 lượt mở luồng, 0 lỗi, 0 phiên đứt bất thường, 0 phiên khựng dài,
  khựng tối đa 0,079 % (bậc 750), trôi p95 ≤ 0,54 s. Ghi hình 600 luồng không hở ở mọi bậc.
- **Chi phí mỗi người xem rất nhỏ**: CPU tăng ~0,0037 % của 16 vCPU (~0,06 % một lõi) và RSS
  ~0,24 MB mỗi viewer. Theo CPU, MediaServer còn chịu được hàng chục nghìn viewer, nên CPU không
  phải giới hạn của luồng xem.
- **Bộ sinh tải hợp lệ**: 8 worker viewer CPU tối đa 58,5 %, loop lag ≤ 12,6 ms; nguồn RTSP CPU
  ≤ 8,3 %, tick trễ ≤ 21 ms.
- **Không rớt luồng ngoài cửa sổ đo**: với 300 camera, nạp cấu hình mỗi 10 s trung bình 239 ms, chậm
  nhất 1,1 s, không có `receive rtp timeout` nào. Lỗi chặn poller ở mục 10.1 chỉ gây rớt từ khoảng
  500 camera trở lên.
- **Healthcheck p95 tốt (≤ 3,2 ms)**, nhưng có mẫu lẻ 0,9–2 s ở bậc 750, 1 250, 1 500, trùng lúc SSD
  bão hoà (xem dưới). Có thể do poller ghi file bị chặn khi kernel dồn page cache bẩn, nhưng chưa
  chứng minh được.

**Phát hiện: SSD không giữ được tốc độ ghi khi ghi liên tục.** Tốc độ ghi hình không đổi
(~148–155 MB/s, 300 camera, ~0,52 MB/s mỗi camera), nhưng đĩa ngày càng chậm (`iostat` của `dm-3`):

| Khoảng | Ghi TB MB/s | Đỉnh MB/s (mẫu 5 s) | `w_await` TB ms | util TB | mẫu util ≥ 99 % |
|---|---|---|---|---|---|
| 15:10–15:20 | 137 | 515 | 32 | 18 % | 0/100 |
| 15:20–15:30 | 149 | 503 | 34 | 21 % | 0/120 |
| 15:30–15:40 | 147 | 484 | 50 | 29 % | 1/120 |
| 15:40–15:50 | 148 | 337 | 81 | 48 % | 14/120 |

Từ ~15:37 (khoảng 25 phút ghi liên tục, ~250 GB trên lượt này, chưa kể G-a ngay trước), đỉnh
thông lượng tụt từ ~510 xuống ~330 MB/s và đĩa bão hoà theo từng đợt flush. `DiskIoMonitor` cảnh báo
await ≥ 50 ms lúc 15:39, 15:44, 15:47, 15:49. Hiện tượng này giống cache ghi của SSD hoặc của RAID
SMC3108 bị dùng hết; cần xác nhận với cấu hình RAID (write-back cache, BBU) và model SSD thật.

Hệ quả cho ngưỡng camera:

- Ở trạng thái đã chậm, util 48 % ứng với ~148 MB/s, tức đĩa chỉ còn **~300–330 MB/s bền vững**,
  tương đương **~580–630 camera** (0,52 MB/s/camera) ở util 100 %. Muốn giữ util ≤ 70 % thì chỉ còn
  **~400–450 camera**.
- G-a đạt tới 600 camera (305 MB/s) và đo 800 camera (408 MB/s) **chỉ vì mỗi bậc ghi 60 s**, khi
  đĩa còn trong vùng nhanh. Với thời gian giữ 600 s và ghi liên tục, nhiều khả năng đĩa sẽ là giới
  hạn trước healthcheck. Lượt G-a chạy lại (mục 10.1) cần theo dõi `w_await` và util ở cuối mỗi bậc.

**Mạng trên db3 (tính, không đo):** NIC full-duplex, ingest camera đi chiều RX, viewer đi chiều TX.
Mix `live` trung bình ~1,45 Mbps mỗi viewer (2 909 Mbps / 2 000). Với `bond0` 2 Gbps dùng an toàn
80 % (1 600 Mbps mỗi chiều):

- RX: ~385 camera (4,15 Mbps/camera), như mục 10.1.
- TX: **~1 100 viewer** theo mix này. Đây là giới hạn luồng xem thực tế của db3, thấp hơn nhiều so
  với khả năng CPU của MediaServer.

**Kết luận sơ bộ cho G-b:** trên 300 camera, MediaServer phục vụ ≥ 2 000 viewer với CPU 27 %, RSS
4,3 GB, không lỗi. Luồng xem không bị giới hạn bởi MediaServer mà bởi mạng (~1 100 viewer với
`bond0` 2 Gbps). Rủi ro thật ở cấu hình 300 camera là **đĩa chậm dần khi ghi liên tục**, không phải
luồng xem.

**Việc cần làm:**

1. Xác nhận loại SSD, cấu hình RAID SMC3108 (cache, chính sách ghi) và thử ghi tuần tự dài để đo
   tốc độ bền vững thật của `/work` (ví dụ `fio --rw=write --bs=1M --size=200G --direct=1`).
2. Bước c (`live_replay`) đọc bản ghi song song với ghi, nên sẽ đẩy SSD đang ở vùng chậm lên cao hơn;
   theo dõi `w_await`/`r_await` và cảnh báo `DiskIoMonitor` khi phân tích G-c (đã làm, mục 10.3:
   đọc không chậm, lỗi nằm ở thread).

### 10.3 Lượt G, bước c — 20 % xem lại (sơ bộ, 2026-10-16)

Tarball `result_G_c.tar.gz` (`result_db3_G_ssd_c16_m32_replay_20261016_161228`): cùng binary/bundle,
300 camera nền, chờ 750 s cho đủ 900 s bản ghi, rồi 400 viewer gồm 320 live + 80 replay
(HTTP-fMP4 VOD luồng chính, lùi 300–900 s, mỗi phiên xem 300 s). Replay mở lúc 16:29:13, đóng lúc
16:34:34. Lượt này **cũng chạy `0-15`/`16-31`**.

**Kết quả: KHÔNG ĐẠT.** Replay 64/80 phiên đạt (80 %, cần ≥ 99 %). Live 320/320 đạt (TTFF max
75 ms, không phiên khựng > 2 s), ghi hình 600 luồng không hở, CPU 20 %, poller max 17,6 %.

| | Giá trị |
|---|---|
| Phiên replay trượt | 16/80, mỗi phiên đúng 1 lần khựng > 2 s (2,0–7,8 s) |
| Media nhận / thời gian xem | bằng nhau (~300 s) ở mọi phiên: server **dừng đẩy rồi đẩy bù**, không mất dữ liệu, không kết nối lại |
| TTFF replay | p95 ~1,3–6,4 s, max 8 568 ms (ngưỡng 10 s) |
| Đọc đĩa trong lúc replay | ~22 MB/s, `r_await` TB 4,9 ms, max 22 ms; ghi vẫn ~147 MB/s, `w_await` TB 75 ms, util TB 47 % |

**Nguyên nhân: đóng segment ghi hình chặn work poller, nơi `MP4Reader` của replay cũng chạy.**

1. `MP4Reader` của mỗi replay chạy trên một poller của `WorkThreadPool` (16 work poller, log
   `OwnerPoller changed event poller N -> work poller M`), khoảng 5 phiên mỗi poller.
2. Binary `e331cbe` đóng segment và chạy các listener `kBroadcastRecordMP4` (`Manager.cpp:236`
   timefile, `registerHotSegmentRange` ghi SQLite tier, `addArchiveSize`) **cũng trên work poller**:
   13 800/13 800 dòng `Record mp4 file` đến từ `work poller N`, không có thread `rec close`.
3. 300 camera khởi động cùng lúc nên 600 segment (60 s) đóng dồn vào khoảng giây :25–:35 mỗi phút.
   Mỗi segment chiếm work poller ~0,3–0,5 s (SQLite + đĩa đang có `w_await` 75–120 ms). Việc chia
   không đều: `work poller 12` nhận 738 lần đóng segment, `work poller 0/10/15` gần như không có.
   Mỗi đợt, một số work poller bị chiếm liên tục 2–8,5 s.
4. Mọi replay nằm trên poller bị chiếm đều khựng cùng một khoảng. Đối chiếu từng phiên với work poller
   của nó:

| work poller | phiên replay | trượt | khựng dài nhất của từng phiên (s) | đợt đóng segment dài nhất trên poller (s) |
|---|---|---|---|---|
| 12 | 3 | 2 | 7,76; 7,71; 0 | 8,5 |
| 9 | 5 | 3 | 4,81; 4,79; 4,48 | 5,5 |
| 7 | 3 | 3 | 3,85; 3,76; 3,64 | 7,4 |
| 14 | 2 | 2 | 3,58; 3,38 | 7,2 |
| 3 | 3 | 3 | 2,81; 2,77; 2,58 | 7,0 |
| 6 | 4 | 2 | 2,02; 2,01; 1,98; 1,41 | 3,7 |
| 13 | 6 | 1 | 2,10; 1,69; … | 6,2 |
| 0, 10, 15 | 24 | 0 | ≤ 0,68 | ≤ 0,5 |

   Hai phiên mở chậm nhất (8,6 s và 6,4 s) cũng được mở lúc 16:29:27, đúng khi work poller 12 và 9
   đang trong đợt đóng segment.

**Develop đã sửa phần lớn**: commit `489f98e3` (2026-09-28) chuyển đóng segment và listener
`kBroadcastRecordMP4` sang pool 1 thread "rec close N" theo stream (`MP4Recorder::closeExecutorFor`).
Nhánh `feature/anhnq48-replay-timeline` của binary đang test không có commit này. Còn lại trên develop:
`MP4Reader`/`MergedMP4Reader` vẫn mặc định chạy trên `WorkThreadPool`, nên mọi việc nặng khác rơi vào
work poller (ví dụ `loadServerConfigJson` theo ý định của code `WebHook.cpp`) vẫn có thể làm replay
khựng.

**Kết luận G-c:** lượt trượt vì lỗi phân luồng của bản build, không phải vì thiếu CPU, RAM hay tốc
độ đọc của SSD.

### 10.4 Tổng hợp lượt G và đề xuất khắc phục

**Tóm tắt (binary `e331cbe`, db3, SSD, 300 camera làm nền cho b/c):**

| Bước | Kết quả | Giới hạn thật | Còn dư |
|---|---|---|---|
| a — ngưỡng camera | bundle báo 600; sạch 400 | `loadServerConfigJson` chặn event poller 3–7 s → rớt 1/16 luồng (từ 500 camera); SSD chỉ giữ ~300–330 MB/s khi ghi lâu (~580 camera ở util 100 %) | CPU 37 %, RAM 28 % ở 800 camera |
| b — luồng xem | ≥ 2 000 viewer, chưa chạm trần | không có trong MediaServer; trên db3 NIC `bond0` 2 Gbps giới hạn ~1 100 viewer | CPU 27 % ở 2 000 viewer |
| c — 20 % replay | trượt, 64/80 | đóng segment chạy trên work poller, chặn `MP4Reader` | CPU 20 % |

Cả ba giới hạn tìm được đều là **việc blocking chạy trên poller**. Ở bất kỳ bước nào, CPU và RAM
cũng chưa phải giới hạn. Ngoài ra còn hai giới hạn phần cứng của db3: tốc độ ghi bền vững của
SSD/RAID và NIC 2 Gbps.

**Đề xuất khắc phục**, theo thứ tự nên làm trước khi chạy lại lượt G:

| # | Việc | Chỗ sửa | Kỳ vọng | Ưu tiên |
|---|---|---|---|---|
| K1 | Build lại binary test từ nhánh đã có `489f98e3` (đóng segment trên pool "rec close"), tức merge develop vào nhánh replay-timeline hoặc test thẳng develop. Kiểm log có thread `rec close N` | build | hết khựng replay do đóng segment (G-c), bớt tải work poller ở G-a | 1 |
| K2 | Đưa `loadServerConfigJson` ra khỏi poller: chạy trên `BlockingExecutors` (`api()` hoặc một executor serial riêng), callback `do_http_hook` chỉ chuyển JSON sang đó. Đồng thời bỏ qua nạp lại khi cấu hình không đổi (so hash JSON `devices`), để ở trạng thái ổn định không gọi `addCamera` cho cả trăm camera mỗi 10 s | `server/WebHook.cpp` (`report_callback`), `server/Manager.cpp` (`loadServerConfigJson`) | hết rớt luồng ở G-a; không tranh work poller với replay | 1 |
| K3 | Cho `MP4Reader`/`MergedMP4Reader` một nhóm poller riêng cho replay, không dùng chung `WorkThreadPool` (vốn chạy timer và hàng đợi khác); cân nhắc đọc trước vài giây media để vài trăm ms chặn không thành khựng | `src/Record/MP4Reader.cpp:32`, `MergedMP4Reader.cpp:96` | replay không bị ảnh hưởng bởi việc khác trên work poller | 2 |
| K4 | Rải thời điểm đóng segment: thêm độ lệch theo camera (ví dụ `hash(stream) % mp4_max_second`) cho segment đầu tiên, để 600 segment không đóng cùng một giây sau khi khởi động hay khởi động lại | `MP4Recorder` (điều kiện cắt file) | đợt đóng segment trải đều cả phút, đỉnh ghi SQLite/đĩa giảm | 2 |
| K5 | Làm mượt flush đĩa: thử `vm.dirty_background_bytes` nhỏ (ví dụ 256 MB) và `vm.dirty_bytes` (ví dụ 1–2 GB) thay vì 10 %/20 % RAM, để kernel ghi đều thay vì từng đợt 300–650 MB/s; kiểm cache ghi và BBU của RAID SMC3108, đo tốc độ ghi bền vững thật bằng `fio` | cấu hình máy (production) | bớt mẫu util 100 % và `w_await` > 100 ms; biết trần ghi thật để chốt ngưỡng camera | 2 |
| K6 | Sửa harness: (a) đếm `receive rtp timeout` và khoảng hở ghi hình cả trong pha ramp; (b) ghi thời điểm từng lần khựng trong `ccu_replay_sessions.csv`; (c) chụp `nstat`/`softnet_stat` mỗi bậc để xác nhận giả thuyết RTO loopback ở G-a; (d) cảnh báo khi `--ms-cpus` chia đôi cặp HT | `tests/integration/stress_vms/check_ccu.py`, `viewer_load.py`, `ccu_runner.sh` | lần sau không bỏ sót lỗi ngoài cửa sổ đo | 3 |
| K7 | Ghi giới hạn NIC vào báo cáo: db3 với `bond0` 2 Gbps chỉ nhận ~385 camera / phục vụ ~1 100 viewer; nếu máy Recording production dùng 10 Gbps thì cần đo lại bằng máy viewer thứ hai (mục 5.4) | báo cáo | – | 3 |

**Chạy lại lượt G sau khi sửa** (cùng thứ tự a → b → c):

- Binary: có K1 + K2 (tối thiểu), ghi commit vào Bảng 0.
- Ghim lõi đúng ma trận: `--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31`; **không** đặt
  `CAM_HOLD_SEC`/`PHASE_SEC` (giữ mặc định 180 s + 600 s).
- Tiêu chí kiểm thêm khi phân tích: không có `receive rtp timeout` ngoài lúc khởi động; mọi dòng
  `Record mp4 file` chạy trên thread `rec close`; replay ≥ 99 % phiên đạt; `w_await`/util của SSD ở
  cuối mỗi bậc camera.

### 10.5 Kiểm chứng local sau sửa (2026-10-17, chưa phải lượt G-r2)

Source: `fix/capacity-service`, các commit `4209b6c6` (harness), `d04e3b30`/`ec5ce710`
(config worker/readiness/shutdown), `6f7c6cea` (close metrics/replay poller/stagger) và `88231d20`
(revision của request khởi động). Không thay schema, không thay auth của endpoint thống kê.

Build Debug trong `build-test/`, C++11, FFmpeg bundled 7.1.5. Build MediaServer đạt; bốn bộ CTest
`unit.replay_all`, `unit.storage_all`, `unit.timeline_query_all`, `unit.server_config_loader_all`
đạt: **37 + 92 + 5 + 6 = 140 test C++**. Regression Python của harness **5/5**, `bash -n` đạt.
Các regression mới kiểm tra apply bị chặn không chặn poller, retry/cache/audio-only/revision,
shutdown, metrics khi task throw/cancel, poller mặc định/explicit và stagger/continuous/segment 1 s.

Chạy thật bằng `run_stress.sh`, một node, nguồn **light 176x144/5 fps/48 kbps**, hai luồng/camera,
không viewer; `-t 4 --affinity 0`, ramp 40 s + giữ 40 s (cửa sổ chấm 30 s), segment 20 s,
`hook.configReapplySec=30`. Nguồn giả lập dùng cổng 28554 vì 18554 đã được sử dụng; không dừng
tiến trình trên cổng đó. Đây là kiểm chứng chức năng/threading, **không dùng để suy ra capacity**.

| camera | luồng | timeout từ khởi động | khoảng hở / stale | queue cuối cửa sổ | wait max từng shard (ms) | kết quả |
|---|---|---|---|---|---|---|
| 20 | 40 | 0 | 0 / 0 | 0 | 83,663 | đạt |
| 50 | 100 | 0 | 0 / 0 | 0 | 167,466 | đạt |
| 120 | 240 | 0 | 0 / 0 | 0 | 674,289 | đạt |

Mọi dòng apply camera ở ba lượt nằm trên `api worker`, mọi dòng Record mp4 file trên `rec close`.
Report lưu được samples của cả hai shard. Ở 120 camera, 240 segment đầu dài **9,8–19,8 s** theo
media DTS (có lượng tử frame/GOP), đúng mục đích rải đợt đầu; các segment sau giữ ngưỡng 20 s.
Bảng summary mới được render thử bằng chính ba report: đủ cột timeout ngoài cửa sổ và verdict.

Artifacts local: `/tmp/s3mk-capacity-local.vJvYdT/cam_{20,50,120}` (report JSON/Markdown và log).
SHA256 binary smoke: `58396a3710a50323fe5f8b0b2169001cb16d783738c5d5c27d0ff35230a186a5`.
Binary thêm sửa revision startup dùng cho lượt replay:
`c7555fc4c3282f43cdab1f8d98f0443ec33a684493a0832f2a2e8a0ae8b63139`.
Metadata Git của các binary kiểm thử này trống do sandbox không đọc được refs; xác định bằng SHA
và source ở trên. Bản giao cho lượt G phải build lại ngoài sandbox, kiểm Git hash/bundle cùng commit.

Replay local 20 camera / 20 viewer (16 live + 4 replay), lùi 30–50 s, xem 20 s: **đạt 4/4 replay**,
TTFF tối đa 545 ms, không khựng, không timeout/hở/stale recording. Log xác nhận OwnerPoller chuyển
sang replay poller. Artifacts ở `/tmp/s3mk-capacity-local.vJvYdT/replay_20`, gồm
`ccu_replay_report.json/.md` và CSV có trường stall_events. Đây là kiểm tra replay fMP4 nhỏ;
không thay lượt G-c 80 phiên / 300 s, replay HLS/RTSP, A/B hay soak trên db3.

**Chưa xác nhận:** throughput trên SSD db3 khi đã hết cache, mức closeThreads tối ưu (vẫn giữ 2),
soak dài, so A/B P3/P4 trên tải bitrate thật và capacity G-r2. Chưa chạy fio, đổi sysctl hoặc chạm db3.
