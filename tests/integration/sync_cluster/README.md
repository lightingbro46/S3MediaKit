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

## Kịch bản
| Phase | Nội dung |
|---|---|
| phase1 | Camera `cam-sync-1` chạy trên node-a; node-b nhận dữ liệu qua bootstrap/pull |
| phase2 | Chuyển camera: node-a bỏ camera (release), node-b nhận camera (assignment FAILOVER) |
| restart | Restart cả hai node; trạng thái bootstrap đã lưu nên không bootstrap lại |
| phase3 | Đổi `keepArchivedMaxFor` ở node-b, giá trị mới phải sang node-a |

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
