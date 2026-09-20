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
- `setup_nodes.py` — tạo thư mục, `config.ini` và `api-config.json` cho từng node; tham số `phase2`/`phase3` đổi cấu hình camera.
- `mock_api.py` — trả HTTP 200 cho mọi hook.
- `compare_db.py` — so sánh các bảng đồng bộ giữa hai node.
- `run_cluster.sh` — chạy trọn kịch bản.
