# Kiểm thử đồng bộ ESC DB với 2 node

Dựng 2 MediaServer (`node-a`, `node-b`) trên `127.0.0.1` cùng một mock service API, rồi kiểm tra `SyncManager` đồng bộ DB ESC giữa hai node.

## Yêu cầu
- `MediaServer` bản **Debug** có `ENABLE_SYNC_CHANGES=ON`. Bản Debug đọc cấu hình camera/cluster từ `./api-config.json` sau khi hook cấu hình trả về thành công.
- Các cổng 18081, 18082 (HTTP), 18551, 18552 (RTSP), 18351, 18352 (RTMP) và 18090 (mock API) phải đang trống.
- Python 3 (chỉ dùng thư viện chuẩn).

## Chạy
```bash
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_sync_cluster
# dừng thủ công nếu script bị ngắt giữa chừng
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_sync_cluster stop
```
`MEDIASERVER_DIR` dùng để chọn thư mục chứa binary, `WAIT_SEC` là thời gian chờ ở mỗi phase.

Kiểm tra các API chạy trên API worker pool (`docs/features/worker.md`, bước 6), chỉ dùng một node:
```bash
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_worker_apis workerapis
```
`check_worker_apis.py` gọi discovery ONVIF **và** discovery RTSP tới một IP bị drop gói (`192.0.2.1`, RFC 5737 TEST-NET-1; ONVIF mất ~20s, ffprobe ~10s) rồi đo độ trễ của `/media/mserver/healthcheck` trong suốt thời gian đó — cả hai dạng address đều phải chạy trên `api worker`, không chiếm poller nào. Ngoài ra kiểm tra `subnetScan` và `register` (lưu certificate, gọi lại không đổi gì). Node test chạy với `auth.enableAuthorize=0`.

Không đổi `BLACKHOLE_IP` sang dải `10.x`: dưới WSL đó là NAT gateway của host và trả "connection refused" ngay, làm mọi check theo thời gian trở nên flaky.

## Kịch bản failover (camera ghi hình thật)
```bash
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_failover failover
```
Cần thêm `ffmpeg`/`ffprobe` và cổng 18554 trống. Script dựng clip H.264 640x360 rồi chạy
`tests/integration/stress_vms/rtsp_source.py` làm camera `cam-fo-1` (ghi hình liên tục, node chính
`priMediaServerId=node-a`). Service API được giả lập bằng cách ghi lại `api-config.json`
(`setup_nodes.py ... fo1|fo2|fo3`); kiểm tra nằm trong `check_failover.py`, mốc thời gian ở
`failover_state.json`.

| Bước | Diễn biến | Kiểm tra |
|---|---|---|
| fo1 `a_live` | Camera chạy và ghi ở node-a | FO-01..04: node-a kéo luồng, ghi hình, có file; nguồn chỉ có 1 server kéo |
| fo2 `b_failover` | Dừng node-a (SIGINT), service giao camera cho node-b | FO-10..19: node-a offline; node-b kéo + ghi; assignment active duy nhất là node-b/FAILOVER; xem trực tiếp HTTP-FMP4 và RTSP từ node-b (FLV không có vì `enable_rtmp=0`); timeline node-b có dữ liệu node-b; replay VOD từ node-b |
| fo3 `a_back` | Khởi động lại node-a, service trả camera về node-a và bỏ khỏi node-b | FO-20..29: node-a kéo + ghi lại; node-b ngắt luồng; nguồn chỉ còn 1 server kéo; assignment active trên cả 2 DB là node-a; timeline gọi vào node-a trả dữ liệu `mediaServerId=node-b` cho khoảng failover; thumbnail qua node-a được forward sang node-b; replay từ node-b vẫn chạy; replay qua node-a chỉ là INFO (URL VOD không forward, player chọn node theo `mediaServerId`) |
| fo4 `retention` | node-b giữ camera ở trạng thái inactive | FO-30..33: file ghi trong lúc failover còn nguyên trước khi hết hạn; `keepArchivedMaxFor` của node-b đúng giá trị; timeline node-a vẫn trả dữ liệu node-b |

`keepArchivedMaxFor` tính theo giờ nguyên (`FO_KEEP_HOURS`, mặc định 1), nên việc chờ dữ liệu hết hạn
chỉ chạy khi đặt `FO_RETENTION_WAIT=1` (FO-35..37: không file nào bị xoá sớm, hết hạn thì node-b xoá
file và timeline node-a không còn trả dữ liệu đó). Hạn chót là file mới nhất + `FO_KEEP_HOURS` +
`FO_RETENTION_GRACE_SEC` (mặc định 5400 s, vì mốc cắt được làm tròn xuống theo
`storage.tieringSplitGranularitySeconds` và retention chạy theo chu kỳ). Kịch bản này chạy với `cluster.failoverActiveDelaySec=1800` (mặc định production) thay vì 5 s
của phase4: node-b bỏ camera trước khi node-a ghi assignment mới (lệch một chu kỳ reload cấu hình),
nếu thời gian chờ ngắn hơn khoảng đó thì node-b gỡ camera, xoá `info.txt` và replay dữ liệu failover
trên node-b trả 403 dù file ghi vẫn còn. `FO_WAIT_SEC` (mặc định 90)
là thời gian chờ mỗi bước. Mã thoát là số check FAIL.

## Kịch bản đồng bộ cấu hình loa
```bash
tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_speaker speaker
```
Loa `spk-sync-1` trỏ tới `127.0.0.1:9` (không kết nối được, chỉ kiểm phần cấu hình). Kiểm tra nằm trong
`check_speaker.py`, mỗi check chờ tối đa `SP_WAIT_SEC` (mặc định 60 s); check "không được xảy ra" theo dõi thêm
`SP_SETTLE_SEC` (mặc định 20 s). Mã thoát là số check FAIL.

| Bước | Diễn biến | Kiểm tra |
|---|---|---|
| sp1 | Loa chạy ở node-a | SP-01..03: `vms_resource` loại `Speaker` (parent `node-a`, url) và kvpair có trên cả 2 DB; không đồng bộ tài khoản ONVIF |
| sp2 | Đổi tên loa | SP-10: tên mới sang cả 2 DB |
| sp3 | Chuyển loa sang node-b | SP-20..21: cả 2 DB trỏ về `node-b` và giữ nguyên. node-a có thể xoá bản ghi vì DB của nó chưa nhận bản ghi node-b vừa ghi; node-b ghi lại ở lần nạp config kế tiếp (tối đa mỗi 30 s) |
| sp4 | Restart cả 2 node | SP-30..31: như sp3 |
| sp5 | node-b restart với `camera.sync_config_to_db=0`, `speaker.sync_config_to_db=0`; thêm camera `cam-sync-off`, loa `spk-sync-off`, đổi tên `spk-sync-1` | SP-40..43: assignment của camera vẫn ghi; không có `vms_resource`/kvpair của camera và loa mới; tên `spk-sync-1` trên DB không đổi |
| sp6 | node-b bật lại đồng bộ, xoá loa khỏi cả 2 node | SP-50: `vms_resource` và kvpair của loa bị xoá trên cả 2 DB |

## Kịch bản bỏ node khỏi cluster
```bash
bash tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_removal removal
```
Mock API trả HTTP 412 (`code 689501`) cho các node có id trong `<work_dir>/not_qualified.txt`. Kịch bản chạy với
`cluster.decommissionAfterSec=15`, `cluster.removedPeerCleanupSec=30` (đổi bằng `DECOMMISSION_AFTER_SEC`,
`REMOVED_PEER_CLEANUP_SEC`). Kiểm tra nằm trong `check_removal.py` (`RM_WAIT_SEC`, mặc định 120). Chính sách:
[docs/api-sync.md](../../../docs/api-sync.md), mục "Node bị bỏ khỏi cluster".

| Bước | Diễn biến | Kiểm tra |
|---|---|---|
| rm1 | node-b chạy `cam-rm-orphan`, `cam-rm-moved`, loa `spk-rm-1` | RM-01..02: node-a nhận đủ, owner là node-b |
| rm2 | Service chuyển `cam-rm-moved` sang node-a, bỏ node-b (node-a không còn liệt kê, node-b nhận 412) | RM-10..12: node-b vào decommissioned, tắt 2 camera + bỏ loa, không ghi log ESC; RM-13..17: node-a nhận camera, sau 30 s dọn camera bị xoá + loa, đóng assignment của node-b, xoá bản ghi `REMOVED_PEER` |
| rm3 | Thêm node-b lại | RM-20..23: node-b thoát decommissioned, đuổi kịp bản dọn; RM-22: không entry nào node-b ghi lúc decommissioned tới node-a |

## Kịch bản đổi thông tin peer
```bash
bash tests/integration/sync_cluster/run_cluster.sh /tmp/s3mk_peerinfo peerinfo
```
Hai node ở server group 5 (`SERVER_GROUP_ID`), để danh sách peer đã lưu mà mất nhóm (đọc lại ra 0) lộ ra
ở pi5. Kiểm tra nằm trong `check_peerinfo.py`: đọc `/media/monitor/summary` của node-a (`cluster.peers`,
`cluster.sync.peers`) và log `node-a/stdout.log`. Mã thoát là số bước lỗi.

| Bước | Diễn biến | Kiểm tra |
|---|---|---|
| pi1 | 2 node chạy, chờ một vòng health check định kỳ (60 s) | node-b reachable, nằm trong peer sync của node-a |
| pi2 | Danh sách server đổi `name` + `hasFailover` của node-b, endpoint giữ nguyên | Tên mới hiện trên summary; trong 20 s node-b luôn reachable và là peer sync; không có `Peer node-b is unreachable`; không có health check node-b nào ngoài lịch 60 s ngay sau khi áp dụng |
| pi3 | Cổng HTTP của node-b đổi sang 18099 (không có gì nghe) | node-a check ngay endpoint mới (thất bại), node-b không còn reachable, rời peer sync |
| pi4 | Trả lại cổng 18082 | node-a check ngay, node-b vào lại peer sync |
| pi5 | Dừng node-a, xoá `api-config.json` của nó (service không trả danh sách server), khởi động lại | `peer.peer_list` lưu `serverGroupId=5`; node-a pull thành công từ node-b sau khi khởi động; không có lần nạp danh sách server nào |

## Kịch bản
| Phase | Nội dung |
|---|---|
| phase1 | Camera `cam-sync-1` chạy trên node-a; node-b nhận dữ liệu qua bootstrap/pull |
| phase2 | Chuyển camera: node-a bỏ camera (release), node-b nhận camera (assignment FAILOVER) |
| restart | Restart cả hai node; trạng thái bootstrap đã lưu nên không bootstrap lại |
| phase3 | Đổi `keepArchivedMaxFor` ở node-b, giá trị mới phải sang node-a |
| phase4 | Xoá camera khỏi cả hai node; sau thời gian chờ failover (`failoverActiveDelaySec=5`), `vms_resource` và `vms_kvpair` của camera phải trống trên cả hai node |

Ở phase2, node-a (node bỏ camera) chỉ release assignment, không ghi `enableActive=0` lên ESC; kiểm tra bằng `transaction_log` của node-a.

Sau mỗi phase, script in ra:
- Số dòng log `database is locked`, `Failed to apply`, `Relay received`, `Relay pull ... failed` và `skip stale` của từng node (mong đợi `locked=0`, `failed_apply=0`).
- Kết quả của `compare_db.py`, so sánh các bảng `vms_resource`, `vms_kvpair`, `vms_resource_status`, `vms_resource_assignment`, `transaction_sequence`. Mật khẩu được che.

Nếu có dòng `CRASH: <node>`, log của node đó có stack trace (`## signal:`). Dòng `WARN: ... did not stop on SIGINT` nghĩa là node không dừng được bằng SIGINT và đã bị kill.

`log_count` khác nhau giữa hai node là bình thường: node nhận dữ liệu qua snapshot thì không có bản sao `transaction_log`.

## File
- `setup_nodes.py` — tạo thư mục, `config.ini` và `api-config.json` cho từng node; tham số `phase2`..`phase4`, `fo1`..`fo3`, `sp1`..`sp6` đổi cấu hình camera/loa, `pi2`..`pi5` đổi mục node-b trong danh sách server, `syncoff:<node>`/`syncon:<node>` đặt `*.sync_config_to_db` của node đang dừng.
- `check_failover.py` — các check của kịch bản failover.
- `check_speaker.py` — các check của kịch bản đồng bộ cấu hình loa.
- `check_peerinfo.py` — các check của kịch bản đổi thông tin peer.
- `mock_api.py` — trả HTTP 200 cho mọi hook.
- `compare_db.py` — so sánh các bảng đồng bộ giữa hai node.
- `run_cluster.sh` — chạy trọn kịch bản.
