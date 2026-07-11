---
description: "Use when writing code in manager/: adding entities, repositories, database migrations, sync handlers, config keys, singletons, camera managers, storage tiering, or cluster sync. Covers DbSchema, SqliteRepository, TransactionLog, SyncManager, MigrationHistory, and managerkit conventions."
applyTo: "manager/**"
---

# managerkit Conventions

All code lives in the `managerkit` namespace. Includes: `using namespace std; using namespace toolkit; using namespace mediakit;`.

## Entity Declaration

Define entities in `manager/Storage/` as plain structs. Register with the ORM macros from `DbSchema.h`:

```cpp
// Single primary key
DECLARE_ENTITY(MyEntity, "my_table",
    {"id"},
    &MyEntity::id,   "id",
    &MyEntity::name, "name"
)

// Composite primary key — use MAKE_PK
DECLARE_ENTITY(MyEntity, "my_table",
    MAKE_PK("peer_guid", "sequence"),
    &MyEntity::peer_guid, "peer_guid",
    &MyEntity::sequence,  "sequence"
)

// No primary key
DECLARE_ENTITY_NO_PK(MyEntity, "my_table",
    &MyEntity::field, "col_name"
)
```

- Use `Optional<T>` (from `DbSchema.h`) for nullable fields — **never** `std::optional`.
- Implement `Json::Value toJson() const` and `static T fromJson(const Json::Value &)` on every entity that crosses a wire or gets logged.

## Repository Pattern

Repositories extend `SqliteRepository<T>`. Constructor takes a DB tag constant:

```cpp
class MyEntityRepository : public SqliteRepository<MyEntity> {
public:
    MyEntityRepository() : SqliteRepository<MyEntity>(Database::kEdgeStorageControllerDb) {}
    // or: Database::kMediaServerDb  for MediaServer DB
};
```

- `save(obj)` — INSERT OR REPLACE (excludes PK unless `include_id=true`).
- For custom queries use `_executor->executeRaw(...)` or `_executor->execDML(...)`.
- For **multi-table transactions**, get a shared txn via `getExecutor()->execTxn()` and pass it to `execDMLWithTxn` / `onUpsertWithTxn` variants.

Two database tags:

| Constant | DB file | Purpose |
|----------|---------|---------|
| `Database::kEdgeStorageControllerDb` | `esc.sqlite` | ESC data: resources, sync, bookmarks, storage |
| `Database::kMediaServerDb` | `mserver.sqlite` | MediaServer data: auth, sessions, metrics |

## Database Migrations

- Add a new `.sql` file to `migration/updates/` (ESC) or `migration/mserver_updates/` (MediaServer).
- Name files with a zero-padded prefix: `09_my_table.sql`. Files are applied in sorted order.
- Do **not** alter existing migration files; always add a new one.
- `MigrationHistory` tracks applied files; already-applied files are skipped.

## Config Keys

Declare keys in a namespace with `onceToken` defaults:

```cpp
namespace MyFeature {
#define MY_FIELD "myfeature."
const string kFooBar = MY_FIELD"foo_bar";

static onceToken token([]() {
    mINI::Instance()[kFooBar] = 42;
});
} // namespace MyFeature
```

Read with `GET_CONFIG(type, varname, key)`.

## Singleton Classes

```cpp
// Header
class MyManager : public std::enable_shared_from_this<MyManager> {
public:
    using Ptr = std::shared_ptr<MyManager>;
    static MyManager &Instance();
    ~MyManager() = default;
private:
    MyManager();
    toolkit::Timer::Ptr _timer;
    std::mutex _mtx;
};

// Impl (.cpp)
INSTANCE_IMP(MyManager)

MyManager::MyManager() {
    _timer = std::make_shared<Timer>(60.0f, []() {
        MyManager::Instance().onManager();  // ← call Instance(), never capture this
        return true;
    }, nullptr);
}
```

## Thread Safety Rules

- **Never** capture raw `this` in timer or `doDelayTask` callbacks; always use `Instance().method()` or `weak_from_this()+lock()`.
- Use `std::lock_guard<std::mutex>` (or `std::recursive_mutex` for classes that relock).
- Async ops on a camera/poller: call `poller->async([obj]() { obj->method(); })` where `obj` is a `shared_ptr`.

## Sync-able Tables (TableSyncHandler)

When a new entity must participate in multi-node gossip sync:

1. Implement `makeSyncHandler()` on the repository/Imp class — returns `TableSyncHandler` with `rowKey`, `onUpsert`, `onDelete`, `onSnapshot`.
2. Provide `onUpsertWithTxn` / `onDeleteWithTxn` if the table is written in batch transactions.
3. Call `SyncManager::Instance().registerTable("table_name", handler)` at startup (e.g., in `Manager.cpp` init).
4. Do **not** add a hardcoded if/else branch in `SyncManager`; use the registration API.

## JSON Conventions

- Use `jsoncpp` (`Json::Value`) — never raw string manipulation.
- Serialize with `StrJsonUtils::writeJsonString(jsonValue)`.
- Parse with `StrJsonUtils::parseJsonString(str, &val)`.

## Related Docs

- DB sync protocol: [docs/api-sync.md](../../docs/api-sync.md)
- Storage tiering: [docs/api-storage-tiering.md](../../docs/api-storage-tiering.md)
- Bookmark API: [docs/api-bookmark.md](../../docs/api-bookmark.md)
