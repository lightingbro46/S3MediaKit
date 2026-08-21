# Performance Debugging Reference

Use this reference for:

- high CPU
- high load average
- high IO wait
- memory growth
- latency spikes
- frame drops
- slow streaming
- excessive disk/network use
- lock contention
- throughput regression

## Principle

Measure before optimizing.

Do not infer the bottleneck from a single metric.

Separate:

- CPU
- scheduler
- memory
- disk
- network
- lock contention
- application queueing

## Establish baseline

Record:

- software version
- workload
- camera/stream count
- codecs
- resolutions
- bitrate
- client count
- storage mode
- host CPU/RAM/disk/network

Performance comparisons are meaningless if workload changes.

## CPU

Start with:

```bash
top -H -p <pid>
```

or:

```bash
pidstat -p <pid> -t 1
```

Determine which threads consume CPU.

Then use:

```bash
perf top -p <pid>
```

or:

```bash
perf record -g -p <pid> -- sleep 30
perf report
```

Look for:

- decode/encode
- memcpy
- pixel conversion
- locks
- JSON/string formatting
- polling loops
- crypto
- excessive logging

## CPU interpretation

On Linux, process `%CPU` may exceed 100% when using multiple cores.

High CPU is not automatically a problem if throughput and latency targets are met.

Look for regressions relative to equivalent workload.

## Load average

Load average includes runnable tasks and tasks in uninterruptible sleep, often IO waits.

High load with low CPU may indicate storage or kernel IO pressure.

Use other metrics before concluding CPU saturation.

## IO wait

Inspect:

```bash
iostat -xz 1
```

Important fields commonly include:

- `%util`
- `await`
- `r/s`
- `w/s`
- `rkB/s`
- `wkB/s`

Also inspect:

```bash
pidstat -d -p <pid> 1
```

Possible issues:

- saturated disk
- high queue depth
- synchronous writes
- fsync frequency
- fragmented/full filesystem
- storage backend latency

## Disk capacity

Very full filesystems can behave worse depending on filesystem/workload.

Check:

```bash
df -h
df -i
```

Do not attribute IO latency solely to percentage used without measurements.

## Memory

Inspect:

```bash
pmap -x <pid>
cat /proc/<pid>/status
```

Track RSS over time.

For growth investigate:

- queues
- packet/frame references
- caches
- maps
- reconnect cleanup
- shared_ptr cycles
- unbounded buffers

Heap profilers or sanitizers can help when reproducible.

## Queueing

For media systems measure queue depth.

A consumer slightly slower than producer can cause:

```text
queue growth
→ memory growth
→ latency growth
→ eventual OOM
```

A live pipeline should usually have an explicit backpressure/drop policy.

## Network

Inspect:

```bash
ss -tinp
nload
iftop
sar -n DEV 1
```

Look for:

- retransmissions
- slow clients
- socket buffer growth
- bandwidth saturation
- many concurrent connections

One slow client must not block unrelated streams.

## Lock contention

Symptoms:

- CPU not fully utilized
- many threads
- low throughput
- threads sleeping/waiting

Use:

```bash
perf lock
```

when supported, or profiler call stacks.

Inspect long lock scopes and callbacks/IO under locks.

## Syscall analysis

Use:

```bash
strace -f -p <pid>
```

carefully in test environments.

Summary mode:

```bash
strace -f -c -p <pid>
```

Can reveal excessive:

- `read`
- `write`
- `futex`
- `poll`
- `epoll_wait`
- `fsync`

Be mindful of tracing overhead.

## Thread-level investigation

Map hot thread IDs:

```bash
top -H -p <pid>
```

Convert decimal TID to hex when matching profiler/debugger output if needed.

Inspect thread purpose from names or stacks.

## Media pipeline performance

Common expensive operations:

- H.265 → H.264 transcoding
- scaling
- pixel format conversion
- blur/pixelate filters
- per-user watermark encoding
- software decode
- frame copies
- packet cloning
- frequent allocations
- muxing/fsync patterns

Determine whether operation runs:

- per camera
- per stream
- per viewer
- per frame

A small per-frame cost becomes large at many cameras.

## FFmpeg filter graphs

Check whether filter graph is:

- initialized once
- reused
- recreated only on format change

Rebuilding graph per frame is highly expensive.

## Memory copies

Look for repeated:

```text
packet → temporary buffer
→ queue buffer
→ mux buffer
```

Measure before redesigning, but remove clearly redundant copies in hot paths.

## Logging overhead

High-frequency logging can affect:

- CPU
- disk IO
- lock contention

Avoid INFO/WARN logs per packet/frame in normal operation.

## Performance regression workflow

1. reproduce stable workload
2. measure baseline
3. measure regression
4. profile
5. identify dominant difference
6. change one factor
7. re-measure

Do not optimize multiple areas simultaneously; it destroys causal evidence.

## Example investigation

Symptom:

```text
live-view latency grows over time
```

Possible evidence:

```text
producer 25 fps
consumer 22 fps
queue grows continuously
```

Root cause:

```text
no backpressure/drop policy
```

Fixing CPU alone may not solve the architecture problem.

## Reporting

Always include:

- workload
- before
- after
- measurement tool
- time window
- bottleneck evidence
- confidence

Avoid claims like "this is slow" without measurements.
