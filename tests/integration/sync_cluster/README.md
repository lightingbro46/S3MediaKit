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
`check_worker_apis.py` gọi discovery ONVIF **và** discovery RTSP tới một IP bị drop gói (`192.0.2.1`, RFC 5737 TEST-NET-1; ONVIF mất ~20s, ffprobe ~10s) rồi đo độ trễ của `/media/mserver/healthcheck` trong suốt thời gian đó — cả hai dạng address đều phải chạy trên `api worker`, không chiếm poller nào. Ngoài ra kiểm tra `subnetScan` và `register` (lưu certificate, gọi lại không đổi gì). Node test chạy với `manager.enableAuthorize=0`.

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
`storage.tieringSplitGranularitySeconds` và retention chạy theo chu kỳ). Kịch bản này chạy với `manager.failoverActiveDelaySec=1800` (mặc định production) thay vì 5 s
của phase4: node-b bỏ camera trước khi node-a ghi assignment mới (lệch một chu kỳ reload cấu hình),
nếu thời gian chờ ngắn hơn khoảng đó thì node-b gỡ camera, xoá `info.txt` và replay dữ liệu failover
trên node-b trả 403 dù file ghi vẫn còn. `FO_WAIT_SEC` (mặc định 90)
là thời gian chờ mỗi bước. Mã thoát là số check FAIL.

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
- `setup_nodes.py` — tạo thư mục, `config.ini` và `api-config.json` cho từng node; tham số `phase2`..`phase4`, `fo1`..`fo3` đổi cấu hình camera.
- `check_failover.py` — các check của kịch bản failover.
- `mock_api.py` — trả HTTP 200 cho mọi hook.
- `compare_db.py` — so sánh các bảng đồng bộ giữa hai node.
- `run_cluster.sh` — chạy trọn kịch bản.
