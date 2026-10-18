# Fixture cho test tích hợp app desktop monitor

Dựng các MediaServer để app desktop **3spro-monitor** (Media Server Monitor, Windows) kiểm thử với
server thật. Mỗi node đóng một kịch bản. Phía app đọc `fixture.json` rồi gọi các node từ Windows
(repo app: `docs/INTEGRATION-TEST.md`, ROADMAP D28).

## Yêu cầu

- `MediaServer` bản **Debug** (chỉ bản Debug đọc `./api-config.json`) của checkout này, ở
  `release/linux/Debug` (đổi bằng `MONITOR_BIN_DIR`).
- Bản `develop` (chưa có `/media/monitor/*`): `bash build_target.sh develop`. Script dựng worktree
  `../S3MediaKit.worktrees/it-develop` (detached), symlink submodule từ checkout này, dùng lại
  FFmpeg/AWS SDK/librdkafka trong `release/linux/3rdpart`, rồi chỉ build `MediaServer` cùng các target
  copy thư viện. `JOBS` mặc định là 4, vì WSL 8 GB thiếu RAM khi build Debug với `nproc` luồng.
  Đổi bằng `DEVELOP_BIN_DIR`.
- Python 3 (chỉ thư viện chuẩn) và `ffmpeg` để dựng clip test.
- libfaketime cho node lệch giờ. Nếu máy chưa cài, `fixture.py` tự tải gói Ubuntu (`apt-get download
  libfaketime`) và giải nén vào `~/.cache/s3mk_monitor_app`, không cài vào hệ thống. Có thể chỉ đường
  dẫn khác bằng `FAKETIME_LIB`. Không có libfaketime thì node đó bị bỏ, kèm cảnh báo.
- Các cổng trống:
  - HTTP 18181–18189, RTSP 18581+, RTMP 18381+;
  - service API 18190;
  - nguồn RTSP 18194;
  - S3 giả 18195.

## Chạy

```bash
python3 tests/integration/monitor_app/fixture.py up       # dừng bản cũ, dựng lại, ghi fixture.json
python3 tests/integration/monitor_app/fixture.py status
python3 tests/integration/monitor_app/fixture.py down
```

`--work DIR` (mặc định `/tmp/s3mk_monitor_app`) và `--only monitor|develop` (chỉ dựng một bản).

## Kịch bản

| Node | Bản | Kịch bản | Thiết lập |
|---|---|---|---|
| `it-mon-ok` | monitor | `ok` | Cluster 101, nhận failover, 1 camera ghi liên tục từ `rtsp_source.py` |
| `it-mon-peer` | monitor | `peer` | Cluster 101, không nhận failover |
| `it-mon-warn` | monitor | `warning` | Cluster 102, `globalmonitor.memory_warning_threshold=1`, camera trỏ vào cổng đóng |
| `it-mon-default` | monitor | `default_secret` | Cluster 0, `api.secret` mặc định đặt lại qua `setServerConfig` sau khi khởi động (`main.cpp` đổi secret mặc định lúc khởi động) |
| `it-mon-off` | monitor | `disabled` | Cluster 0, `monitor.enable=0` |
| `it-dev` | develop | `old_build` | Cluster 103, 1 camera ghi |
| `it-mix-mon`, `it-mix-dev` | monitor + develop | `mixed` | Cluster 104 gồm một node mỗi bản (nâng cấp dở dang). `it-mix-mon` có pool MINIO trên `fake_s3.py`, kiểm DB mỗi 10 giây |
| `it-mon-clock` | monitor | `clock_skew` | Cluster 106, chạy dưới libfaketime với `FAKETIME=+10m`, chỉ giờ thực; đồng hồ monotonic giữ nguyên |

Thiết lập chung cho mọi node:

- Ngưỡng swap/CPU được nâng lên, vì các node dùng chung máy WSL.
- `monitor.coverageIntervalSec=60`.
- `cluster.enableFailover` khớp với cờ `failover` VMS đẩy xuống. Nếu không, sau khi `setServerConfig` nạp lại cấu hình, node quay về giá trị trong ini cho tới lần VMS đẩy kế tiếp.

`expect` của từng node liệt kê các giá trị `/media/monitor/summary` mà kịch bản đạt được khi ổn định.
Phía app chờ đủ các giá trị đó rồi mới thu.

## Gây sự cố khi đang chạy: `ctl`

Kịch bản động của app (ROADMAP 8.5 bên app) gây sự cố trên fixture đang chạy rồi gỡ ra:

```bash
F=tests/integration/monitor_app/fixture.py
python3 $F ctl stop it-mon-peer                  # SIGINT như vận hành tắt server; start để bật lại
python3 $F ctl camera it-mon-ok-rec offline      # rtsp_source.py ngắt luồng của camera (drop_spec.json); online
python3 $F ctl pool down                         # tắt fake_s3.py: pool OFFLINE, storage critical; up
python3 $F ctl vms it-mon-warn --group 105 --failover 0 --bulk 520
                                                 # VMS đổi nhóm, cờ failover, thêm 520 camera (cổng đóng)
python3 $F ctl secret it-mon-default it-rotated  # đổi api.secret qua setServerConfig; "initial" để trả lại
python3 $F ctl reset                             # gỡ mọi thứ ở trên
```

Thời gian server phản ứng, đo trên WSL Ubuntu 22.04:
- `vms`: nhóm, cờ failover và danh sách camera mới có hiệu lực dưới 30 giây (hook cấu hình, `configReapplySec=0`).
- `pool down`: pool chuyển `OFFLINE` sau khoảng 20 giây.
- `camera offline`: camera offline ngay; `RecordingStalled` mở sau khoảng 160 giây (`recording.stalledSec` 150).

`fixture.json` luôn theo trạng thái hiện tại: `secret`, `group`, `failover`, `bulk`.
`initial.json` giữ trạng thái lúc `up` để `reset` và `secret … initial` trả về.

## `fixture.json`

```json
{
  "schema": 1, "host": "172.x.x.x", "workDir": "/tmp/s3mk_monitor_app",
  "script": "/home/…/tests/integration/monitor_app/fixture.py",
  "builds": { "monitor": { "ref": "...", "commit": "...", "dirty": false, "binaryTime": 0 } },
  "nodes": [ { "id": "it-mon-ok", "scenario": "ok", "build": "monitor", "url": "http://172.x.x.x:18181",
               "secret": "...", "group": 101, "failover": true, "cameras": ["it-mon-ok-rec"], "bulk": 0,
               "pool": null, "clockOffset": null, "monitorApi": true, "expect": { "overall": "ok" } } ]
}
```

`url` dùng địa chỉ eth0 của WSL, không dùng 127.0.0.1. MediaServer cho client loopback đi qua mà
không cần secret, nên chỉ client ngoài loopback mới kiểm được sai secret hoặc thiếu secret.

Đổi tên kịch bản hay field trong `fixture.json` thì phải sửa cả phía app:
- `scripts/it/capture.mjs`, `e2e.mjs`, `dynamic.mjs`;
- `src/services/api/contract.test.ts`;
- `src-tauri/src/contract.rs`.
