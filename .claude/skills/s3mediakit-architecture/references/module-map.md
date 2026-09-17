# Module map

Directory-level ownership. Paths are the authoritative answer to "where does this live?".

## `src/` — `mediakit` core

| Dir | Contents |
|---|---|
| `Common/` | `MediaSource`, `MediaSink`, `MultiMediaSourceMuxer`, `Stamp` (timestamp correction), `Device`, `Parser`, `config.{h,cpp}` (config keys + defaults), `macros.h` (`CHECK`/`CHECK_RET`) |
| `Extension/` | `Frame`, `Track`, `Factory` (codec registry), `CommonRtp`/`CommonRtmp` base adapters |
| `Rtsp/` `Rtmp/` `Rtp/` `Http/` `TS/` `FMP4/` `Mjpeg/` | Per-protocol sessions, demuxers, muxers, media sources |
| `Record/` | `MP4Recorder`, `MP4Reader`, `MergedMP4Reader`, `HlsMaker*`, `HlsMediaSource`, `Recorder`, `EventRecordSession` |
| `Player/` `Pusher/` | Client-side pull/push (`MediaPlayer`, `MediaPusher`, proxy players) |
| `Codec/` `Transcode/` | Encode/decode and transcoding helpers (ffmpeg-backed) |
| `Motion/` | Motion detection pipeline (gated by `ENABLE_MOTION`) |
| `Onvif/` | ONVIF discovery/protocol helpers used by device control |
| `Rtcp/` `Ssdp/` `Shell/` `Processor/` | RTCP feedback, SSDP discovery, telnet shell, frame processors |

`webrtc/` and `srt/` are separate top-level directories, gated by `ENABLE_WEBRTC` / `ENABLE_SRT`.

## `ext-codec/` — codec plugins

H264, H265, AAC, G711, Opus, JPEG and friends. Each plugin supplies track/frame metadata,
config parsing and RTP/RTMP adapters, and registers itself through `src/Extension/Factory.cpp`.
Nothing here should own cross-protocol pipeline policy.

## `manager/` — `managerkit`

| Dir | Contents |
|---|---|
| `Camera/` | `CameraManager`, `CameraController`, `StreamSource`/`StreamSink` (device → media pipeline), `GenericRtspCameraImp`, `RecordPolicy`, `CameraStatistic` |
| `Speaker/` | IP speaker equivalents: `SpeakerManager`, `SpeakerController`, `AudioFileManager` |
| `Control/` | `OnvifDeviceControl` (PTZ, profiles), `SubnetScan` (device discovery) |
| `Local/` | The recording + storage engine. `Time*` family = time-indexed record store (`TimeRecorder`, `TimeRecorderManager`, `TimeFile`, `TimeMaker`, `TimeMuxer`, `TimeDemuxer`, `TimeQuery`, `TimeRebuilder`, `TimeScheduler`, `TimeFileAccessManager`, `TimeFileDirtyTracker`). `Tier*` family = tiering (`TierStorageManager`, `TierObjectStorage`, `TierNASStorage`, `TierLocalDiskStorage`, `TierFileStorageBase`). Plus `StorageManager`, `SearchEngine`, `MotionSearch`/`MotionBlockManager`, `StatusGapSearch`, `MediaGapFillManager`, `ExtractJobManager`, `StatisticRecorder` |
| `Server/` | Health and cluster: `GlobalMonitor` (entry point), `CpuMonitor`, `MemoryMonitor`, `HddMonitor`, `NetworkMonitor`, `ResourceMonitor`, `ReaderMonitor`, `ClusterManager`, `RestartScheduler` |
| `Storage/` | SQLite entities. One header per table, each holding the row struct, a `*Repository` (SQL) and a `*Imp` (domain). `DbSchema.h` has `Optional<T>` and shared schema types; `DbStorage.{h,cpp}` holds DB handles (`kMediaServerDb`, `kEdgeStorageControllerDb`); `MigrationHistory` applies numbered migrations |
| `User/` | `UserAuthorManager` (auth/permissions), `UserSessionCache`, `UserAuditLog` |
| `Extension/` | `SyncManager` + `TableSyncHandler` (multi-node replication), `Resource`, `Plugin`, `Benchmark` |
| `Common/` | `DeviceSource`, `DeviceOption`, `DeviceControl`, `StrUtil` |
| `proto/` | Protobuf/IDL definitions shared with peers |

## `server/`

| File | Role |
|---|---|
| `main.cpp` | Config defaults, logging setup, SSL, port binding, starts servers, calls `installGlobalMonitor()` and `installWebApi()` |
| `Manager.{h,cpp}` | Glue between `managerkit` and the media pipeline; JSON option mapping (`makeCameraOptionJson`, `makeSpeakerOptionJson`), lifecycle helpers |
| `WebApi.{h,cpp}` | Handler macros, auth guards, error helpers, `installWebApi()` calling every `registerXxxApis()` |
| `WebApiConfig/Monitor/Extract/Motion/Playback/Bookmark/SyncDb/Control/Storage/SdCardSync.cpp` | Endpoint groups |
| `WebApiErrCode.{h,cpp}` | `ApiErrCode::CODE_*` values and messages |
| `WebHook.{h,cpp}` | Outgoing hook notifications |
| `Process.cpp`, `System.cpp`, `ShellParser.h` | Child-process and OS helpers |
| `FFmpegSource.{h,cpp}` | Pull-via-ffmpeg source |
| `VideoStack.{h,cpp}` | Video wall composition (gated by `ENABLE_VIDEOSTACK`) |

## `api/`

`include/mk_*.h` is the frozen C ABI (`mk_common`, `mk_media`, `mk_player`, `mk_pusher`,
`mk_track`, `mk_frame`, `mk_recorder`, `mk_thread`, `mk_transcode`, `mk_webrtc`, …);
`source/` implements it over `mediakit`. Changing a header signature breaks CGo
(`golang/`) and Android consumers — treat it as a public API change.

## `migration/`

- `updates/` → Edge Storage Controller database (`kEdgeStorageControllerDb`)
- `mserver_updates/` → MediaServer database (`kMediaServerDb`)

Files are `NN_snake_case_description.sql`, applied in numeric order by `MigrationHistory`.

## `tests/`

- `unit/` — GTest suites registered in `tests/unit/CMakeLists.txt` via `add_s3_unit_test`,
  discovered into ctest with the `unit.` prefix. Subdirs: `common codec console extension
  http manager onvif record rtcp rtmp rtsp server storage`.
- `integration/tier_storage/` — process-isolated tier-storage and MinIO integration programs.
- `performance/` — benchmarks.
- Top-level `test_*.cpp` — legacy stand-alone demo programs, built only with
  `ENABLE_LEGACY_TESTS=ON`.
