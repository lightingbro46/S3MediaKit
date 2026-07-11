---
description: "Use when writing code that uses S3ToolKit utilities: Timer, EventPoller, WorkThreadPool, RingBuffer, SqlitePool, QueryBuilder, NoticeCenter, ResourcePool, onceToken, logger macros (TraceL/DebugL/InfoL/WarnL/ErrorL), TcpServer/Session, or Ticker. Covers correct async dispatch, ring buffer reader setup, parameterized SQL, observer registration, and common pitfalls."
applyTo: "3rdpart/S3ToolKit/**"
---

# S3ToolKit Usage Conventions

All toolkit types live in `namespace toolkit`. Import with `using namespace toolkit;`.

## Logger Macros (`Util/logger.h`)

```cpp
TraceL << "detail: " << value;   // LTrace — verbose debug
DebugL << "debug: " << value;    // LDebug
InfoL  << "started";             // LInfo
WarnL  << "unexpected: " << ex.what();   // LWarn
ErrorL << "fatal: " << msg;      // LError
```

- Use the `L`-suffix macros everywhere; never call `Logger::write()` directly.
- For per-connection / per-object logging include the object address: `TraceP(this) << "msg"` (defined as `TraceP(ptr)` in logger.h).
- Log level order: `LTrace(0) < LDebug < LInfo < LWarn < LError`.

## Timer (`Poller/Timer.h`)

```cpp
// Repeating timer — return true to continue, false to stop
_timer = std::make_shared<Timer>(30.0f, []() {
    MyManager::Instance().onManager();  // ← call Instance(), never capture this
    return true;
}, nullptr /*poller — nullptr picks a WorkThreadPool poller*/);
```

- Always store as `Timer::Ptr` — RAII destroys the timer when the ptr resets.
- **Never** capture raw `this` in the callback. Use `Instance()` for singletons, or `weak_from_this()` + lock for non-singletons:
  ```cpp
  auto weak = weak_from_this();
  _timer = std::make_shared<Timer>(5.0f, [weak]() {
      auto self = weak.lock();
      if (!self) return false;
      self->onManager();
      return true;
  }, nullptr);
  ```
- Reset the timer in the destructor to cancel pending firings: `_timer.reset();`.

## EventPoller (`Poller/EventPoller.h`)

```cpp
// Get a lightly loaded poller
auto poller = EventPollerPool::Instance().getPoller();
// or prefer current thread poller
auto poller = WorkThreadPool::Instance().getPoller();

// Dispatch a task onto the poller thread
poller->async([=]() { /* runs on poller thread */ });

// may_sync=true: executes synchronously if already on this poller thread
poller->async(task, /*may_sync=*/true);

// Highest-priority task
poller->async_first(task);

// One-shot delayed task (ms)
poller->doDelayTask(500, []() -> uint64_t {
    doSomething();
    return 0;  // return 0 to cancel; return N to reschedule after N ms
});

// Check if we are on this poller's thread
if (poller->isCurrentThread()) { /* direct call OK */ }
```

- Every network `Session` owns a poller. Use `session->getOwnerPoller(...)` to dispatch tasks back to it.
- Don't call blocking operations (file I/O, long loops) inside a poller task — offload to `WorkThreadPool`.

## WorkThreadPool (`Thread/WorkThreadPool.h`)

```cpp
// Background CPU work
WorkThreadPool::Instance().getExecutor()->async([]() { doHeavyWork(); });
```

- `getPoller()` returns a lightly loaded `EventPoller` (I/O bound).
- `getExecutor()` returns a thread pool executor (CPU bound).

## RingBuffer (`Util/RingBuffer.h`)

```cpp
using RingType = toolkit::RingBuffer<Frame::Ptr>;
RingType::Ptr ring = std::make_shared<RingType>();

// Writer side
ring->write(frame, frame->keyFrame());

// Reader side — each reader is bound to a poller
auto reader = ring->attach(poller, /*is_key_frame_start=*/true);
reader->setReadCB([](const Frame::Ptr &frame) { /* runs on poller thread */ });
reader->setDetachCB([]() { /* ring was destroyed */ });
reader->flushGop();   // replay cached GOP immediately
```

- All `setReadCB` / `setDetachCB` callbacks run on the **reader's poller thread** — no external locking needed inside callbacks.
- Call `flushGop()` immediately after `setReadCB` to replay the GOP cache to the new reader.

## SqlitePool (`Util/SqlitePool.h`)

```cpp
// Initialize (once per DB file)
auto pool = std::make_shared<SqlitePool>();
pool->Init(full_path, /*wal_mode=*/true);
pool->setSize(3 + std::thread::hardware_concurrency());

// Synchronous query — throws SqliteException on error
pool->syncQuery(sql, args...);

// Fire-and-forget async write (auto-retries 3×)
pool->asyncQuery(sql, values_vector);

// Multi-statement transaction
auto txn = std::make_shared<SqliteTransaction>(pool);
txn->execScript(sql_block);
txn->commit();   // or it auto-rolls-back on destruct without commit
```

- WAL checkpoint runs automatically every 30 s — do not call `PRAGMA wal_checkpoint` manually.
- Always catch `SqliteException` around `syncQuery`; propagate or log, never swallow.
- Use `SqliteTransaction` when you need atomicity across multiple statements.

## QueryBuilder (`Util/QueryBuilder.h`)

```cpp
auto query = QueryBuilder()
    .insertInto("my_table")
    .values({{"col1", val1}, {"col2", val2}})
    .onConflict({"pk_col"})   // optional upsert
    .build();
auto params = query.getParams();

// Execute via QueryExecutor
QueryExecutor<SqlitePool, SqliteBaseWriter>::execDML(pool, query);
auto rows = QueryExecutor<SqlitePool, SqliteBaseWriter>::executeRaw(pool, query);
```

- **Always use parameterized queries** — never concatenate user input into the SQL string directly.
- `getParams()` returns the bound values in the same order as `?` placeholders.

## NoticeCenter (`Util/NoticeCenter.h`)

```cpp
// Subscribe (tag = this or any stable pointer used as a key)
NoticeCenter::Instance().addListener(this, "event.name", [](ArgType arg) { ... });

// Unsubscribe
NoticeCenter::Instance().delListener(this);

// Emit (returns number of listeners notified)
int n = NoticeCenter::Instance().emitEvent("event.name", arg1, arg2);
if (!n) { /* no listeners — handle fallback */ }
```

- Lambda signature must match the declared `#define BroadcastXxxArgs` exactly.
- Always call `delListener(this)` in the destructor if you registered in the constructor.
- `emitEvent` copies the listener map before iterating — safe to add/remove listeners inside a callback.

## onceToken (`Util/onceToken.h`)

```cpp
// Run-once initializer (config key registration)
static onceToken token([]() {
    mINI::Instance()[key] = default_value;
});

// Scoped cleanup (RAII guard)
onceToken guard(nullptr, [&]() { cleanup(); });
```

- Constructor body runs immediately; destructor body runs on scope exit.
- `static` onceToken runs once per translation unit load — safe for global init.
- Pass `nullptr` as the first argument when only a destructor body is needed.

## ResourcePool (`Util/ResourcePool.h`)

```cpp
ResourcePool<MyObj> pool;
pool.setSize(8);

auto obj = pool.obtain();   // shared_ptr_imp<MyObj>
obj->doWork();
// obj goes out of scope → returns to pool automatically
// To discard instead of recycle:
obj.quit();
```

## TcpServer / Session (`Network/`)

```cpp
auto server = std::make_shared<TcpServer>();
server->start<MySession>(port, "0.0.0.0", backlog);
```

Implement a session:
```cpp
class MySession : public toolkit::Session {
public:
    MySession(const Socket::Ptr &sock) : Session(sock) {}
    void onRecv(const Buffer::Ptr &buf) override { /* handle data */ }
    void onError(const SockException &err) override { /* handle close */ }
    void onManager() override { /* periodic health check, called ~2/s */ }
};
```

- `send(Buffer::Ptr)` is thread-safe.
- `shutdown()` closes the connection gracefully.
- Wrap with `SessionWithSSL<MySession>` for TLS.

## Ticker (`Util/TimeTicker.h`)

```cpp
Ticker ticker;
doWork();
auto ms = ticker.elapsedTime();  // since last resetTime() or construction

// Auto-warn if destructor is called after threshold_ms
Ticker guard(500 /*ms threshold*/);
```
