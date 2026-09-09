# Kiểm thử tích hợp Storage Tiering với Codex agent

Tài liệu này hướng dẫn chạy bộ kiểm thử Storage Tiering trên máy phát triển bằng Codex agent và một MinIO dành riêng cho test. Không sử dụng bucket, credential hoặc dữ liệu production.

## Phạm vi kiểm thử

Runner hiện chạy ba nhóm độc lập:

| Nhóm | Target | Nội dung chính |
|---|---|---|
| Unit | `unit_storage` | Schema, entity, policy, resource monitor và timefile |
| TierStorageManager integration | `integration_tier_storage_manager` | HOT → NAS/MinIO, restore, playback path resolution, retention, concurrency, SearchEngine, camera statistics/record root, policy lifecycle và restart recovery |
| MinIO integration | `integration_tier_storage_minio` | Register pool, upload, exists, download, so sánh nội dung và delete |

Mỗi scenario `TierStorageManager` chạy trong child process riêng với SQLite và filesystem tạm dưới `/tmp`. Case restart recovery chạy hai process `prepare` và `verify` trên cùng một database tạm. Fixture tự dọn dữ liệu sau khi kết thúc. Hai scenario orchestration HOT → MinIO và restore từ MinIO chỉ chạy khi process test nhận đủ `TIER_TEST_S3_*`; thiếu cấu hình sẽ được CTest ghi rõ là `Skipped`.

Case SearchEngine khởi tạo cả MediaServer DB (`migration/mserver_updates`) và ESC DB (`migration/updates`), vì ownership lookup dùng `vms_resource_assignment` trong ESC DB trước khi đọc timefile cục bộ. Timeline metadata vẫn nằm ở global record root; `CameraOption::recordRootPath` chỉ đổi vị trí MP4 vật lý.

Case concurrency dùng một S3 fault server chỉ bind loopback, giữ mỗi download đủ lâu để kiểm tra trạng thái durable trong SQLite. Test xác nhận không quá `storage.auto_restore_max_concurrent=2` child ở trạng thái `RUNNING` và vẫn còn child `PENDING` khi capacity đã đầy. Môi trường sandbox phải cho phép bind TCP loopback.

Các case playback gọi trực tiếp `resolvePlaybackSegmentPath()` để kiểm tra file thường, file rỗng, symlink, file không đọc được và COLD range chưa có restore cache. Case unreadable hạ quyền từ root sang `nobody`; trong user namespace không cho phép đổi UID/GID, riêng case này được đánh dấu `Skipped` thay vì kiểm tra sai dưới quyền root.

MinIO test chỉ thao tác object dưới prefix riêng `agent-runs/<pid>`. Test không xóa bucket hoặc object không thuộc lần chạy hiện tại.

## Điều kiện chuẩn bị

- Linux với compiler hỗ trợ C++11.
- CMake và GoogleTest đã cài đặt.
- AWS SDK đã có tại `3rdpart/aws-sdk/bin/`.
- MinIO đang chạy và health endpoint trả về HTTP thành công.
- Bucket test đã được tạo trước và credential có quyền đọc/ghi/xóa object trong bucket đó.
- Để kiểm tra capacity MinIO, credential test phải có quyền `admin:Prometheus`;
  test coi metrics bị `401/403` là thất bại thay vì capacity bằng `0`.
- Codex agent được khởi động từ process nhận các biến `TIER_TEST_S3_*`.

Kiểm tra MinIO mà không truyền credential:

```bash
curl --fail --max-time 5 \
  "${TIER_TEST_S3_ENDPOINT%/}/minio/health/live"
```

## Khai báo biến môi trường an toàn

Không ghi credential vào repository, `config.ini`, script, prompt hoặc command có thể lưu trong shell history. Nhập credential tương tác:

```bash
read -r -p 'MinIO endpoint: ' TIER_TEST_S3_ENDPOINT
read -r -p 'MinIO access key: ' TIER_TEST_S3_ACCESS_KEY
read -r -s -p 'MinIO secret key: ' TIER_TEST_S3_SECRET_KEY
printf '\n'
read -r -p 'MinIO test bucket [s3mediakit-tier-test]: ' TIER_TEST_S3_BUCKET
TIER_TEST_S3_BUCKET="${TIER_TEST_S3_BUCKET:-s3mediakit-tier-test}"

export TIER_TEST_S3_ENDPOINT
export TIER_TEST_S3_ACCESS_KEY
export TIER_TEST_S3_SECRET_KEY
export TIER_TEST_S3_BUCKET
```

Chỉ kiểm tra trạng thái biến, không in giá trị:

```bash
for name in \
  TIER_TEST_S3_ENDPOINT \
  TIER_TEST_S3_ACCESS_KEY \
  TIER_TEST_S3_SECRET_KEY \
  TIER_TEST_S3_BUCKET; do
  if [[ -n "${!name:-}" ]]; then
    printf '%s=SET\n' "$name"
  else
    printf '%s=UNSET\n' "$name"
  fi
done
```

## Truyền biến môi trường vào Codex agent

Biến được `export` sau khi một agent đã khởi động sẽ không tự xuất hiện trong process agent đó. Hãy tạo session Codex mới từ chính terminal vừa khai báo biến.

### Codex CLI đã có trong PATH

```bash
codex \
  -C /home/s3pro/S3MediaKit.worktrees/feature-tier-storage \
  -c shell_environment_policy.inherit=all \
  -c shell_environment_policy.ignore_default_excludes=true
```

`ignore_default_excludes=true` là cần thiết vì tên biến credential có chứa `KEY` và `SECRET`. Nên khởi động từ một shell sạch, không chứa credential không liên quan.

### Terminal báo `codex: command not found`

Codex CLI có thể nằm bên trong VS Code extension nhưng chưa có trong `PATH`. Tìm binary mới nhất:

```bash
CODEX_CLI_BIN="$(find /root/.vscode-server/extensions \
  -type f -path '*/bin/linux-x86_64/codex' -print \
  | sort -V | tail -n 1)"

test -n "$CODEX_CLI_BIN" && test -x "$CODEX_CLI_BIN"
"$CODEX_CLI_BIN" --version
```

Khởi động session bằng binary vừa tìm được:

```bash
"$CODEX_CLI_BIN" \
  -C /home/s3pro/S3MediaKit.worktrees/feature-tier-storage \
  -c shell_environment_policy.inherit=all \
  -c shell_environment_policy.ignore_default_excludes=true
```

Không cài package Snap chỉ dựa trên gợi ý tự động của shell. Cần xác minh nguồn package trước khi cài CLI độc lập.

Nếu đang dùng Codex trong VS Code, việc export biến trong integrated terminal không cập nhật extension process đang chạy. Dùng CLI như trên là cách ổn định nhất; phương án còn lại là đóng hoàn toàn extension host/VS Code rồi khởi động lại từ terminal đã export biến.

## Chạy test bằng agent

Trong session Codex mới, gửi yêu cầu:

```text
Dùng $tier-storage-testing chạy toàn bộ tier-storage tests. Báo riêng kết quả unit, TierStorageManager integration và MinIO integration. Skipped không được tính là pass.
```

Agent sử dụng skill tại:

```text
.codex/skills/tier-storage-testing/SKILL.md
```

Runner thực tế:

```text
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

Agent phải giữ nguyên các thay đổi đang có trong worktree, không in credential và không thao tác object ngoài prefix của test.

## Chạy runner trực tiếp

Có thể chạy cùng workflow mà không qua prompt agent:

```bash
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

Runner thực hiện theo thứ tự:

1. Xác nhận các biến môi trường bắt buộc.
2. Kiểm tra MinIO health endpoint.
3. Configure CMake với test, Tier Storage và AWS SDK.
4. Build ba target test.
5. Chạy `unit.storage_all`.
6. Chạy các scenario `integration.tier-manager.*`.
7. Chạy `integration.tier_storage_minio_all`.

Đổi build directory khi cần:

```bash
export TIER_TEST_BUILD_DIR="$PWD/build-tier-test"
.codex/skills/tier-storage-testing/scripts/run_tier_storage_tests.sh
```

## Chạy từng nhóm riêng biệt

Configure và build:

```bash
cmake -S . -B build \
  -DENABLE_TESTS=ON \
  -DENABLE_TIER_STORAGE=ON \
  -DENABLE_AWS_SDK=ON

cmake --build build --target \
  unit_storage \
  integration_tier_storage_manager \
  integration_tier_storage_minio \
  -j2
```

Unit storage:

```bash
ctest --test-dir build \
  -R '^unit\.storage_all$' \
  --output-on-failure
```

TierStorageManager integration:

```bash
ctest --test-dir build \
  -R '^integration\.tier-manager\.' \
  --output-on-failure
```

MinIO integration:

```bash
ctest --test-dir build \
  -R '^integration\.tier_storage_minio_all$' \
  --output-on-failure
```

Chạy một scenario manager cụ thể:

```bash
ctest --test-dir build \
  -R '^integration\.tier-manager\.RunsCycleAndMovesHotSegmentToNas$' \
  --output-on-failure
```

## Đọc kết quả

Một lần chạy đầy đủ phải báo riêng:

- Build thành công hay thất bại.
- Unit test pass/fail.
- Số scenario `TierStorageManager` pass/fail.
- MinIO integration pass/fail/skipped.

`GTEST_SKIP()` hoặc `SKIPPED` không phải là pass của MinIO. Nếu runner hoàn tất bình thường với đầy đủ biến môi trường, MinIO test phải thực hiện thật các thao tác upload/download/delete.

Ví dụ kết quả manager hợp lệ khi đã cấu hình MinIO:

```text
integration.tier-manager.RegistersHotSegmentIdempotently          Passed
integration.tier-manager.RunsCycleAndMovesHotSegmentToNas         Passed
integration.tier-manager.RunsCycleAndMovesHotSegmentToMinio       Passed
integration.tier-manager.RestoresSegmentFromMinio                 Passed
integration.tier-manager.ReusesActiveRestoreForDuplicateRequest   Passed
integration.tier-manager.MarksExpiredAndDeletesAfterApproval      Passed
integration.tier-manager.ProtectsRetentionUntilGuardsExpire       Passed
integration.tier-manager.EnforcesRestoreConcurrencyLimit          Passed
integration.tier-manager.SearchEngineReflectsTierMetadata         Passed
integration.tier-manager.LoadsAndRefreshesSavedCameraRecordRoot   Passed
integration.tier-manager.PolicyLifecycleChangesCameraStorage      Passed
integration.tier-manager.ResolvesExistingEmptyAndSymlinkPlaybackFiles Passed
integration.tier-manager.RejectsUnreadablePlaybackFile            Passed
integration.tier-manager.RequiresRestoreWhenObjectCacheIsMissing  Passed
integration.tier-manager.RecoversRunningWorkAcrossFreshProcesses Passed
```

## Xử lý lỗi thường gặp

### `Missing required environment variable`

Biến chưa được export trong process chạy runner. Khai báo lại biến và mở session agent mới. Không gửi credential qua prompt.

### `MinIO healthcheck failed`

Kiểm tra:

- MinIO process đang chạy.
- Dùng API port, không dùng WebUI port.
- Endpoint truy cập được từ network namespace của agent.
- Firewall, container port mapping và proxy không chặn kết nối.

### MinIO test bị skipped

Agent process không nhận đủ endpoint/access key/secret key. Kiểm tra bằng trạng thái `SET/UNSET`, sau đó khởi động session mới với `shell_environment_policy` như trên.

### Upload trả về Access Denied hoặc bucket không tồn tại

Tạo bucket test trước, kiểm tra policy của credential và đảm bảo tên bucket trong `TIER_TEST_S3_BUCKET` khớp. Không cho test dùng bucket production.

### CMake không tìm thấy AWS SDK

Xác nhận các thư mục sau tồn tại:

```text
3rdpart/aws-sdk/bin/include
3rdpart/aws-sdk/bin/lib
```

Sau đó configure lại với `-DENABLE_AWS_SDK=ON`.

### Agent pass unit nhưng chưa chứng minh MinIO

Đọc riêng kết quả `integration.tier_storage_minio_all`. Unit và HOT → NAS không chứng minh kết nối hoặc quyền MinIO.

## Coverage hiện tại và phần còn thiếu

Đã tự động hóa:

- HOT registration idempotent.
- HOT → NAS qua `TierStorageManager::runTieringCycle()`.
- HOT → MinIO qua `TierStorageManager::runTieringCycle()`; kiểm tra object/content trước khi nguồn HOT bị xóa.
- Restore từ MinIO; kiểm tra durable job và nội dung file restore.
- Hai request restore trùng nhau tái sử dụng cùng active job/child.
- Retention approval: đánh dấu `EXPIRED`, giữ file trước approval, xóa vật lý và chuyển `DELETED` sau approval.
- Protected video và retention override chặn xóa đến khi cả hai guard hết hiệu lực.
- Giới hạn concurrent restore bằng trạng thái child durable trong SQLite.
- SearchEngine trả đúng `tier` và `restore_required` khi range chuyển HOT → COLD.
- `StatisticRecorder::loadSavedCameraStatistics()` phục hồi `CameraOption::recordRootPath`; effective root từ policy được lưu bền vững lại vào `info.txt`.
- Lifecycle policy create/update/delete đổi HOT record root và đường tiering thực tế, chặn xóa policy đang được camera dùng, rồi fallback về system default sau remove override.
- Recovery tiering job và restore child từ `RUNNING` về `PENDING` qua process mới.
- Vòng đời object MinIO trực tiếp qua `TierObjectStorage`.

Chưa tự động hóa đầy đủ:

- Missing-file playback fallback.
- MinIO mất kết nối giữa chừng trong upload/download.
- Backfill cursor qua stable window.
- Chọn custom HOT pool khi bootstrap có nhiều pool hợp lệ.

Không dùng kết quả hiện tại để kết luận toàn bộ Tier Storage đã được cover. Ma trận scenario chi tiết nằm tại `.codex/skills/tier-storage-testing/references/scenarios.md`.
