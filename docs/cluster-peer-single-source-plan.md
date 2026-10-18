# Plan: một nguồn dữ liệu peer cho ClusterManager và SyncManager

Ngày: 2026-10-18 · Nền: commit 586d5fb2 (`fix(cluster): reconcile peer updates safely`)

## Trạng thái (2026-10-18)

- P0, P1, P2: xong. Unit P3 xong (`unit_health_monitor` 42/42, toàn bộ `unit.` 359/359).
- `run_cluster.sh` chế độ thường: đạt 4 phase; chế độ `failover`: 0 check lỗi.
- Còn lại: bước tích hợp "đổi `name`/`hasFailover` của node B" (mục P3) chưa viết vào `run_cluster.sh`.

## 1. Bối cảnh

Hiện có hai danh sách peer phải giữ khớp bằng tay:

| Nơi | Nội dung | Ai ghi |
|---|---|---|
| `ClusterManager::_map_peer_url` | peer đã qua health check → base URL | `onHealthCheckResult`, `addMediaServer`, `removeMediaServer` |
| `SyncManager::_peers` | peer cùng nhóm để sync → base URL | `ClusterManager` gọi `addPeer/removePeer`; `Manager.cpp` (rejoin) gọi `addPeer` |

Commit 586d5fb2 vá race giữa hai danh sách bằng revision và gọi `SyncManager` khi còn giữ
`ClusterManager::_mtx`. Review commit đó còn chỉ ra:

1. Đổi bất kỳ trường nào (`name`, `hasFailover`, cổng RTSP/RTMP) cũng xoá URL và gỡ peer khỏi sync
   cho tới khi health check trả về.
2. `removePeer` trong `addMediaServer` chạy ngoài khoá → có thể gỡ mất peer vừa được check thành công.
3. Peer khác nhóm làm `SyncManager::removePeer` in `InfoL` mỗi 60 s.
4. `serverGroupId` không có trong `MediaServerInfo::toJson/fromJson` → peer nạp từ `peer.peer_list`
   luôn ở nhóm 0 sau khi khởi động lại.
5. Thứ tự khoá mới `ClusterManager::_mtx → SyncManager::_mtx` là một bất biến ngầm.

Lỗi 2, 3, 5 sinh ra từ việc đồng bộ hai danh sách. Plan này bỏ hẳn danh sách thứ hai thay vì vá tiếp.

## 2. Mục tiêu và nguyên tắc

- `ClusterManager` là **nguồn duy nhất** về thành viên cụm, URL đã khoẻ và nhóm.
- `SyncManager` **đọc** danh sách peer sync từ `ClusterManager` mỗi lần cần (tick, health view,
  kiểm tra đuổi kịp), không lưu bản sao URL.
- `SyncManager` chỉ giữ **trạng thái riêng của sync** theo peer id (`_bootstrapping`,
  `_bootstrap_probes`, `_last_bootstrap_peer`, `_last_complete_pull_ms`).
- Quy tắc khoá: `ClusterManager` không bao giờ gọi `SyncManager` khi giữ `_mtx`; `SyncManager`
  không bao giờ gọi `ClusterManager` khi giữ `_mtx` của nó. Không còn lồng khoá giữa hai lớp.
- Giữ cơ chế revision của 586d5fb2 — nó vẫn cần để `_map_peer_url` không nhận kết quả cũ.

Ngoài phạm vi: health check theo sự kiện (thay timer 60 s), đổi giao thức sync, `cluster-media-forward-plan`.

## 3. Các bước

### P0 — Chuẩn hoá dữ liệu trong ClusterManager

Tệp: `manager/Server/ClusterManager.h/.cpp`

- P0.1 Lưu `serverGroupId`: `toJson` ghi `v["serverGroupId"]`; `fromJson` đọc
  `v.get("serverGroupId", 0).asInt()` (file cũ thiếu trường vẫn đọc được, nhóm 0 như hiện nay).
- P0.2 Gán mặc định `= false` cho `useWebDomain`, `useDomain`, `useCustomPath`.
- P0.3 `addMediaServer` chỉ tăng revision + xoá URL khi **endpoint đổi**
  (`buildOrginUrls(cũ) != buildOrginUrls(mới)`) hoặc peer mới. Đổi `name`, `hasFailover`,
  cổng không phải HTTP(S), `serverGroupId` → chỉ cập nhật `_map_server_info`, giữ URL và revision.
  Không đặt health check ngay trong trường hợp này.
- P0.4 Thêm API đọc:

  ```cpp
  // Peers of this server group that passed the last health check: id -> base URL. Excludes self.
  std::unordered_map<std::string, std::string> getSyncPeerUrls();
  ```

  Lọc trong một lần giữ `_mtx`: có URL, `serverGroupId == Cluster::kServerLocationId`, khác
  `General::kMediaServerId`.

### P1 — SyncManager đọc peer từ ClusterManager

Tệp: `manager/Extension/SyncManager.h/.cpp`

| Chỗ dùng `_peers` hiện tại | Thay bằng |
|---|---|
| `onTick` (`peers_copy = _peers`) | `peers_copy = ClusterManager::Instance().getSyncPeerUrls()` **trước** khi lấy `_mtx` |
| Chọn peer bootstrap (`_peers.count/at` trong khoá) | tra trong `peers_copy` |
| `maybePruneLog` (tập `healthy`) | khoá của `getSyncPeerUrls()` |
| `healthView` (lặp `_peers`) | lấy `getSyncPeerUrls()` ngoài khoá, rồi khoá để đọc `_last_complete_pull_ms` |
| `isCaughtUpSince` (lặp `_peers`) | như `healthView` |
| `maybeCleanupRemovedPeers(peers_copy, …)` | giữ nguyên, nhận `peers_copy` mới |

- P1.1 Xoá `_peers`, `addPeer`, `removePeer`, và dòng `_peers.clear()` trong `start()/stop()`.
- P1.2 Dọn trạng thái riêng theo **thành viên cụm**, không theo độ khoẻ: đầu `onTick`, lấy
  `getMediaServerIds()` lọc cùng nhóm; xoá khỏi `_bootstrap_probes`, `_last_complete_pull_ms`,
  `_last_bootstrap_peer` các id không còn là thành viên. Peer chỉ tạm mất kết nối giữ nguyên
  `_last_complete_pull_ms` — hiện `removePeer` xoá nó mỗi lần health check lỗi, làm lùi việc dọn
  dữ liệu node đã rời cụm (`shouldCleanupRemovedPeer`).
- P1.3 `_bootstrapping` không dọn theo danh sách: callback của `doBootstrap` luôn chạy (HTTP có
  timeout) và tự `erase`. Ghi rõ điều này trong comment.
- P1.4 Comment quy tắc khoá ở đầu `SyncManager.h` và `ClusterManager.h` (mục 2).

### P2 — Gỡ các lời gọi chéo

- P2.1 `ClusterManager.cpp`: bỏ mọi `SyncManager::addPeer/removePeer` trong `addMediaServer`,
  `removeMediaServer`, `onHealthCheckResult`. Giữ `onPeerAddedToCluster/onPeerRemovedFromCluster`
  (sự kiện thành viên, ghi DB trên sync worker), gọi ngoài khoá như hiện nay.
- P2.2 `onHealthCheckResult` chỉ còn cập nhật `_map_peer_url`; bỏ `GET_CONFIG` nhóm và nhánh
  `#ifdef ENABLE_SYNC_CHANGES` trong đó.
- P2.3 `server/Manager.cpp` `onServerQualified`: bỏ vòng `sync.addPeer(...)` sau `sync.start()` —
  tick đầu tiên tự đọc peer.
- P2.4 Cập nhật comment cũ ở `SyncManager::start()` (nhắc `ClusterManager::addPeer()` và
  `healthCheckPersistedPeers()` không còn tồn tại).

### P3 — Test

Unit (`tests/unit/manager/test_cluster_manager.cpp`, chạy qua `unit.health_monitor_all`):

- `serverGroupId` đi qua `toJson → fromJson` giữ nguyên; JSON cũ thiếu trường → 0.
- Đổi `name` / `hasFailover` giữ URL và revision; đổi IP/cổng HTTP xoá URL, tăng revision.
- `getSyncPeerUrls` loại: peer chưa khoẻ, peer khác nhóm, chính node này.
- Đổi nhóm của peer đang khoẻ: lập tức vào/ra `getSyncPeerUrls`, không cần health check.
- Giữ 4 test revision hiện có.

Unit SyncManager (thêm vào `test_sync_apply.cpp` hoặc file mới cùng target):

- `isCaughtUpSince` / `healthView` chỉ tính peer khoẻ cùng nhóm (seed qua `ClusterManagerTestPeer`).
- Peer mất kết nối rồi khoẻ lại giữ `_last_complete_pull_ms`; peer bị xoá khỏi cụm thì mất.

Tích hợp: `tests/integration/sync_cluster/run_cluster.sh` cả chế độ thường và `failover`.
Thêm bước: đổi `name`/`hasFailover` của node B trong danh sách ESC giả → sync A↔B không gián đoạn
(không có log `Removed peer`, `last_ok_age_sec` không tăng vọt).

Build: `build-test` (unit), `build` (MediaServer), `build-sync-off` (bảo đảm không còn tham chiếu
`SyncManager` ngoài `#ifdef`).

## 4. Thay đổi hành vi cần biết

| Tình huống | Trước | Sau |
|---|---|---|
| Peer vừa khoẻ | `addPeer` ngay, dùng ở tick sau | dùng ở tick sau (như cũ, ≤ `pullIntervalSec`) |
| Đổi tên / `hasFailover` | mất URL + gỡ khỏi sync tới khi check xong | không ảnh hưởng |
| Peer tạm lỗi health check | xoá `_last_complete_pull_ms` | giữ; chỉ xoá khi rời cụm |
| Khởi động lại, ESC chưa trả lời | peer nhóm 0 → không sync | peer giữ nhóm đã lưu → sync được ngay |
| Log peer khác nhóm | `Removed peer` mỗi 60 s | không có |

Rủi ro: `getSyncPeerUrls()` gọi mỗi tick và mỗi lần health view — chỉ là sao chép map vài phần tử,
không đáng kể. `HealthMonitor` vẫn dùng `getPeerUrls()` (mọi nhóm) như cũ.

## 5. Thứ tự commit đề xuất

1. P0 (dữ liệu + API mới, chưa ai dùng) + test P0.
2. P1 + P2 cùng một commit (bỏ `_peers` và lời gọi chéo phải đi cùng nhau) + test SyncManager.
3. Chạy `run_cluster.sh` thường + `failover`, ghi kết quả vào mô tả commit.
