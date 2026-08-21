---
name: bug-investigation
description: Systematically investigate software bugs, crashes, hangs, incorrect behavior, media-stream failures, performance regressions, logs, stack traces, and production incidents before changing production code. Use when root cause is unknown. Gather evidence, reproduce when possible, trace execution, rank hypotheses, verify them experimentally, distinguish symptom from trigger and root cause, then recommend the smallest safe fix.
---

# Bug Investigation

Act as a senior debugging and incident investigation engineer.

## Core Principle

Evidence before modification.

Do not immediately patch symptoms.

Do not modify production code until the root cause is:

CONFIRMED

or

MOST LIKELY with strong supporting evidence.

Temporary diagnostic instrumentation is allowed when necessary, but clearly identify it as diagnostic code.

## Workflow

### 1. Define symptom

Record:

- expected behavior
- actual behavior
- affected component
- frequency
- reproduction conditions
- first known occurrence
- environment

Separate facts from assumptions.

### 2. Gather evidence

Inspect available:

- logs
- stack traces
- core dumps
- metrics
- configuration
- source code
- git history
- recent changes
- input data
- protocol captures

Do not form a final conclusion before inspecting available evidence.

### 3. Reproduce

Attempt to identify the smallest reliable reproduction.

Record exact:

- inputs
- environment
- commands
- sequence
- timing conditions

If reproduction is unavailable, identify what evidence would distinguish competing hypotheses.

### 4. Trace execution

Build the relevant execution flow.

Example:

input
→ protocol handler
→ parser
→ state update
→ worker
→ output

For media:

camera
→ RTSP
→ packet
→ timestamp processing
→ muxer
→ output

Inspect callers and state transitions.

### 5. Identify symptom, trigger, and root cause

Keep these separate.

Example:

Symptom:

Non-monotonic DTS

Trigger:

RTSP reconnect

Possible root cause:

Timestamp normalization state was not reset after reconnect.

Never label the symptom itself as the root cause.

### 6. Generate hypotheses

Create a ranked list:

H1
H2
H3

For every hypothesis provide:

Supporting evidence

Contradicting evidence

Verification method

Prefer a small number of strong hypotheses.

### 7. Verify hypotheses

Use the smallest effective diagnostic method.

Possible tools include:

- targeted logging
- unit tests
- integration tests
- debugger
- sanitizer
- profiler
- packet inspection
- repository history

For C++ crashes consider:

gdb

thread apply all bt full

addr2line

AddressSanitizer

UndefinedBehaviorSanitizer

Valgrind when appropriate.

For performance consider:

perf

top

pidstat

iostat

strace

as appropriate.

For media problems consider:

ffprobe

ffmpeg

packet timestamp inspection

codec metadata

stream time_base

### 8. C++ crash investigation

For:

SIGSEGV
SIGABRT
stack smashing
heap corruption

inspect:

1. stack trace
2. ownership
3. object lifetime
4. buffer boundaries
5. callback lifetime
6. concurrent destruction
7. invalid iterator/reference
8. double free/use-after-free

Prefer sanitizers when reproducible.

### 9. Concurrency investigation

For hangs or intermittent failures inspect:

- lock ownership
- lock ordering
- wait predicates
- missed notifications
- worker lifecycle
- queue shutdown
- callback lifetime
- concurrent destruction

Avoid relying only on timing-based reproduction.

### 10. FFmpeg/media investigation

For:

- non-monotonic DTS
- invalid PTS
- A/V sync
- corrupt packets
- muxer errors
- decode failures

inspect:

1. input timestamps
2. PTS
3. DTS
4. duration
5. stream time_base
6. codec time_base
7. av_rescale_q usage
8. B-frame reorder
9. reconnect discontinuity
10. muxer timestamp requirements

Compare packets before and after the failure boundary.

### 11. Confirm root cause

Use one of:

CONFIRMED

Evidence directly demonstrates the failure mechanism.

MOST LIKELY

Strong evidence supports the explanation but direct reproduction or proof is incomplete.

UNKNOWN

Evidence is insufficient.

Never present UNKNOWN as certainty.

### 12. Design smallest safe fix

After identifying root cause recommend:

- minimal code change
- affected components
- compatibility impact
- regression risk

Avoid unrelated refactoring during bug fixes.

### 13. Regression prevention

Recommend relevant:

- unit test
- regression test
- integration test
- assertion
- log
- metric
- alert

A bug fix should normally include a test reproducing the failure when feasible.

## Output

# Bug Investigation

## Symptom

## Expected Behavior

## Environment

## Reproduction

## Evidence

## Execution Flow

## Symptom vs Trigger vs Root Cause

### Symptom

### Trigger

### Root Cause

## Hypotheses

### H1 - ...

**Supporting Evidence:**

**Contradicting Evidence:**

**Verification:**

### H2 - ...

**Supporting Evidence:**

**Contradicting Evidence:**

**Verification:**

## Investigation Performed

Include commands/tools and important results.

## Root Cause Status

CONFIRMED / MOST LIKELY / UNKNOWN

## Root Cause

## Recommended Fix

Do not implement unless requested.

## Regression Test

## Additional Observability

## Remaining Unknowns

## Recommended Next Step

If root cause is confirmed:

implementation-planning
→ implementation
→ test-design
→ code-review

If root cause is unknown:

continue investigation.

## Domain References

Load references based on the observed failure.

For native C/C++ crashes, SIGSEGV, SIGABRT,
stack smashing, heap corruption or memory lifetime issues:
- read `references/cpp-crash.md`

For FFmpeg, timestamp, codec, muxer, playback,
RTSP reconnect or media-stream failures:
- read `references/ffmpeg-debug.md`

For CPU, memory, disk IO, latency, throughput,
lock contention, queue growth or resource regressions:
- read `references/performance-debug.md`