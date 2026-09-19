#include <cstdlib>
#include <iostream>
#include <string>

#include "Common/config.h"
#include "Extension/Resource.h"
#include "Storage/DbStorage.h"
#include "Util/mini.h"

using namespace managerkit;
using namespace mediakit;
using namespace toolkit;

namespace {

bool expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main() {
    char temp_dir[] = "/tmp/s3-status-gap-test-XXXXXX";
    if (!mkdtemp(temp_dir)) {
        std::cerr << "FAILED: cannot create temporary database directory" << std::endl;
        return 1;
    }

    const std::string temp_path(temp_dir);
    mINI::Instance()[Database::kDbSavePath] = temp_path;
    mINI::Instance()[General::kMediaServerId] = std::string("status-gap-test");

    LocalResourceStatusGapImp repository;
    auto schema_txn = repository.getExecutor()->beginWriteTxn();
    schema_txn->execScript(
        "CREATE TABLE local_resource_status_gap ("
        "resource_id TEXT NOT NULL, gap_id TEXT NOT NULL, disconnect_time INTEGER NOT NULL, "
        "reconnect_time INTEGER NOT NULL DEFAULT 0, disconnect_reason TEXT NOT NULL DEFAULT '', "
        "state INTEGER NOT NULL DEFAULT 1 CHECK (state = 1), updated_time INTEGER NOT NULL, "
        "PRIMARY KEY (resource_id, gap_id));"
        "CREATE UNIQUE INDEX idx_status_gap_test_one_open ON local_resource_status_gap(resource_id) "
        "WHERE state = 1 AND reconnect_time = 0;");
    schema_txn->commit();

    auto &manager = ResourceStatusManager::Instance();
    const uint64_t now = static_cast<uint64_t>(time(nullptr));
    bool ok = true;

    ok &= expect(manager.onDisconnected("stream-a", "timeout", now - 10), "create first outage");
    ok &= expect(manager.onDisconnected("stream-a", "timeout", now - 9), "deduplicate repeated outage");
    ok &= expect(manager.onDisconnected("stream-a", "unauthorized", now - 8), "update reason in the same outage");

    auto rows = repository.findByResourceIdAndTimeRange("stream-a");
    ok &= expect(rows.size() == 1, "one resource has only one open gap");
    ok &= expect(!rows.empty() && rows[0].disconnect_time == now - 10, "reason update preserves outage start");
    ok &= expect(!rows.empty() && rows[0].disconnect_reason == "unauthorized", "latest reason is retained");

    ok &= expect(manager.onReconnected("stream-a", now), "close outage");
    ok &= expect(manager.onReconnected("stream-a", now + 1), "repeated reconnect is idempotent");
    rows = repository.findByResourceIdAndTimeRange("stream-a");
    ok &= expect(rows.size() == 1 && rows[0].reconnect_time == now, "reconnect closes without creating ONLINE row");

    ok &= expect(manager.onDisconnected("stream-a", "unauthorized", now + 10), "new outage with old reason is not lost");
    ok &= expect(manager.onDisconnected("stream-b", "network", now + 11), "create outage for second resource");
    const std::vector<std::string> stream_ids = {"stream-a", "stream-b"};
    auto batched = manager.getResourceStatusGaps(stream_ids, now + 5, now + 20);
    ok &= expect(batched.size() == 2, "batch query returns both resources");

    auto boundary = repository.findByResourceIdAndTimeRange("stream-a", now, now + 10);
    ok &= expect(boundary.empty(), "half-open query excludes gaps touching only a boundary");

    ok &= expect(manager.removeResourceStatusGaps("stream-a", now + 1), "retention call succeeds");
    rows = repository.findByResourceIdAndTimeRange("stream-a");
    ok &= expect(rows.size() == 1 && rows[0].disconnect_time == now + 10,
                 "retention removes closed gaps and preserves open gap");

    if (ok) {
        std::cout << "Resource status gap tests passed" << std::endl;
        return 0;
    }
    return 1;
}
