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
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf "$(ls /usr/lib/linux-tools/*/perf | head -1)" /usr/local/bin/perf
```

```bash
docker build -f Dockerfile.captest -t s3mk-captest .
docker run --rm s3mk-captest bash -c 'python3 --version; taskset -V; ls /opt/media/bin/MediaServer; perf --version'
```

`perf` (gói `linux-tools-generic`) dùng để lấy hàm nóng ở bậc 500 của `live_step`. `/usr/bin/perf` của
Ubuntu chỉ là script tìm `perf` khớp kernel đang chạy; trong container kernel là của host nên script
báo thiếu gói và thoát. Lệnh `ln -sf` ở trên trỏ thẳng tới binary trong `/usr/lib/linux-tools/`, binary
này chạy được với kernel khác phiên bản. Chạy container với `--privileged` (hoặc
`--cap-add SYS_ADMIN`) và đặt `kernel.perf_event_paranoid` ≤ 1 trên host. Nếu `perf` vẫn không chạy
thì bỏ qua; các số khác vẫn đủ.

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

**Đợt đo 2 (2026-10-19 16:44, `result_db3_smoke_20261019_164431`): đạt.** Binary và bundle cùng
commit `3388afc` (nhánh `feature/anhnq48-health-monitor`, có các sửa của mục 10.4/10.5), cùng ghim lõi
`0-7,16-23` / `8-15,24-31`, `CAM_HOLD_SEC=60 PHASE_SEC=60`, profile full.

| camera | luồng online | CPU % (lõi MS) | poller max % | event poller tổng (% 1 lõi) | RSS MB | ghi đĩa MB/s | NIC vào Mbps | healthcheck p95 ms | timeout ngoài cửa sổ | thread config / record | kết luận |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 20 | 40 | 3,5 | 2,0 | 31 | 330 | 12,4 | 83,3 | 2,0 | 0 | `api worker` / `rec close` 53 | đạt |
| 50 | 100 | 6,1 | 3,7 | 72 | 704 | 27,6 | 207,5 | 2,2 | 0 | `api worker` / `rec close` 130 | đạt |

- CPU, poller, ghi đĩa, mạng gần như trùng lần 2 của đợt 1. RSS cao hơn ~10 % (330/704 so với
  297/635 MB).
- Thread chạy đúng như bản sửa: nạp cấu hình chạy trên `api worker`, mọi lần đóng segment chạy trên
  `rec close`, không còn trên event poller hay work poller.
- Monitor `/media/monitor/summary` ở bậc 50 báo `pollers.delay_max_ms` warning (128 ms > 50 ms) ở
  3/3 mẫu, trong khi healthcheck p95 2,2 ms. Bundle đánh dấu là ứng viên báo giả, xem mục 10.6.

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

### 5.6 Sau lượt G: baseline A/B và kiểm đĩa (P6)

Mục này là phần đo trên máy của [capacity-followup-plan.md](capacity-followup-plan.md) (L2, L5, baseline A/B). Chạy trên db3 với **cùng binary và bundle `3388afc`** của đợt 2 (mục 10.6–10.9), không cần
build mới. Có hai việc:

- **Baseline A/B**: đo riêng hiệu quả của hai sửa đổi bật/tắt được.
- **P6**: biết trần ghi thật của SSD/RAID và thử sysctl ghi đĩa.

Thứ tự nên chạy, tổng ~5–6 giờ: **P6 bước 1–3 → A/B → P6 bước 4 + `closeThreads`**. Trần đĩa
quyết định cách đọc mọi lượt sau, nên đo trước.

Phần đầu lệnh dùng chung (`$D`, `$B` như đầu mục 5):

```bash
G="--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31"
OFF="--set record.replayPollerThreads=0 --set record.segmentStartStagger=0"
```

**Trước mỗi lượt**, khi máy rảnh, ghi lại trạng thái đĩa để so các lượt với nhau:

```bash
iostat -x 5 12 > /data/cap/iostat_idle_$(date +%H%M).txt
```

#### 5.6.1 P6, bước 1–3: phần cứng, TRIM, tốc độ ghi bền vững

Chạy trên **host** db3, khi MediaServer production và mọi test đã dừng. Các bước này cần quyền admin.
`fstrim` và `sysctl` tác động lên cả máy, nên phải được admin đồng ý trước.

**Bước 1. Thông tin RAID và SSD** (SMC3108 là controller LSI/Broadcom 3108):

```bash
storcli64 /c0 show                                            # mức RAID, các ổ vật lý
storcli64 /c0/vall show all | grep -iE "cache|policy|raid"    # WriteBack/WriteThrough, ReadAhead
storcli64 /c0/cv show all || storcli64 /c0/bbu show all       # pin/supercap của cache
smartctl -i -d megaraid,0 /dev/sda                            # model SSD; đổi 0,1,… theo từng ổ
```

**Bước 2. RAID có chuyển TRIM xuống SSD không.** Đây là nghi vấn chính cho việc cùng tải nhưng util
SSD có lúc 17 %, có lúc 44 % (mục 10.7).

```bash
lsblk -D /dev/sda     # DISC-GRAN/DISC-MAX = 0: không có TRIM
fstrim -v /home       # /work nằm trên LV centos-home; "not supported" = không có TRIM
```

**Bước 3. Tốc độ ghi bền vững bằng `fio`.** Mỗi lần chạy 15–20 phút, đủ lâu để vượt qua cache của
RAID và SSD:

```bash
mkdir -p /data/cap/fio && cd /data/cap/fio
# (a) ghi tuần tự trực tiếp: trần phần cứng
fio --name=seq --rw=write --bs=1M --size=200G --direct=1 --ioengine=libaio --iodepth=16 \
    --status-interval=30 --write_bw_log=seq --log_avg_msec=5000 | tee fio_seq_1.txt
# (b) giống ghi hình: nhiều file ghi nối qua page cache
fio --name=rec --rw=write --bs=64k --numjobs=16 --nrfiles=40 --size=12G --direct=0 \
    --end_fsync=1 --status-interval=30 | tee fio_rec.txt
```

Chạy (a) **hai lần**: lần đầu khi đĩa "sạch", lần thứ hai ngay sau khi xoá 200–500 GB (ví dụ ngay sau
một lượt `cam_step` dài, khi harness đã dọn bản ghi), lưu vào `fio_seq_2.txt`. Nếu lần 2 chậm hơn rõ
và bước 2 cho thấy không có TRIM, giả thuyết ở mục 10.7 được xác nhận. Kết quả (a) là trần ghi để chốt
ngưỡng camera (~0,51 MB/s mỗi camera ở profile full). Đo xong thì xoá dữ liệu thử:
`rm -rf /data/cap/fio/*.0`, giữ lại các file `.txt` và `.log`.

#### 5.6.2 Baseline A/B

Hai khoá trong `[record]` được đọc lúc MediaServer khởi động, nên `--set` có tác dụng:

| Khoá | Mặc định (B, đợt 2) | Baseline (A) | Khi tắt |
|---|---|---|---|
| `record.replayPollerThreads` | 8 | 0 | `MP4Reader` của replay quay về `WorkThreadPool` (P3) |
| `record.segmentStartStagger` | 1 | 0 | segment đầu không rải, mọi camera đóng segment cùng lúc (P4) |

Đóng segment trên `rec close` và nạp cấu hình trên `api worker` không tắt được. Vì vậy baseline **không
quay lại đúng đợt 1**: nó chỉ đo phần đóng góp của P3 và P4.

**A và B phải chạy liền nhau, cùng trạng thái đĩa.** Không so baseline mới với tarball đợt 2 cũ, vì
SSD chậm khác nhau tới ~2,5 lần tuỳ lúc (mục 10.7).

```bash
# c — replay: hiệu quả P3 (~23 phút mỗi lượt)
$D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only live_replay --cameras 300 $G $OFF --tag Gc_base --yes
$D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only live_replay --cameras 300 $G      --tag Gc_new  --yes

# a — 3 bậc gần ngưỡng: hiệu quả P4 lên hàng đợi đóng segment (~42 phút mỗi lượt)
$D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only cam_step --cam-steps 500,600,700 $G $OFF --tag Ga_base --yes
$D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only cam_step --cam-steps 500,600,700 $G      --tag Ga_new  --yes
```

So sánh:

| Cặp | Chỉ số |
|---|---|
| `Gc_base` / `Gc_new` | phiên replay đạt, khựng dài nhất, TTFF replay p95/max (bảng phiên trong `SUMMARY.md`) |
| `Ga_base` / `Ga_new` | hàng đợi `rec close` (`queued`, `oldest_queued_ms` trong `ccu_report.json`), listener p99 (`MP4 close timing` trong log), healthcheck p95/max, `receive rtp timeout` |

Nếu một cặp khác biệt rõ, chạy thêm hai lượt mỗi lượt chỉ tắt một khoá
(`--set record.replayPollerThreads=0` hoặc `--set record.segmentStartStagger=0`, tag `*_noreplay`,
`*_nostagger`) để biết khoá nào có tác dụng. Nếu không khác biệt thì bỏ qua.

#### 5.6.3 P6, bước 4: sysctl ghi đĩa, và số thread `rec close` (L2)

**sysctl dirty.** Container dùng chung kernel với host, nên đặt trên **host**, và nhớ trả lại sau khi đo:

```bash
sysctl vm.dirty_ratio vm.dirty_background_ratio vm.swappiness | tee /data/cap/sysctl_old.txt
sysctl -w vm.dirty_background_bytes=268435456 vm.dirty_bytes=2147483648 vm.swappiness=10

$D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only cam_step --cam-steps 500,600,700 $G --tag Ga_dirty --yes

# trả lại: ghi *_ratio sẽ tự xoá *_bytes; swappiness lấy từ sysctl_old.txt
sysctl -w vm.dirty_ratio=20 vm.dirty_background_ratio=10 vm.swappiness=<giá trị cũ>
```

So `Ga_dirty` với `Ga_new` theo: `w_await`, util, số mẫu util ≥ 99 %, `vmstat` `b`/`wa`, listener
p99, healthcheck max. Chỉ đề xuất cấu hình này cho production nếu tốt hơn rõ. Nếu `Ga_new` đã chạy
cách đó lâu, chạy lại `Ga_new` ngay trước `Ga_dirty`.

**Số thread `rec close`** (sysctl để mặc định):

```bash
for n in 4 8; do
  $D -v /data/cap:/work -w /work s3mk-captest ./ccu_test.sh $B --only cam_step --cam-steps 700 $G \
     --set record.closeThreads=$n --tag Ga_close$n --yes
done
```

So với bậc 700 của `Ga_new` (2 thread): hàng đợi và thời gian chờ `rec close`, RSS, số fd `file`,
healthcheck.

#### 5.6.4 Gửi lại

- Tarball của các tag `Gc_base`, `Gc_new`, `Ga_base`, `Ga_new`, `Ga_dirty`, `Ga_close4`, `Ga_close8`
  (và `*_noreplay`, `*_nostagger` nếu có chạy).
- Output bước 1 (`storcli64`, `smartctl`), bước 2 (`lsblk -D`, `fstrim`), bước 3 (`fio_seq_1.txt`,
  `fio_seq_2.txt`, `fio_rec.txt`, `seq_bw.*.log`).
- `sysctl_old.txt` và các file `iostat_idle_*.txt`.
- Thứ tự và giờ chạy thực tế của từng lượt.

### 5.7 Điều khiển các lượt từ host

[`tests/integration/stress_vms/capacity_matrix.sh`](../tests/integration/stress_vms/capacity_matrix.sh)
chạy trên **host**. Mỗi kịch bản là một lần `docker run … ./ccu_test.sh` với đúng lệnh của mục 5.1–5.3
và 5.6. Các bước P6 chạy thẳng trên host. Script chạy một hoặc nhiều kịch bản theo thứ tự trên dòng
lệnh, rồi in bảng kết quả và danh sách tarball cần gửi lại. Chép file vào cùng thư mục với
`ccu_test.sh` (mặc định `/data/cap`):

```bash
cd /data/cap
./capacity_matrix.sh --list                         # liệt kê kịch bản và tuỳ chọn
./capacity_matrix.sh --dry-run all                  # in lệnh sẽ chạy
nohup ./capacity_matrix.sh all > matrix.out 2>&1 &  # smoke G C4 C8 M16 M8, ~14 giờ
./capacity_matrix.sh C4-b:200                       # một bước, ép N = 200 camera
./capacity_matrix.sh p6 ab C24                      # P6, baseline A/B, rồi lượt C24
./capacity_matrix.sh --suffix _r3 --set record.closeThreads=4 G-a
```

| Kịch bản | Chạy gì |
|---|---|
| `smoke` | 20/50 camera, giữ 60 s; trượt thì dừng cả chuỗi |
| `G`, `C4`, `C8`, `C24`, `M16`, `M8`, `H` | cả lượt: a, b (G và H thêm c) |
| `<lượt>-a` | `cam_step` (ngưỡng camera) |
| `<lượt>-b[:N]`, `<lượt>-c[:N]` | `live_step` / `live_replay` (c chỉ ở G, H) trên N camera |
| `ab` hoặc `Gc_base` `Gc_new` `Ga_base` `Ga_new` | baseline A/B (mục 5.6.2) |
| `close` hoặc `Ga_close4` `Ga_close8` | `record.closeThreads` 4/8 ở 700 camera (mục 5.6.3) |
| `dirty` | đặt sysctl dirty nhỏ trên host, chạy `Ga_dirty`, trả lại sysctl (cả khi Ctrl+C) |
| `diskinfo` | `storcli64`, `smartctl`, `lsblk -D`, `iostat` (mục 5.6.1 bước 1) |
| `trim` | `fstrim -v` trên mount chứa `/data/cap`; **admin phải đồng ý trước** |
| `fio-seq`, `fio-rec` | `fio` ghi tuần tự 200 GB / ghi kiểu ghi hình. Chạy `fio-seq` lần 2 sau một lượt xoá lớn |
| `p6` | `diskinfo trim fio-seq fio-rec dirty` |
| `all` | `smoke G C4 C8 M16 M8` |

- **N của bước b/c**, theo thứ tự ưu tiên: N ghi trên dòng lệnh; ngưỡng của bước a trong cùng lần chạy;
  ngưỡng trong tarball bước a mới nhất ở `/data/cap`; cuối cùng là 300. Từ ngưỡng ra N theo mục 2.5:
  ≥ 300 thì 300, nếu không thì làm tròn xuống bội của 50. N ≠ 300 được gắn vào tag (ví dụ
  `C4_ssd_c4_m32_view200`).
- **H**: bản ghi nằm trên `--hdd-dir` (mặc định `/mnt/hdd/cap`, mount vào `/hdd`, harness chạy với
  `--work /hdd/ccu_work`). Tarball vẫn ghi ra `/data/cap`.
- `dirty`, `diskinfo`, `trim`, `fio-*` cần root. `fio-*` cần cài `fio` trên host.
- Trước mỗi kịch bản docker, script ghi `iostat -x 5 3` lúc máy rảnh. Log từng kịch bản nằm trong
  `matrix_logs_<thời gian>/`. Output P6 được đóng thành `p6_<host>_<thời gian>.tar.gz`. Bảng kết quả lưu
  ở `matrix_<thời gian>.txt`. Ctrl+C dừng container đang chạy và vẫn in danh sách.

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
| Đợt đo 2 (2026-10-19): binary / bundle | Binary Release (`debug_build=0`), git `3388afc` nhánh `feature/anhnq48-health-monitor` (commit 2026-10-19 08:25, build 14:43), sha256 `0dea37d6…2a1054`; bundle `3388afcc` đóng gói 2026-10-19 16:29, cùng commit với binary. sysctl vẫn mặc định (`vm.dirty_ratio=20`, `vm.dirty_background_ratio=10`, `net.core.somaxconn=128`), tức chưa làm P6/K5 |
| Bitrate camera thật dự kiến (main / sub, codec) | |

### Bảng 1 — Tiến độ các lượt

Theo cách chia lõi của db3 (vCPU N và N+16 cùng lõi vật lý, NUMA node0 = `0-7,16-23`); xem mục 2.5.

| Lượt | `--ms-cpus` | `--gen-cpus` | RAM | Ổ | a `cam_step` (tag) | b `live_step` (tag, N camera) | c `live_replay` (tag) | Ghi chú |
|---|---|---|---|---|---|---|---|---|
| G | 0-7,16-23 | 8-15,24-31 | 32 GB | SSD | `G_ssd_c16_m32` (2026-10-16 14:47): **chạy với `0-15`/`16-31`, giữ 60 s mỗi bậc**, cần chạy lại (mục 10.1) | `G_ssd_c16_m32_view`, N = 300 (2026-10-16 15:11, cũng chạy `0-15`/`16-31`): ≥ 2 000 đạt (mục 10.2) | `G_ssd_c16_m32_replay` (2026-10-16 16:12, `0-15`/`16-31`): **trượt**, replay 64/80 (mục 10.3) | 8 lõi vật lý / 16 luồng, node0; smoke 20/50 cam đạt (mục 4.1). **Đợt 2 (2026-10-19, binary `3388afc`)**: smoke đạt; a chạy đúng ghim lõi và giữ 600 s, ngưỡng 700, 800 sập vì đĩa (mục 10.6); b `G_ssd_c16_m32_view` (2026-10-20 08:11, N = 300): ≥ 2 000 đạt (mục 10.7); c `G_ssd_c16_m32_replay` (2026-10-20 08:57): **đạt**, replay 80/80 (mục 10.8). **G-a lần 3** `G_ssd_c16_m32_r2` (2026-10-20 09:24, sau G-c): ngưỡng 600 theo bundle, bền vững 500 vì đĩa chậm (mục 10.10). **Đợt 2 là kết quả G-r2** (tag giữ tên cũ, chỉ khác ngày giờ trong tên tarball; điều kiện đúng P7) |
| C4 | 0-1,16-17 | 8-15,24-31 | 32 GB | SSD | `C4_ssd_c4_m32` (2026-10-20 11:57, binary `3388afc`): ngưỡng **200**, 300/400 trượt do `info.txt` ghi trên event poller (mục 10.11) | `C4_ssd_c4_m32_view` (N = 300): trượt ngay ở 0 viewer. `C4_ssd_c4_m32_view200` (N = 200): ≥ 2 000 đạt, bậc 200 viewer trượt vì khựng (mục 10.12) | — | 2 lõi / 4 luồng, `-t 4` |
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
| G đợt 2 (mục 10.6) | 700 theo bundle; **600** nếu giữ util SSD ≤ 70 % khi đĩa còn nhanh, **~450–500** khi đĩa ở trạng thái chậm của G-b đợt 2 (mục 10.7) | 800 (bundle ghi "không hợp lệ", thực chất trượt vì đĩa) | ghi cần ~410 MB/s > trần SSD ~350 MB/s → event poller kẹt chờ ghi, 11 849 timeout kéo, 11 550 khoảng hở | 800 (gán nhầm, xem mục 10.6) | 34,4 | 31,2 | 10 551 | 358,0 | – / wa 2,4 | 164,0 |
| G lần 3, `G_ssd_c16_m32_r2` (mục 10.10) | 600 theo bundle; **500** nếu tính hàng đợi `rec close` (ở 600 tăng liên tục) | 700 | đĩa chỉ ghi ~303 MB/s (cần ~358): 10 507 timeout kéo, 10 179 khoảng hở, healthcheck p95 1 406 ms | 800 (gán nhầm như lần 2) | 30,9 (bậc 500) | 30,5 | 7 145 | 255,8 | – / wa 2,6 | 13,2 |
| C4 (mục 10.11) | 200 | 300 | 352 timeout kéo + 364 khoảng hở + healthcheck p95 239 ms: event poller kẹt ~6 s khi ghi `info.txt` cho 150 luồng | – | 41,6 | 35,2 | 2 230 | 103,9 | – / wa 0,4 | 30,5 |
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
| G đợt 2 (mục 10.7) | 300 | ≥ 2 000 (mọi bậc đạt) | chưa chạm (trên db3: NIC TX ~1 100 viewer) | – | 27,1 | 24,5 | 4 706 | 4 203,5 | 86 (hls-main, bậc 400) | 0,002 | OK |
| C4 (mục 10.12) | 200 (300: nền trượt) | ≥ 2 000 (bậc 200 viewer trượt lẻ) | chưa chạm theo CPU; khựng theo nhịp ghi `info.txt` (L8) | 200 viewer: 26/200 phiên khựng > 2 s | 57,8 | 49,0 | 2 328 | 3 701,0 | 334 (fmp4-sub, bậc 2 000) | 0,121 | OK |
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

**Lượt: G đợt 2 (tag `G_ssd_c16_m32`, 2026-10-19 16:57, binary `3388afc`, MS `0-7,16-23`, bộ sinh tải `8-15,24-31`, chờ 180 s + giữ 600 s)**

Thêm ba cột từ log và iostat của `dm-3` trong cửa sổ đo: util trung bình, `w_await` trung bình, và
thời gian chờ lâu nhất của việc đóng segment trong hàng đợi `rec close` (`oldest_queued_ms`).

| camera | luồng online | CPU % (lõi MS) | poller max % | RSS MB | ghi đĩa MB/s | util / `w_await` | NIC Mbps | healthcheck p95 ms | timeout ngoài cửa sổ | chờ `rec close` max | kết luận |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 100 | 200 | 10,0 | 9,1 | 1 482 | 53,4 | 6 % / 10 ms | 415,0 | 2,3 | 0 | 0,06 s | đạt |
| 200 | 400 | 16,4 | 15,7 | 2 776 | 104,2 | 12 % / 8 ms | 829,8 | 2,9 | 0 | 2,4 s | đạt |
| 300 | 600 | 20,3 | 20,1 | 4 350 | 155,1 | 17 % / 12 ms | 1 244,5 | 4,1 | 0 | 1,5 s | đạt |
| 400 | 800 | 23,3 | 20,1 | 6 146 | 206,3 | 23 % / 16 ms | 1 659,4 | 4,2 | 0 | 2,2 s | đạt |
| 500 | 1000 | 26,0 | 23,2 | 7 247 | 257,3 | 31 % / 23 ms | 2 076,2 | 5,2 | 0 | 2,3 s | đạt |
| 600 | 1200 | 29,8 | 22,7 | 7 739 | 307,9 | 51 % / 35 ms | 2 490,9 | 6,0 | 0 | 3,8 s | đạt |
| 700 | 1400 | 34,4 | 31,2 | 10 551 | 358,0 | 81 % / 50 ms | 2 906,4 | 164,0 | 0 | 27,2 s | đạt (sát ngưỡng) |
| 800 | 1534 | 31,9 | 60,7 | 12 665 | 361,5 | 100 % / 91 ms | 3 027,1 | 3 056,8 | 2 395 | 675 s | bundle: không hợp lệ; thực chất trượt vì đĩa |

### Bảng 5 — Xem lại 20% (bước c, tuỳ chọn)

| Lượt | Số camera nền | Người xem (live + replay) | Phiên replay đạt / tổng | TTFF replay max ms | Bậc live + nền | Ghi hình | Kết luận |
|---|---|---|---|---|---|---|---|
| G (sơ bộ, mục 10.3) | 300 | 320 live + 80 replay (bậc 400) | 64/80 (80 %, cần ≥ 99 %) | 8 568 | đạt (live 320/320) | OK | **trượt**: 16 phiên khựng 2–7,8 s do đóng segment chặn work poller |
| G đợt 2 (mục 10.8) | 300 | 320 live + 80 replay (bậc 400) | **80/80** (100 %) | 1 060 | đạt (live 320/320) | OK | **đạt**: khựng dài nhất 0,58 s, replay chạy trên `replay poller` |
| H | | 400 + 100 | | | | | |

### Bảng 6 — Quan sát thêm

| Lượt | Quan sát (crash, cảnh báo lúc khởi động, OOM, ổ đầy, `database is locked`, lượt chạy lại…) |
|---|---|
| smoke (2026-10-16) | Lần 1 MediaServer abort lúc khởi động (`no such column: currentSessionId`) do binary `e331cbe` lệch migration với bundle `b74ea144`; chạy lại với bundle `3aca28e8` cùng nhánh thì đạt (mục 4.1). Máy có HT, `--ms-cpus 0-15` làm MS và bộ sinh tải dùng chung lõi vật lý, đổi sang `0-7,16-23` / `8-15,24-31` |
| G-a sơ bộ (2026-10-16) | Không crash, không `database is locked`. Ở bậc 500/600/800, `loadServerConfigJson` (nạp cấu hình mỗi 10 s) chặn một event poller 3–7 s, toàn bộ luồng kéo camera trên poller đó `receive rtp timeout` rồi kết nối lại, mất ~10–25 s ghi hình; bundle không bắt vì xảy ra trước cửa sổ đo. Lượt chạy nhầm cách ghim lõi cũ và giữ bậc 60 s (mục 10.1) |
| G-b sơ bộ (2026-10-16) | Không crash, không lỗi join, không rớt luồng (nạp cấu hình ở 300 camera ≤ 1,1 s). **SSD chậm dần khi ghi liên tục**: cùng ~148 MB/s, sau ~25 phút util trung bình tăng 21 % → 48 %, đỉnh thông lượng tụt 510 → 330 MB/s, `w_await` 32 → 81 ms (đỉnh 120 ms); `DiskIoMonitor` cảnh báo await ≥ 50 ms 4 lần. Healthcheck có mẫu lẻ 0,9–2 s ở bậc 750/1250/1500 (p95 vẫn ≤ 3,2 ms) trùng lúc đĩa bão hoà. Vẫn chạy `0-15`/`16-31` |
| G-c sơ bộ (2026-10-16) | Live sạch, replay trượt 16/80. Mỗi phút 600 segment đóng dồn trong ~10 s; binary `e331cbe` chạy việc đóng segment + listener `kBroadcastRecordMP4` (timefile, SQLite tier, archive size) trên **work poller**, cùng chỗ với `MP4Reader` của replay → work poller bị chiếm 2–8,5 s, mọi replay trên poller đó khựng cùng lúc. Binary thiếu commit `489f98e3` của develop (pool "rec close"). `DiskIoMonitor` cảnh báo await 7 lần; đọc đĩa không chậm (`r_await` TB 4,9 ms) |
| smoke đợt 2 (2026-10-19) | Đạt, không crash. Binary/bundle cùng commit `3388afc`. Config chạy trên `api worker`, đóng segment trên `rec close`. Monitor báo `pollers.delay_max_ms` 128 ms ở bậc 50 khi healthcheck p95 2,2 ms (mục 10.6) |
| G-a đợt 2 (2026-10-19) | 100–700 camera: 0 `receive rtp timeout`, 0 khoảng hở, nạp cấu hình chậm nhất 2,0 s nhưng chạy trên `api worker` nên không rớt luồng (lỗi mục 10.1 đã hết). 800 camera: đĩa bão hoà (util 100 %, `vmstat wa` tới 37 %, `b` tới 19), event poller kẹt chờ ghi → 12 634 `receive rtp timeout`, 21 277 lần kéo lại, 11 550 khoảng hở; hàng đợi `rec close` lên 14 706 việc (chờ 675 s), 34 410 fd file, RSS +3,7 GB trong 10 phút; pool `api worker` bận 135 %, chờ tới 749 s, `device.control` từ chối request, webhook lỗi 31 %; 2 lần `database is locked` khi ghi range HOT cho tier. Monitor báo pollers load 95–100 % và delay 0,2–2 s ở mọi bậc, kể cả 100 camera. Bundle gán bậc 800 "không hợp lệ (bộ sinh tải quá tải)" là nhầm (mục 10.6) |
| G-b đợt 2 (2026-10-20) | Không crash, 0 lỗi join, 0 phiên khựng dài, 0 `receive rtp timeout`, 0 khoảng hở, 0 `database is locked`; ghim lõi đúng. **SSD chậm ngay từ đầu**: cùng ~148 MB/s như bậc 300 của G-a đợt 2 nhưng util TB 44 % (so với 17 %), `w_await` TB 50 ms (so với 12 ms), 8–13 mẫu util ≥ 99 % mỗi 10 phút; `DiskIoMonitor` cảnh báo 8 lần; listener đóng segment p99 1,1 s, max 5,0 s. Ở 2 000 viewer, HTTP-fMP4 trôi p95 1,4 s và nhận 99,1 % tốc độ media (HLS vẫn 100 %). Monitor `storage.retention` critical từ bậc 1 250 là do cấu hình test (HOT giữ 180 ngày), không phải lỗi |
| G-c đợt 2 (2026-10-20) | Không crash, 0 `receive rtp timeout`, 0 khoảng hở, 0 `database is locked`. 79 nguồn replay đều chạy trên `replay poller 0–7` (8–12 nguồn mỗi poller), không còn trên work poller. Đóng segment vẫn chậm (listener p99 0,9 s, max 5,5 s, 119 dòng `Slow MP4 close`) nhưng không ảnh hưởng replay. SSD vẫn ở trạng thái chậm như G-b (util 44 %). Dòng tổng của bảng kịch bản trong `SUMMARY.md` ghi nhầm `chua chay / khong co bao cao` dù báo cáo replay có đủ |
| G-a lần 3 (2026-10-20 09:24, tag `G_ssd_c16_m32_r2`) | Cùng binary/bundle/config với lần 2, chạy ngay sau G-c. SSD ở trạng thái chậm: 300 camera util 50 % (lần 2: 17 %), trần ~300–305 MB/s (lần 2: ~350). Bậc 600 bundle chấm đạt nhưng hàng đợi `rec close` tăng đều 1 448 → 5 355 việc, chờ 77 → 267 s, fd file 4 301 → 8 232, RSS +1,9 GB trong 10 phút. 700 trượt, 800 sập như 800 của lần 2 (bundle lại gán "không hợp lệ"). Không crash (mục 10.10) |
| C4-a (2026-10-20 11:57) | Không crash. CPU 52 % của 4 vCPU, poller max 46 % ở 300 camera; đĩa util 50–59 %, không kẹt ghi. Bậc 300: lúc 12:28:21, 352 luồng trên event poller 1/2/3 `receive rtp timeout` cùng lúc; 364 khoảng hở 5–7 s. Bậc 400: 73 khoảng hở 5 s, healthcheck p95 304 ms. Nguyên nhân: `CameraStatisticImp::save()` ghi `info.txt` đồng bộ trên event poller, mỗi lần 60–130 ms khi đĩa bận (mục 10.11) |
| C4-b (2026-10-20 12:57 và 13:38) | N = 300: nền trượt (healthcheck p95 226 ms), 100 viewer có 8 phiên khựng > 2 s, dừng sau 2 bậc. N = 200: ≥ 2 000 viewer, 0 lỗi join, 0 timeout, 0 khoảng hở; bậc 200 viewer có 26/200 phiên khựng > 2 s, trùng phút một poller kẹt 2,9 s ghi `info.txt`. Khựng nhỏ ở bậc 100/600/1 000/2 000 (0,12–0,27 %) cũng trùng các phút kẹt 1,3–2,0 s (mục 10.12) |

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

Chạy lại sau review `734008f3` (2026-10-17): clip sự kiện không rải segment đầu, replay poller
priority LOWEST, giải mã response server_load trên pool api. Cùng tham số trên (light, một node,
segment 20 s, `hook.configReapplySec=30`, 0 viewer, giữ 40 s):

| lượt | luồng | timeout từ khởi động | hở / stale | queue tối đa (mẫu 5 s) | wait max shard (ms) | thread | kết quả |
|---|---|---|---|---|---|---|---|
| 20 camera | 40 | 0 | 0 / 0 | 0 | 67, 112 | config: api worker 6; record: rec close 360 | đạt |
| 50 camera | 100 | 0 | 0 / 0 | 0 | 207, 259 | config: api worker 6; record: rec close 900 | đạt |
| replay 20 camera | 40 | 0 | 0 / 0 | – | – | config: api worker 8; record: rec close 549 | đạt |

21 lần nhận config (`Load server config success`) đều trên `api worker`; chỉ 6 lần apply thật, các lần
còn lại bỏ qua vì không đổi. Replay 16 live + 4 replay, lùi 36–40 s, xem 20 s: **4/4 đạt**, TTFF max
526 ms, không khựng; cả 4 phiên chạy trên `replay poller N`. Binary Debug cùng source `734008f3`, SHA256
`2f3b035fcfff7be0c55c81d27f23016b40fd0f01e8d8617cb96b0208f0e91529`.

**Chưa xác nhận:** throughput trên SSD db3 khi đã hết cache, mức closeThreads tối ưu (vẫn giữ 2),
soak dài, so A/B P3/P4 trên tải bitrate thật và capacity G-r2. Chưa chạy fio, đổi sysctl hoặc chạm db3.

### 10.6 Đợt đo 2: smoke và lượt G-a (2026-10-19)

Tarball `result_smoke_test_2.tar.gz` (`result_db3_smoke_20261019_164431`) và `result_G_a_2.tar.gz`
(`result_db3_G_ssd_c16_m32_20261019_165710`). Binary Release và bundle cùng commit `3388afc` (nhánh
`feature/anhnq48-health-monitor`), dòng `git hash:` trong `stdout.log` của mọi bậc khớp `commit:` trong
`runner.log`. Smoke ở [mục 4.1](#41-kết-quả-chạy-thử-trên-db3-2026-10-16), số liệu từng bậc của G-a ở
[Bảng 4](#bảng-4--chi-tiết-bậc-camera-một-bảng-cho-mỗi-lượt).

**Điều kiện G-a lần này đúng ma trận:** `--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31` (`system.txt`:
`nproc(affinity)=16`), chờ 180 s + giữ 600 s mỗi bậc (cửa sổ chấm 570–582 s), profile full, 0 viewer,
ghi 24/7 cả hai luồng, SSD `/work`, bậc 100 → 800, tổng 111 phút. Đợt này được dùng làm kết quả G-r2 của
P7. Tag giữ tên cũ: tag chỉ đặt tên thư mục/tarball (`ccu_runner.sh`), không đổi kịch bản, và tên
tarball đã có ngày giờ nên không lẫn với đợt 1. Còn lệch so với P7: không chạy baseline A/B (`replayPollerThreads=0;
segmentStartStagger=0`), sysctl đĩa chưa đổi (P6/K5). Config chạy: `fileSecond=60`,
`closeThreads=2`, `segmentStartStagger=1`, `configReapplySec=300`.

#### Kết quả chính

| | Đợt 1 (`e331cbe`, ghim sai, giữ 60 s) | Đợt 2 (`3388afc`, ghim đúng, giữ 600 s) |
|---|---|---|
| Ngưỡng theo bundle | 600 | **700** |
| Bậc sạch cao nhất (kể cả ngoài cửa sổ đo) | 400 | **700** (0 timeout từ lúc khởi động, 0 khoảng hở) |
| Ngưỡng khuyên dùng trên db3/SSD | – | **600** (util SSD 51 %; 700 đã 81 %) |
| Giới hạn | `loadServerConfigJson` chặn event poller | **tốc độ ghi bền vững của SSD/RAID, ~350 MB/s** |
| CPU ở bậc đạt cao nhất | 28 % (600) | 34 % (700) |

**Các sửa của mục 10.4 đã có tác dụng (K1, K2, K4):**

- Nạp cấu hình chạy trên `api worker` ở mọi bậc (6 lần áp dụng thật mỗi bậc, các lần khác bỏ qua vì
  không đổi). Lần chậm nhất 2,0 s (600 camera) nhưng **không còn `receive rtp timeout` nào** từ 100
  tới 700 camera, so với 53–100 luồng rớt mỗi bậc từ 500 camera ở đợt 1.
- Mọi dòng đóng segment chạy trên `rec close` (16 491 lần ở bậc 700), không còn trên work poller.
- Thời điểm đóng segment trải trong ~40 s mỗi phút (giây :00–:41 ở bậc 700) thay vì dồn vào ~10 s.

**Tài nguyên theo số camera (cửa sổ đo 10 phút):**

- **CPU** tăng gần tuyến tính, ~0,045 % của 16 vCPU mỗi camera: 10 % ở 100, 34 % ở 700. Event poller
  ~0,73 % một lõi mỗi camera (511 % ở 700, chia đều 16 poller, bận nhất 33 %). Nhóm `tier` giảm từ
  19 % xuống 9 %, `rec close` 13 % và `api worker` 12 % ở 700.
- **RAM**: RSS ~12,5 MB mỗi camera từ 100 tới 600 (1,5 → 7,7 GB), cao hơn đợt 1 (~11 MB). Trong
  từng bậc 100–700, RSS phẳng hoặc dao động ±5 %, chưa thấy rò rỉ. Từ 600 lên 700 RSS nhảy 2,8 GB vì
  hàng đợi `rec close` giữ segment chưa đóng (fd file 3 609 → 4 442).
- **Đĩa** (`dm-3`): ghi đúng tốc độ cần (~0,51 MB/s mỗi camera) tới 700. Util tăng nhanh hơn tuyến
  tính: 31 % (500), 51 % (600), 81 % (700), 100 % (800). Ở 800 cần ~410 MB/s nhưng đĩa chỉ ghi được
  348–361 MB/s trung bình, đỉnh 435 MB/s. Trần ~350 MB/s khớp với tốc độ bền vững đã thấy ở G-b
  (mục 10.2). Trong một bậc 10 phút, thông lượng đầu và cuối bậc như nhau: đĩa đã ở trạng thái bền
  vững ngay từ đầu vì các bậc chạy nối nhau (đã ghi 36–281 GB mỗi bậc).
- **Mạng** (loopback): 2,9 Gbps ở 700 camera. Trên db3 với `bond0` 2 Gbps, camera thật vẫn chỉ nhận
  được ~385 camera (mục 10.1), thấp hơn ngưỡng đĩa.
- **Bộ nhớ hệ thống**: từ bậc 700, `free` chỉ còn 0,2–0,5 GB (page cache ~20 GB) và có swap-out
  nhỏ (`so` tới 1,3 MB/s, swap dùng tăng ~100 MB). Chưa gây hại, nhưng production nên đặt
  `vm.swappiness` thấp.

#### Vì sao 700 chỉ vừa đạt

Bậc 700 đạt mọi tiêu chí của bundle nhưng có ba dấu hiệu đã sát giới hạn:

1. **SSD util 81 %, `w_await` 50 ms**, `DiskIoMonitor` cảnh báo 3 lần, monitor báo
   `storage/db.write_ms` 629 ms > 500 ms ở 6/57 mẫu.
2. **Hàng đợi `rec close` dồn tới 27 s.** Mỗi phút có 1 400 segment cần đóng (23 việc/s) trên 2
   thread. Đóng segment chậm vì **listener `kBroadcastRecordMP4`**, không phải vì IO file. Theo log
   `MP4 close timing`:

   | camera | listener p50 / p95 / p99 / max ms | IO p99 / max ms | queue max | chờ max |
   |---|---|---|---|---|
   | 100 | 11 / 24 / 76 / 532 | 9 / 26 | 2 | 0,06 s |
   | 400 | 13 / 30 / 334 / 2 074 | 12 / 447 | 50 | 2,2 s |
   | 600 | 11 / 138 / 442 / 1 747 | 16 / 551 | 65 | 3,8 s |
   | 700 | 11 / 220 / 775 / 2 612 | 37 / 976 | 322 | 27,2 s |
   | 800 | 15 / 1 289 / 1 949 / 3 844 | 69 / 866 | 14 706 | 675 s |

   Listener ghi timefile và SQLite (`registerHotSegmentRange`, `addArchiveSize`). File DB nằm cùng
   ổ `/work` với bản ghi, nên mỗi lần commit SQLite phải chờ sau dòng ghi video 350 MB/s. Log chưa
   tách thời gian của từng listener nên chưa biết listener nào chậm nhất.
3. **Healthcheck p95 164 ms** (ngưỡng 200 ms), max 1,2 s, so với ≤ 6 ms ở các bậc dưới.

#### Vì sao 800 sập (bundle gán nhầm "không hợp lệ")

Chuỗi sự kiện của bậc 800 (ramp bắt đầu 18:33:00):

1. 18:33:27–18:33:57: 1 600 luồng lên, ghi ~410–450 MB/s, cao hơn tốc độ đĩa xả được. Page cache bẩn
   tăng tới giới hạn `vm.dirty_ratio=20` (~6 GB).
2. 18:34:17–18:34:38: kernel chặn tiến trình đang ghi (dirty throttling). `vmstat` `b` 13–19, `wa`
   34–37 %. Event poller (nơi ghi gói vào file mp4) bị kẹt trong lệnh ghi nên không đọc socket RTSP.
3. 18:34:27: những dòng `receive rtp timeout` đầu tiên, tiếp theo là vòng lặp kéo lại: 12 634 timeout,
   21 277 `Try playback again`, 11 550 khoảng hở ghi hình, 51 luồng ghi sai lịch. Lần kẹt thứ hai
   lúc 18:35:18–28 (`b` 16–19). Monitor báo poller delay tới 11,8 s.
4. Việc đóng segment không theo kịp: listener p95 1,3 s, 2 thread chỉ đóng được ~5,5 segment/s so
   với 26,7/s cần. Hàng đợi tăng không giới hạn: 14 706 việc, chờ 675 s, 34 410 fd file đang mở, RSS
   tăng tuyến tính 8,3 → 12,7 GB trong 10 phút. Nếu chạy tiếp, fd hoặc RAM sẽ cạn.
5. Kéo theo: pool `api worker` bận 135 %, chờ tới 749 s (chỉ 2 lần nạp cấu hình trong bậc),
   `device.control` từ chối request (`16 requests in flight`), webhook lỗi 31 %,
   `keepalive`/`server usage` timeout, 2 lần `database is locked` khi ghi range HOT. Hai segment đó
   thiếu trong DB tier.

Bundle gán bậc 800 là **"không hợp lệ (bộ sinh tải quá tải)"** vì nguồn RTSP trễ nhịp p99 166 ms
(ngưỡng 100 ms). Kết luận này sai: CPU của nguồn chỉ 46 % (ngưỡng 70 %), và nguồn trễ nhịp **sau khi**
MediaServer đã kẹt và kéo lại hàng chục nghìn lần (mỗi worker nguồn xử lý ~2 700 lượt PLAY/TEARDOWN).
Chính bundle cũng ghi `Yếu tố giới hạn: disk` và `iowait 23,2 %`. Bậc 800 là **trượt vì đĩa**. Lời khuyên
"thêm lõi cho `--gen-cpus`" ở cuối `runner.log` không áp dụng.

#### Giả thuyết RTO loopback (mục 10.1): đã loại

Bundle mới chụp `nstat`/`softnet_stat` trước và sau mỗi bậc. Trong cả 8 bậc, `softnet` dropped = 0,
time_squeeze = 0. `TcpRetransSegs` xấp xỉ `TCPLossProbes` (ví dụ bậc 700: 33 413 so với 33 493 trong
cửa sổ): gần như mọi lần truyền lại là tail-loss probe, không phải RTO 200 ms. Healthcheck 230–280 ms ở
đợt 1 không do loopback. Nhiều khả năng do cùng loại kẹt ghi đĩa như trên (đợt 1 mỗi bậc chỉ 60 s
nhưng ghi tới 408 MB/s). `ListenOverflows` 25–270 mỗi bậc, phần lớn trong pha ramp (trong cửa sổ đo chỉ 0–25), (backlog 128 của nguồn
RTSP khi 1 400–1 600 kết nối mở cùng lúc), không ảnh hưởng kết quả.

#### Monitor báo động trên poller ở mọi bậc

`/media/monitor/summary` báo `process/pollers.load_max` 95–100 % và `pollers.delay_max_ms` critical ở
**mọi mẫu của mọi bậc**, kể cả 100 camera. Bundle đánh dấu "báo giả?". Số đo:

| camera | 50 (smoke) | 100 | 200 | 300 | 400 | 500 | 600 | 700 |
|---|---|---|---|---|---|---|---|---|
| delay_max ms (max) | 128 | 218 | 479 | 852 | 1 051 | 1 364 | 1 731 | 1 977 |
| CPU thread poller bận nhất (%) | 3,7 | 9,1 | 15,7 | 20,1 | 20,1 | 23,2 | 22,7 | 31,2 |

- `load_max` lấy từ `ThreadLoadCounter` (S3ToolKit): đo thời gian poller **thức** (ngoài
  `epoll_wait`), tính cả lúc bị chặn trong syscall. CPU thật chỉ 9–31 %, nên 95–100 % nghĩa là poller
  hoặc thức mà không dùng CPU (chặn trong `write`/`open` file), hoặc bộ đếm (32 mẫu gần nhất) sai lệch
  khi poller thức rất nhiều lần mỗi giây. Ngưỡng 80/95 % hiện tại không phân biệt được tải thật.
- `delay_max_ms` đo từ lúc gửi `async` tới lúc task chạy trên từng poller: là độ trễ thật. Giá trị
  tăng gần tuyến tính ~2,8 ms mỗi camera và có ở mọi mẫu, nên đây là việc định kỳ (≤ 10 s) chạy trên
  một event poller với chi phí O(số camera), hoặc poller bị chặn khi ghi file. Healthcheck p95 vẫn
  ≤ 6 ms tới 600 camera vì chỉ 1/16 poller bị ảnh hưởng mỗi lần. **Đã tìm ra ở lượt C4 (mục 10.11)**: mỗi
  phút `CameraStatisticImp::save()` ghi `info.txt` của từng luồng ngay trên event poller; khi đĩa bận,
  mỗi lần ghi mất 60–130 ms.

#### Kết luận G-a đợt 2

- **Ngưỡng camera trên db3 với SSD hiện tại: 700 theo bundle, khuyên dùng 600** (util SSD ≤ 70 %,
  hàng đợi đóng segment ≤ 4 s). CPU (34 %) và RAM (33 % của 31 GB) còn dư nhiều. Giới hạn là **tốc
  độ ghi bền vững của SSD/RAID ~350 MB/s**, tương đương ~680 camera ở util 100 % với profile full
  (4,5 Mbps/camera).
- Với camera thật qua `bond0` 2 Gbps, db3 dừng ở ~385 camera vì mạng trước khi chạm đĩa.
- Không còn lỗi phân luồng nào làm rớt luồng dưới ngưỡng đĩa. Khi đĩa quá tải, MediaServer **không
  tự bảo vệ**: hàng đợi đóng segment tăng không giới hạn, event poller bị chặn khi ghi, kéo theo API
  và webhook.

#### Đề xuất tiếp theo

Các việc nâng cấp L1–L8 tách sang [capacity-followup-plan.md](capacity-followup-plan.md). Trong báo cáo này: L1 (thời gian
listener đóng segment), L2 (`closeThreads`), L3 (giới hạn hàng đợi `rec close`), L4 (SQLite khác ổ
ghi hình), L5 (đĩa và sysctl), L6 (chỉ số poller của monitor), L7 (nhãn "không hợp lệ" của
harness), L8 (`info.txt` trên event poller, mục 10.11).

### 10.7 Đợt đo 2: lượt G-b, ngưỡng luồng xem trên 300 camera (2026-10-20)

Tarball `result_G_b_2.tar.gz` (`result_db3_G_ssd_c16_m32_view_20261020_081145`): binary và bundle cùng
commit `3388afc` như G-a đợt 2, ghim lõi đúng (`0-7,16-23` / `8-15,24-31`), 300 camera nền (600 luồng,
ghi 24/7), viewer 0 → 2 000 theo mix `live` (HTTP-fMP4 và HLS, luồng chính và phụ), mỗi bậc đo 120 s
(bậc 500 đo 270 s), tổng 39 phút. sysctl đĩa vẫn mặc định. `perf` không chạy được (image thiếu
`linux-tools`), nên SUMMARY không có mục hàm nóng.

**Kết quả: mọi bậc đạt, CCU ≥ 2 000, chưa chạm trần. Nghiệm thu A1 (bậc 500) đạt, dư 300 %.**

| viewer | CPU % | event poller (% 1 lõi) | poller max % | RSS MB | TX loopback Mbps | TTFF p95 lớn nhất ms | khựng % | trôi p95 fMP4 / HLS s | healthcheck p95 / max ms | chờ `rec close` max s |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 19,0 | 276 | 18,1 | 4 313 | 1 244 | – | – | – | 3,4 / 1 525 | 3,5 |
| 500 | 19,9 | 292 | 22,0 | 4 429 | 2 000 | 58 (hls-main) | 0,001 | 0,16 / 0 | 3,8 / 48 | 6,0 |
| 1 000 | 22,4 | 332 | 19,6 | 4 475 | 2 709 | 47 (hls-main) | 0 | 0,03 / 0 | 4,9 / 44 | 4,4 |
| 1 500 | 24,7 | 369 | 21,3 | 4 670 | 3 475 | 19 (hls-main) | 0,002 | 0,03 / 0 | 7,5 / 602 | 6,7 |
| 2 000 | 27,1 | 409 | 24,5 | 4 706 | 4 204 | 43 (hls-main) | 0,002 | **1,40** / 0,15 | 11,2 / 1 409 | 4,5 |

**So với G-b đợt 1 (mục 10.2):**

| | Đợt 1 (`e331cbe`, ghim `0-15`/`16-31`) | Đợt 2 (`3388afc`, ghim đúng) |
|---|---|---|
| CPU ở 0 → 2 000 viewer | 19,6 → 26,9 % | 19,0 → 27,1 % |
| RSS ở 0 → 2 000 viewer | 3 775 → 4 259 MB | 4 313 → 4 706 MB |
| TTFF max | 159 ms | 86 ms (hls-main, bậc 400) |
| Khựng tối đa | 0,079 % | 0,09 % (bậc 100), còn lại ≤ 0,036 % |
| Trôi p95 lớn nhất | 0,54 s | 1,40 s (fMP4, bậc 2 000) |
| `receive rtp timeout` / khoảng hở ghi hình | 0 / 0 | 0 / 0 |

- **Phía người xem sạch**: 2 000 lượt mở luồng, 0 lỗi, 0 phiên đứt bất thường, 0 phiên khựng dài.
  1 370 dòng `HttpSession onError … end of file` đều ở 08:50, là lúc harness đóng viewer khi kết
  thúc.
- **Chi phí mỗi viewer không đổi so với đợt 1**: CPU ~0,004 % của 16 vCPU (~0,065 % một lõi), RSS
  ~0,2 MB. RSS nền cao hơn đợt 1 ~540 MB ở 300 camera, khớp với mức ~12,5 MB/camera của G-a đợt 2
  (đợt 1 ~11 MB).
- **Bộ sinh tải hợp lệ**: 8 worker viewer CPU tối đa 60,5 %, loop lag ≤ 11,5 ms; nguồn RTSP CPU
  ≤ 7,7 %, trễ nhịp p99 ≤ 3,3 ms.
- **Ghi hình không bị viewer ảnh hưởng**: 600 luồng ghi ~155 MB/s ở mọi bậc, 0 khoảng hở, nạp cấu hình
  chạy trên `api worker`, đóng segment trên `rec close` (22 785 lần).

**Dấu hiệu đầu tiên ở 2 000 viewer: HTTP-fMP4 bắt đầu tụt sau.** Trôi p95 của fMP4 (luồng chính và
phụ) tăng từ ≤ 0,18 s lên 1,4 s, tốc độ media nhận được còn 99,1 %. HLS không bị (trôi 0,15 s, 100 %).
Cùng bậc, `TCPLossProbes` lên 9 863 và `TcpRetransSegs` 4 494 (bậc 1 500: 1 828 / 960), còn
`softnet` vẫn không drop. Phía client chưa quá tải (CPU 60 %, loop lag 11,5 ms), poller bận nhất của
MediaServer 24,5 %. Bậc vẫn đạt. Trần của fMP4 trên loopback nhiều khả năng nằm trong khoảng
2 000–3 000 viewer. Muốn biết chính xác cần thêm bậc 2 500/3 000, nhưng trên db3 điều này không có ý
nghĩa thực tế vì NIC chặn trước (xem dưới).

**SSD chậm hơn rõ so với hôm trước, ngay từ đầu lượt.** Cùng tốc độ ghi, `dm-3` = `sda`, không có tải
ghi nào khác trên `sda`/`sdb`:

| | G-a đợt 2, bậc 300 (2026-10-19 17:27) | G-b đợt 2 (2026-10-20 08:15–08:50) |
|---|---|---|
| Ghi TB | 149 MB/s | 148 MB/s |
| util TB | 17 % | **44 %** (39–46 % theo từng 10 phút, không tăng dần) |
| `w_await` TB | 12 ms | **50 ms** (max 114 ms) |
| Mẫu util ≥ 99 % | 0 | 8–13 trong mỗi 10 phút |
| Listener đóng segment p99 / max | 322 / 1 728 ms | 1 066 / 4 972 ms |
| Chờ `rec close` max | 1,5 s | 6,7 s |

Đĩa đã chậm từ phút đầu và giữ nguyên trong 35 phút, không chậm dần như đợt 1 (mục 10.2). Trước lượt
này, G-a đợt 2 đã ghi rồi xoá ~1,6 TB trên cùng ổ. Giả thuyết là SSD sau RAID SMC3108 không nhận
TRIM, nên sau khi xoá nhiều dữ liệu, SSD phải vừa ghi vừa dọn block (garbage collection). Cũng có thể
do trạng thái cache ghi của RAID. Tarball không đủ để phân biệt hai khả năng. Hệ quả:

- Trần ghi ~350 MB/s của G-a đợt 2 (mục 10.6) đo khi đĩa còn "sạch" là **mức lạc quan**. Ở trạng thái
  này, 148 MB/s đã dùng 44 % util, tức trần chỉ còn khoảng **330 MB/s (~640 camera ở util 100 %, ~450
  camera ở util 70 %)**, khớp với 300–330 MB/s của G-b đợt 1. Ngưỡng 600 camera khuyên ở mục 10.6
  nên hạ xuống **~450–500 camera** cho tới khi làm rõ P6/K5 (`fio` sau khi xoá nhiều, `fstrim`,
  cache/BBU của RAID).
- Kể cả ở 300 camera, listener đóng segment có lần tới 5 s. Việc tách SQLite khỏi ổ ghi hình (L4) và
  gộp transaction (L1) càng cần.

**Monitor:** `pollers.delay_max_ms` 590–710 ms và `pollers.load_max` 98–99 % ở mọi bậc, gần như không
đổi theo số viewer. Điều này khớp với nhận định ở mục 10.6: độ trễ poller tỉ lệ theo số camera (300
camera ở G-a đợt 2 là 852 ms), không theo luồng xem. `storage/db.write_ms` warning 530–1 531 ms ở 4
bậc và `disk_io.mount:/work` warning ở 6 bậc là đúng với tình trạng đĩa ở trên.
`storage.retention` critical từ bậc 1 250 (`0.05 day(s) … short of the 180.00 day(s)`) là do cấu
hình test đặt HOT giữ 180 ngày trên SSD 736 GB, không phải lỗi.

**Mạng trên db3 (tính, không đo):** mix `live` trung bình ~1,44 Mbps mỗi viewer (2 884 Mbps client /
2 000). Với `bond0` 2 Gbps dùng an toàn 80 %, TX chỉ đủ **~1 100 viewer**, như đợt 1.

**Kết luận G-b đợt 2:** trên 300 camera, MediaServer phục vụ ≥ 2 000 viewer với CPU 27 %, RSS 4,7 GB,
không lỗi, ghi hình nguyên vẹn. Kết quả lặp lại đợt 1 dưới điều kiện ghim lõi đúng. Giới hạn luồng xem
thực tế của db3 là NIC (~1 100 viewer). Ở 2 000 viewer, HTTP-fMP4 bắt đầu tụt sau. Rủi ro chính vẫn
là đĩa: SSD có thể chậm hơn ~2,5 lần ở cùng tải tuỳ trạng thái, nên ngưỡng camera phải tính theo
trạng thái chậm.

**Việc tiếp theo:** G-c đợt 2 (đã chạy, mục 10.8). Việc kiểm đĩa (L5) và thêm `perf` vào image
(L11) ở [capacity-followup-plan.md](capacity-followup-plan.md).

### 10.8 Đợt đo 2: lượt G-c, 20 % xem lại (2026-10-20)

Tarball `result_G_c_2.tar.gz` (`result_db3_G_ssd_c16_m32_replay_20261020_085723`): cùng binary/bundle
`3388afc`, ghim lõi đúng, 300 camera nền, chờ ~180 s ramp rồi tích bản ghi, sau đó 400 viewer gồm
320 live + 80 replay (HTTP-fMP4 VOD luồng chính, lùi 300–900 s, mỗi phiên xem 300 s, phân bố `hot`:
80 phiên trên 38 camera, `cam-0294` có 14 phiên). Replay chạy khoảng 09:14:07 → 09:19:27. Tổng 23
phút.

**Kết quả: ĐẠT.** Replay 80/80 phiên đạt (100 %, cần ≥ 99 %), live 320/320, ghi hình 600 luồng không
hở.

| | Đợt 1 (`e331cbe`, mục 10.3) | Đợt 2 (`3388afc`) |
|---|---|---|
| Phiên replay đạt | 64/80 (80 %) | **80/80 (100 %)** |
| TTFF replay p50 / p95 / max | – / 1,3–6,4 s / 8 568 ms | 539 / 757 / 1 060 ms |
| Khựng dài nhất của một phiên replay | 7,8 s | **0,58 s** (không phiên nào > 1 s) |
| Tổng thời gian khựng mỗi phiên (300 s) | tới ~8 s | p50 0,51 s, max 0,69 s |
| Thread chạy `MP4Reader` | work poller (chung với đóng segment) | `replay poller 0–7` |
| Thread đóng segment | work poller | `rec close` |
| Live: TTFF max / phiên khựng > 2 s | 75 ms / 0 | 63 ms / 0 |
| CPU / poller max | 20 % / 17,6 % | 20,5 % / 19,4 % (`replay poller` tổng 19 % một lõi) |
| Đọc đĩa trong lúc replay | ~22 MB/s, `r_await` TB 4,9 ms | ~6 MB/s, `r_await` TB 1,6 ms, max 43 ms |

**Sửa của đợt 1 đã giải quyết lỗi:**

- Cả 79 nguồn replay (80 phiên, hai phiên dùng chung một nguồn VOD nên phiên sau có TTFF 1,7 ms) đều
  chuyển sang `replay poller N`, chia đều 8–12 nguồn mỗi poller. Log không còn `MP4Reader` nào trên
  work poller.
- Đóng segment vẫn chậm, thậm chí chậm hơn đợt 1 vì SSD đang ở trạng thái chậm (mục 10.7): listener
  `kBroadcastRecordMP4` p50 12 ms, p95 173 ms, p99 924 ms, max 5,5 s; 119 dòng `Slow MP4 close`, có
  đều trong suốt thời gian replay; hàng đợi `rec close` tối đa 50 việc, chờ 5,5 s. Nhưng việc này giờ
  chạy trên `rec close`, nên không còn chặn replay. Ở đợt 1, cùng mức chậm này đã làm 16 phiên khựng
  2–8 s.
- Mọi phiên replay có 1–4 lần khựng ngắn 0,1–0,58 s, tổng ~0,5 s mỗi 300 s (~0,2 %). Mức này dưới xa
  ngưỡng 2 s. Live cũng có khựng ngắn tương tự (175 lần trên 320 phiên, dài nhất 0,78 s). Harness chỉ
  ghi thời điểm cho lần khựng > 1 s, nên `stall_events` rỗng và chưa đối chiếu được với lúc đóng
  segment.

**Đĩa:** ghi ~148 MB/s với util TB 44 %, `w_await` TB 48 ms (max 115 ms), 16 mẫu util ≥ 99 % trong
20 phút. Đây vẫn là trạng thái chậm đã thấy ở G-b đợt 2. Đọc chỉ ~6 MB/s vì bản ghi được xem mới
5–15 phút tuổi và tập trung vào 38 camera, phần lớn còn trong page cache. **Lượt này chưa kiểm replay
đọc thật từ đĩa** (bản ghi cũ, page cache trống) khi SSD đang bận ghi.

**Monitor:** `pollers.delay_max_ms` 620–825 ms và `pollers.load_max` 99 % ở mọi mẫu, giống G-a/G-b
đợt 2 (mục 10.6). Lần này monitor không báo đĩa hay `db.write_ms`.

**Harness:** dòng `live_replay` trong bảng kịch bản đầu `SUMMARY.md` ghi `chua chay / khong co bao
cao`, dù mục chi tiết bên dưới có đủ báo cáo replay và `rc=0` (L10 trong [capacity-followup-plan.md](capacity-followup-plan.md)).

**Kết luận G-c đợt 2:** với 300 camera ghi 24/7, 320 live + 80 replay (20 %) chạy sạch: replay 100 %
đạt, TTFF ≤ 1,1 s, không phiên nào khựng quá 0,6 s. Lỗi phân luồng của đợt 1 (K1, K3) đã sửa xong và có
hiệu lực trên tải bitrate thật.

### 10.9 Tổng hợp lượt G đợt 2 (binary `3388afc`, db3, SSD)

| Bước | Đợt 1 | Đợt 2 | Giới hạn thật | Còn dư |
|---|---|---|---|---|
| a — ngưỡng camera | bundle 600, sạch 400 | **bundle 700**, khuyên 600 khi đĩa nhanh, **~450–500 khi đĩa chậm** | tốc độ ghi bền vững của SSD/RAID (330–350 MB/s); trên 800, đĩa quá tải làm event poller kẹt và hàng đợi `rec close` tăng không giới hạn | CPU 34 %, RAM 33 % ở 700 camera |
| a — lần 3, đĩa chậm (mục 10.10) | – | bundle 600, **bền vững 500** | như trên, trần đĩa còn ~300 MB/s | CPU 31 %, RAM 23 % ở 500 camera |
| b — luồng xem (300 camera) | ≥ 2 000 | **≥ 2 000** | không có trong MediaServer; HTTP-fMP4 bắt đầu tụt ở 2 000; trên db3 NIC 2 Gbps chặn ~1 100 viewer | CPU 27 % ở 2 000 viewer |
| c — 20 % replay (300 camera) | trượt, 64/80 | **đạt, 80/80** | chưa chạm; chưa thử đọc lạnh từ đĩa | CPU 20,5 % |

Ba lỗi phân luồng của đợt 1 đều đã hết: nạp cấu hình chạy trên `api worker`, đóng segment trên `rec
close`, replay trên `replay poller`. Giới hạn còn lại đều nằm ở **đĩa**: tốc độ ghi bền vững, mức chậm
của SSD tuỳ trạng thái (util 17 % so với 44 % ở cùng tải), và việc listener đóng segment ghi SQLite trên
cùng ổ (p99 ~1 s, max 5,5 s). Với camera thật, NIC 2 Gbps của db3 chặn trước ở ~385 camera và ~1 100
viewer.

Quy trình chạy baseline A/B, P6 và `closeThreads` trên db3 ở [mục 5.6](#56-sau-lượt-g-baseline-ab-và-kiểm-đĩa-p6).

Việc còn lại và thứ tự làm ở [capacity-followup-plan.md](capacity-followup-plan.md).

### 10.10 Lượt G-a lần 3: chạy lại khi SSD đã chậm (2026-10-20)

Tarball `result_G_a_3.tar.gz` (`result_db3_G_ssd_c16_m32_r2_20261020_092402`, tag `G_ssd_c16_m32_r2`).
Binary, bundle (`3388afc`), ghim lõi, thời gian giữ bậc, config (`closeThreads=2`,
`replayPollerThreads=8`, `segmentStartStagger=1`) và sysctl đều **giống hệt lần 2** (mục 10.6).
Khác duy nhất là thời điểm: lần 3 chạy lúc 09:24, ngay sau G-b (08:11) và G-c (08:57), khi SSD đang ở
trạng thái chậm của mục 10.7. Lần 2 chạy lúc 16:57 hôm trước.

**Kết quả: ngưỡng theo bundle 600 (lần 2: 700). Bền vững chỉ 500.**

| camera | ghi MB/s | util / `w_await` | mẫu util ≥ 99 % | chờ `rec close` max | listener p99 ms | RSS MB (Δ trong cửa sổ) | healthcheck p95 / max ms | kết luận bundle |
|---|---|---|---|---|---|---|---|---|
| 100 | 51 | 17 % / 36 ms | 0/120 | 0,5 s | – | 1 472 (+86) | 2,3 / 298 | đạt |
| 200 | 100 | 34 % / 58 ms | 0/120 | 2,3 s | – | 2 939 (+49) | 3,1 / 575 | đạt |
| 300 | 150 | 50 % / 62 ms | 14/121 | 5,7 s | – | 4 346 (+171) | 3,1 / 895 | đạt |
| 400 | 196 | 64 % / 70 ms | 16/121 | 8,2 s | – | 5 593 (−48) | 3,8 / 1 485 | đạt |
| 500 | 246 | 81 % / 77 ms | 8/121 | 20,6 s | 1 162 | 7 145 (+586) | 13,2 / 1 238 | đạt |
| 600 | 293 | 95 % / 92 ms | 53/121 | **274 s, tăng liên tục** | 1 558 | 10 089 (+1 948) | 20,1 / 2 806 | đạt |
| 700 | 303 | 100 % / 106 ms | 124/128 | 624 s | 2 004 | 11 175 (+3 455) | 1 406 / 7 287 | **trượt**: 10 507 timeout, 10 179 khoảng hở, 90 luồng ghi sai lịch |
| 800 | 302 | 100 % / 103 ms | 124/129 | 664 s | – | 13 837 (+4 672) | 5 000 / 7 445 | "không hợp lệ" (gán nhầm, như lần 2) |

Cột util / `w_await` lấy từ `iostat` của `dm-3` trong cửa sổ đo. Listener p99 lấy từ log
`MP4 close timing` (chỉ tính cho 500–700).

**So với lần 2, đĩa chậm hơn ở mọi bậc:**

| | Lần 2 (2026-10-19) | Lần 3 (2026-10-20) |
|---|---|---|
| util ở 300 camera (~150 MB/s) | 17 % | 50 % |
| util ở 600 camera (~300 MB/s) | 51 % | 95 % |
| Ghi được tối đa (util 100 %) | ~350 MB/s (bậc 800) | **~300–305 MB/s** (bậc 700/800) |
| Ngưỡng theo bundle / bền vững | 700 / 600 | 600 / **500** |

CPU (≤ 31 %), event poller và RAM theo camera vẫn như lần 2. Trần ghi ~300 MB/s thấp hơn cả ước tính
~330 MB/s ở mục 10.7, nên mức chậm của SSD trên db3 còn thay đổi theo thời điểm.

**Bậc 600 "đạt" nhưng không bền.** Trong 10 phút giữ bậc, hàng đợi `rec close` tăng gần tuyến tính
(tổng hai shard theo từng phút: 1 448, 1 801, 2 276, 2 950, 3 352, 3 675, 4 073, 4 439, 4 838, 5 355),
thời gian chờ lâu nhất 77 → 267 s, số fd `file` 4 301 → 8 232, RSS +1,9 GB. Bản ghi chưa hở và chưa có
timeout nên bundle chấm đạt. Nhưng nếu giữ lâu hơn, fd và RAM sẽ cạn như ở bậc 800 của lần 2. Theo
tiêu chí P7 ("backlog `rec close` về 0 sau mỗi phút"), bậc này trượt. Ở bậc 500, hàng đợi dao động
45–400 việc rồi rút về, chờ ≤ 20,6 s: vẫn bền nhưng đã sát giới hạn (util 81 %).

**Bậc 700 sập theo cùng chuỗi với bậc 800 của lần 2** (mục 10.6): đĩa ghi không kịp, page cache bẩn đầy,
kernel chặn tiến trình ghi (`vmstat` `b` 19, `wa` 40 %), event poller kẹt, có `receive rtp timeout` từ
10:48:28 (còn trong pha ramp), kéo lại và hở hàng loạt. Monitor báo poller delay 11,4 s (bậc 700) và
19,6 s (bậc 800).

**Kết luận:**

- Trên db3 hiện tại, ngưỡng camera phụ thuộc trạng thái SSD nhiều hơn mọi yếu tố khác: **500–700
  camera** tuỳ lúc đo, với CPU và RAM còn dư. Nên chốt theo trạng thái chậm: **~400 camera** (~210 MB/s,
  tức util ~70 % của trần 300 MB/s, ở ~0,51 MB/s mỗi camera) cho tới khi P6 (mục 5.6.1) đo được trần thật và làm rõ chuyện TRIM/cache RAID.
- Harness đang bỏ sót một kiểu trượt: hàng đợi đóng segment tăng không giới hạn trong khi bản ghi chưa
  hở (L9 trong [capacity-followup-plan.md](capacity-followup-plan.md)).

### 10.11 Lượt C4, bước a — ngưỡng camera với 4 vCPU (2026-10-20)

Tarball `result_C4_a.tar.gz` (`result_db3_C4_ssd_c4_m32_20261020_115740`): binary/bundle `3388afc`,
MediaServer `0-1,16-17` (2 lõi vật lý / 4 luồng HT, `-t 4` nên chỉ có **4 event poller**), bộ sinh tải
`8-15,24-31`, giữ 600 s mỗi bậc, profile full, 0 viewer. Dừng sau 2 bậc trượt (300, 400), tổng 54 phút.
Chạy ngay sau G-a lần 3, SSD vẫn ở trạng thái chậm.

**Kết quả: ngưỡng 200 camera.**

| camera | CPU % (4 vCPU) | event poller (% 1 lõi) | poller max % | RSS MB | ghi MB/s | util / `w_await` | healthcheck p95 / max ms | timeout / khoảng hở | kết luận |
|---|---|---|---|---|---|---|---|---|---|
| 100 | 28,2 | 93 | 24,5 | 1 116 | 53 | 16 % / 34 ms | 4,3 / 921 | 0 / 0 | đạt |
| 200 | 41,6 | 146 | 35,2 | 2 230 | 104 | 30 % / 51 ms | 30,5 / 2 525 | 0 / 0 | đạt |
| 300 | 52,4 | 188 | 46,1 | 3 493 | 154 | 50 % / 60 ms | **239** / 4 286 | **352 / 364** | trượt |
| 400 | 62,4 | 225 | 57,7 | 4 613 | 205 | 59 % / 59 ms | **304** / 5 278 | 0 / **73** | trượt |

**Không phải CPU, cũng không phải đĩa quá tải:**

- CPU ở 300 camera chỉ 52 % của 4 vCPU, poller bận nhất 46 %. Chi phí mỗi camera ~0,007 vCPU, như G.
  Lưu ý: 4 vCPU là 2 lõi vật lý có HT, nên 52 % đã tương đương ~80 % sức thật của 2 lõi. CPU sắp
  thành giới hạn, nhưng chưa phải nguyên nhân trượt ở bậc này.
- Đĩa ghi đủ tốc độ cần, util 50–59 %, `vmstat` `b` ≤ 6, `wa` ≤ 12 %. Không có kẹt ghi kiểu bậc 800
  của G-a (mục 10.6). Hàng đợi `rec close` ≤ 78 việc, chờ ≤ 6,4 s, không tăng dần.
- Nạp cấu hình chạy trên `api worker` (tối đa 1,1 s), không chặn poller.

**Nguyên nhân: ghi `info.txt` đồng bộ trên event poller.** Mỗi phút, `addStreamStatistic` chạy cho từng
luồng **trên event poller của luồng đó** và gọi `save()` → `FileRecorder::save_string()`: mở
`info.txt` với `trunc`, ghi, đóng (`manager/Camera/CameraStatistic.cpp:954`,
`manager/Local/FileRecorder.h:66`). File này nằm trên `/work`, cùng ổ với bản ghi. Đo khoảng cách giữa
dòng log `addStreamStatistic` và `syncResourceStatus` kế tiếp trên cùng thread (chính là thời gian
`save()`):

| Lượt | Luồng / poller | `save()` p50 / p95 / p99 / max ms |
|---|---|---|
| G-a lần 2, 300 camera (16 poller, đĩa nhanh) | ~38 | 1 / 9 / 29 / 59 |
| G-a lần 2, 700 camera | ~88 | 1 / 57 / 72 / 100 |
| C4, 300 camera (4 poller) | 150 | 1 / 68 / 101 / 129 |
| C4, 400 camera | 200 | 1 / 74 / 95 / 127 |

Thường mỗi lần ghi mất ~1 ms. Nhưng có những phút, khi đĩa đang xả page cache, gần như mọi lần ghi
mất 60–130 ms. Mỗi poller ghi liền cho mọi luồng của nó trong một đợt, nên thời gian kẹt cộng dồn
theo số luồng trên poller:

| Phút (C4, 300 camera) | Tổng thời gian `save()` mỗi poller | Hệ quả |
|---|---|---|
| 12:27 | 0,1–0,2 s | không |
| **12:28** | **5,8–6,2 s** liên tục trên cả 4 poller | 12:28:21: 150 + 144 + 58 luồng trên poller 1/2/3 `receive rtp timeout`, kéo lại, 364 khoảng hở 5–7 s; monitor `cameras.offline_pct` 56 % |
| 12:32, 12:35 | 2,5–3,3 s | healthcheck chậm (max 4,3 s) |
| C4 400 camera: 12:41, 12:44 | 0,8–3,8 s | khoảng hở 5 s (không kịp tới timeout), healthcheck p95 304 ms |

Ở G với 16 poller, mỗi poller chỉ gánh ~38–88 luồng nên cùng hiện tượng chỉ gây kẹt 0,4–2 s.
Đây chính là `pollers.delay_max_ms` tăng ~2,8 ms mỗi camera ở mục 10.6. Với 4 poller, kẹt dài gấp 4
lần và vượt ngưỡng timeout media của RTSP. Vì thế **ngưỡng C4 hiện đo được (200) là do lỗi phân luồng,
không phải do 4 vCPU**.

Việc mở file với `trunc` rồi đóng trên XFS còn có thể làm kernel đẩy ghi file đó ngay lúc đóng
(XFS flush khi đóng file vừa bị truncate). Điều này giải thích vì sao từng lần ghi chậm theo nhịp xả
của đĩa. Đây là giả thuyết, chưa đo trực tiếp.

**Đề xuất sửa:** L8 (ưu tiên 1) trong [capacity-followup-plan.md](capacity-followup-plan.md). Sau khi sửa, kỳ vọng C4 bị giới hạn bởi
CPU ở khoảng 300 camera (52 % của 4 vCPU HT ≈ 80 % của 2 lõi thật), và `pollers.delay_max_ms` cao ở G
cũng hết.

**Kết luận C4-a:** với binary `3388afc`, 4 vCPU nhận được **200 camera** (400 luồng, ghi 24/7). Giới hạn
là việc ghi file đồng bộ trên event poller, lộ ra vì chỉ có 4 poller. C4-b nên chạy với `--cameras 200`.
Nếu sửa L8 trước, nên chạy lại C4-a rồi mới chạy C4-b.

### 10.12 Lượt C4, bước b — luồng xem với 4 vCPU (2026-10-20)

Hai tarball, cùng binary/bundle `3388afc`, MediaServer `0-1,16-17` (`-t 4`), mix `live`:

- `result_C4_b_200.tar.gz` (`result_db3_C4_ssd_c4_m32_view200_20261020_125704`): **200 camera nền**
  (ngưỡng của C4-a), viewer 0 → 2 000, 38 phút.
- `result_C4_b.tar.gz` (`result_db3_C4_ssd_c4_m32_view_20261020_133811`): **300 camera nền** như G-b, 8
  phút.

**300 camera nền: trượt ngay ở 0 viewer.** Healthcheck p95 226 ms (CPU 51 %, poller max 42 %). Ở 100
viewer: 8/100 phiên khựng > 2 s, TTFF 176–246 ms. Bundle dừng sau 2 bậc trượt. Kết quả này khớp với
C4-a, vì 300 camera đã vượt ngưỡng của C4 (mục 10.11). Không có ngưỡng luồng xem trên 300 camera cho C4.

**200 camera nền: CCU ≥ 2 000, chưa chạm trần.** Nghiệm thu A1 (bậc 500) đạt. Có một bậc trượt lẻ.

| viewer | CPU % (4 vCPU) | event poller (% 1 lõi) | poller max % | RSS MB | TX loopback Mbps | TTFF max ms | khựng % | phiên khựng > 2 s | healthcheck p95 / max ms | kết luận |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 42,9 | 149 | 38,2 | 2 191 | 831 | – | – | – | 15,0 / 249 | đạt |
| 100 | 43,5 | 155 | 40,3 | 2 221 | 973 | 18 | 0,137 | 0 | 24,5 / 1 776 | đạt |
| 200 | 44,7 | 157 | 37,0 | 2 227 | 1 111 | 110 | 0,805 | **26/200** | 19,1 / 3 633 | **trượt** |
| 500 | 47,3 | 170 | 39,3 | 2 280 | 1 552 | 22 | 0,026 | 0 | 34,1 / 1 389 | đạt |
| 1 000 | 50,8 | 183 | 50,4 | 2 304 | 2 275 | 250 | 0,185 | 0 | 15,3 / 2 099 | đạt |
| 1 500 | 55,2 | 200 | 52,0 | 2 312 | 2 972 | 205 | 0,005 | 0 | 78,9 / 881 | đạt |
| 2 000 | 57,8 | 214 | 49,0 | 2 328 | 3 701 | 334 | 0,121 | 0 | 82,5 / 1 809 | đạt |

- **Theo tài nguyên, C4 còn dư nhiều**: từ 0 lên 2 000 viewer, CPU chỉ tăng 42,9 → 57,8 % của 4
  vCPU (~0,03 % một lõi mỗi viewer), RSS +140 MB. 2 000 lượt mở luồng, 0 lỗi, 0 phiên đứt bất
  thường. Ghi hình 400 luồng không hở, 0 `receive rtp timeout`. Bộ sinh tải hợp lệ (client CPU ≤ 55 %,
  loop lag ≤ 8,6 ms).
- **So với G-b (16 vCPU, 300 camera)**: TTFF max cao hơn (334 so với 86 ms), khựng % cao hơn
  (0,12–0,8 so với ≤ 0,04 %), healthcheck max 0,9–3,6 s ở hầu hết các bậc.

**Bậc 200 viewer trượt vì cùng lỗi của C4-a, không phải vì tải.** Đo thời gian `save()` của
`CameraStatisticImp` từ log như mục 10.11 (mỗi poller 100 luồng). Tổng thời gian một poller kẹt khi ghi
`info.txt` theo từng phút: thường 0,1 s, nhưng có những phút 1,0–2,9 s. Các bậc có khựng đều rơi vào
các phút kẹt lâu:

| Bậc (cửa sổ đo) | Phút kẹt lâu nhất trong cửa sổ | Khựng % / phiên > 2 s |
|---|---|---|
| 100 (13:03–13:05) | 13:03: 1,3 s | 0,137 % / 0 |
| **200 (13:06–13:08)** | **13:06: 2,9 s** (dài nhất cả lượt) | **0,805 % / 26** |
| 400 (13:11–13:13) | 13:12: 0,3 s | 0 % / 0 |
| 600 (13:19–13:21) | 13:19: 2,0 s | 0,266 % / 0 |
| 1 000 (13:24–13:26) | 13:24: 1,4 s | 0,185 % / 0 |
| 1 250 (13:27–13:29) | 13:28: 0,6 s | 0,004 % / 0 |
| 2 000 (13:33–13:35) | 13:33: 1,4 s | 0,121 % / 0 |

Viewer của các luồng nằm trên poller đang kẹt không nhận dữ liệu suốt thời gian kẹt. Kẹt ~3 s thì
vượt ngưỡng khựng 2 s. Bậc nào trượt là ngẫu nhiên, tuỳ phút nào đĩa đang xả page cache. Vì chỉ một
bậc trượt (bundle dừng sau 2 bậc liên tiếp) nên lượt vẫn chạy tới 2 000.

**Kết luận C4-b:** với 4 vCPU và 200 camera, MediaServer phục vụ **≥ 2 000 viewer** theo CPU và RAM,
nhưng chất lượng xem không ổn định: theo nhịp ghi `info.txt` trên event poller, mỗi vài phút một phần
viewer khựng 1–3 s. Trên 300 camera, C4 không đạt ngay cả khi không có viewer. Sau khi sửa L8 trong
[capacity-followup-plan.md](capacity-followup-plan.md), cần chạy lại C4-a và C4-b. Kỳ vọng bậc 200 viewer
hết trượt, và có thể chạy C4-b trên 300 camera nếu C4-a mới đạt 300.
