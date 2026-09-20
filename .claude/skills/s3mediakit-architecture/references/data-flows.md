# End-to-end flows

Traces to orient yourself. Follow the named symbols with `rg` — signatures change.

## 1. Camera → live stream

```
CameraManager                 owns the camera set, applies CameraOption
  └─ GenericRtspCameraImp     device-specific implementation (ONVIF profiles, URLs, auth)
      └─ StreamSource         one per stream (primary/secondary), bound to an EventPoller
          │                   DeviceSourceEventInterceptor; tracks live state, status,
          │                   record setup, event-record sessions
          └─ mediakit pull     MediaPlayer / proxy player pulls RTSP/RTMP from the device
              └─ MultiMediaSourceMuxer
                  ├─ RtspMediaSource / RtmpMediaSource / HlsMediaSource / FMP4 / TS
                  └─ recorder sinks (see flow 2)
```

Key points:
- `StreamOption` (in `StreamSource.h`) is the full per-stream config; it has `operator==`
  so the manager can detect a real change and restart only then.
- Everything on a `StreamSource` after construction runs on its poller. `startEventRecord`
  / `stopEventRecord` dispatch asynchronously, which is why `hasActiveEventSession()` also
  consults an in-flight flag.
- Output protocols are fanned out by `MultiMediaSourceMuxer`. Never push a frame directly
  into one protocol's source.

## 2. Recording

Two recorders coexist:

- **`src/Record/`** — the upstream ZLMediaKit recorders: `MP4Recorder` writes plain MP4,
  `HlsMaker*`/`HlsRecorder` write HLS, `MP4Reader`/`MergedMP4Reader` replay them.
- **`manager/Local/Time*`** — the VMS time-indexed store used for continuous recording and
  playback:
  - `TimeRecorderManager` → `TimeRecorder` per stream, driven by `RecordPolicy` and
    `TimeScheduler` (schedules, retention).
  - `TimeMaker`/`TimeMuxer` write segments; `TimeFile` is the on-disk unit;
    `TimeFileAccessManager` arbitrates readers/writers; `TimeFileDirtyTracker` marks
    segments needing rebuild; `TimeRebuilder` repairs indexes.
  - `TimeQuery` + `SearchEngine` answer playback/timeline queries; `StatusGapSearch` and
    `MediaGapFillManager` reconcile gaps (including SD-card sync, see `docs/sd-card-sync-flow.md`).
  - Event/motion clips go through `EventRecordSession` with pre/post roll.

Playback path is documented in `docs/playback-flow.md`.

## 3. Tiering (local → NAS/S3)

```
StoragePolicy / PolicyAssignment (Storage entities)
  └─ TierStorageManager           schedules TieringJob rows
      ├─ TierLocalDiskStorage
      ├─ TierNASStorage
      └─ TierObjectStorage        AWS SDK (ENABLE_AWS_SDK), gated by ENABLE_TIER_STORAGE
          └─ TierBackfillTask     backfills existing recordings
```

Restore jobs and their client contract: `docs/api-storage-tiering.md`,
`docs/restore-job-client-integration.md`, `docs/storage-tiering-flow.md`.
All of this is blocking I/O — it runs on `WorkThreadPool`, not on a poller.

## 4. REST request

```
HttpSession (src/Http)
  └─ installWebApi() routing table (server/WebApi.cpp)
      └─ handler registered by some registerXxxApis() in server/WebApi<Group>.cpp
          ├─ CHECK_AUTH_TOKEN()      JWT (skipped on internal sync APIs by design)
          ├─ permission guard
          ├─ CHECK_ARGS_(...)        required parameters
          ├─ work: managerkit *Imp / mediakit calls
          └─ val["data"] = ...       or RETURN_API_RESPONSE(ApiErrCode::CODE_*, msg)
```

Async variants (`API_ARGS_*_ASYNC`) capture an `HttpResponseInvoker`; the response may be
delivered from a work thread. Exactly one `invoker(...)` per request.

## 5. Persistence

```
caller (manager/service/WebApi)
  └─ <Entity>Imp            validation, defaults, generated ids, timestamps,
      │                     JSON ↔ struct, state transitions
      └─ <Entity>Repository parameterized SQL, row mapping, transactions
          └─ toolkit::SqlitePool / SqliteTransaction
              └─ DbStorage: kMediaServerDb | kEdgeStorageControllerDb
```

Schema evolves only by adding a new numbered file under `migration/updates/` (ESC) or
`migration/mserver_updates/` (MediaServer); `MigrationHistory` records what has been applied.

## 6. Multi-node sync (`ENABLE_SYNC_CHANGES`)

```
local write → TransactionLog row (op, table, payload, sequence)
                    │
SyncManager::onTick() every tick:
   ├─ push local log to peers / pullFromRelay(peer)
   ├─ applyBatch(rows)     → per-table TableSyncHandler::onUpsert/onUpsertBatch/onDelete
   ├─ bootstrap path:      needsBootstrap() → doBootstrap() → applySnapshot() → onSnapshot
   ├─ recordRelayAck()     → TransactionPeerAckLog
   └─ maybePruneLog()      trims acknowledged history
```

- A table joins sync by implementing `makeSyncHandler()` on its `Imp` and calling
  `SyncManager::registerTable()` at startup — no `if/else` chain to edit.
- `TableSyncHandler::rowKey` provides the last-writer-wins key; returning `""` means
  "always apply".
- `*WithTxn` variants are used when a batch runs inside one `SqliteTransaction`.
- Conflict handling is LWW over the row key, so payloads must carry their own timestamps;
  stale rows are dropped before the write.
- Only ESC tables participate in the current design. Internal, node-local statistics are
  deliberately excluded from replication.

Protocol details: `docs/api-sync.md`, endpoints in `server/WebApiSyncDb.cpp`.

## 7. Health and cluster

`installGlobalMonitor()` (`server/Manager.cpp`) starts `GlobalMonitor::Instance()`, which
drives `CpuMonitor`, `MemoryMonitor`, `HddMonitor`, `NetworkMonitor`, `ResourceMonitor` and
`ReaderMonitor`. `ClusterManager` tracks peers and resource assignment; `RestartScheduler`
handles planned restarts. Monitor output feeds `SystemMetrics` rows and the monitor APIs.
