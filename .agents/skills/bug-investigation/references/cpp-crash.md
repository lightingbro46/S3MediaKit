# C++ Crash Investigation Reference

Use this reference for:

- SIGSEGV
- SIGABRT
- stack smashing
- heap corruption
- double free
- use-after-free
- invalid free
- intermittent native crashes

## Principle

Do not patch the line that crashes until the corrupted state and ownership path are understood.

The crash site is often only where memory corruption becomes visible.

## First evidence to collect

Capture:

- signal
- PID/TID
- executable build
- git commit/version
- stack trace
- all thread stacks
- core dump if available
- recent logs
- runtime arguments/configuration

Useful GDB commands:

```gdb
bt
bt full
info threads
thread apply all bt
thread apply all bt full
```

For optimized binaries, ensure debugging symbols are available when possible.

## Core dump workflow

Typical flow:

```bash
coredumpctl list
coredumpctl info <PID-or-executable>
coredumpctl gdb <PID-or-executable>
```

or:

```bash
gdb /path/to/executable /path/to/core
```

Inside GDB:

```gdb
set pagination off
info threads
thread apply all bt full
```

## addr2line

For raw addresses:

```bash
addr2line -Cfpie /path/to/binary 0xADDRESS
```

Use the exact binary matching the crashing build.

With shared libraries, account for relocation/base addresses when necessary.

## Sanitizers

Prefer AddressSanitizer for reproducible memory bugs.

Typical flags:

```text
-fsanitize=address
-fno-omit-frame-pointer
-g
```

Useful for:

- heap use-after-free
- stack use-after-return
- double free
- buffer overflow

UndefinedBehaviorSanitizer:

```text
-fsanitize=undefined
```

Useful for:

- signed overflow
- invalid shifts
- invalid casts
- alignment
- other UB

ThreadSanitizer:

```text
-fsanitize=thread
```

Useful for data races, but expect significant runtime overhead and library compatibility considerations.

## Stack smashing

Message:

```text
*** stack smashing detected ***
```

usually indicates stack-canary corruption.

Investigate:

- local arrays
- `memcpy`
- `memmove`
- `sprintf`
- `strcpy`
- incorrect buffer lengths
- structure size mismatch
- writing through invalid pointers

Do not assume the function printing the message caused the overwrite.

Search backward from where the damaged stack object may have been modified.

## Heap corruption

Symptoms may include:

- `malloc(): corrupted top size`
- `double free or corruption`
- crash inside `free`
- crash inside allocator internals

Suspect:

- buffer overrun
- use-after-free
- double free
- mismatched alloc/free
- concurrent mutation

Allocator crash sites are often secondary symptoms.

## Use-after-free investigation

Trace:

1. allocation/creation
2. ownership
3. registration with callbacks/workers
4. destruction
5. callback execution after destruction

Common media-server pattern:

```text
session destroyed
    ↓
worker queue still contains callback
    ↓
callback captures raw this
    ↓
use-after-free
```

Inspect cancellation and queue draining.

## shared_ptr investigation

Check:

- object created with `shared_ptr`
- callbacks capture strong self-reference
- cycles prevent destruction
- weak_ptr locking
- raw pointer extracted then retained beyond shared lifetime

For classes using `enable_shared_from_this`, verify object is actually owned by a `shared_ptr` before `shared_from_this()`.

## Iterator/reference invalidation

Crashes can come from:

- vector growth
- erase
- rehash
- container mutation by another thread

Trace whether stored references/pointers remain valid after mutations.

## Concurrent destruction

For intermittent crashes inspect:

- stop flag
- callbacks
- worker queue
- thread join
- unregister
- object destructor

A destructor must not tear down state still accessible by another thread.

## Virtual calls

Potential crash patterns:

- callback into partially destroyed object
- virtual call during destruction
- dangling base pointer
- ABI mismatch

## Binary/library mismatch

If crash occurs in library boundaries inspect:

- ABI version
- struct definitions
- compile flags
- linked library version
- runtime-loaded library version

Check:

```bash
ldd /path/to/executable
```

## Reproduction strategy

Reduce variables:

- same input
- same camera/stream
- same reconnect sequence
- same thread count
- same build

Run with sanitizer if feasible.

Automate reproduction loops when crash is probabilistic.

## Root-cause confidence

CONFIRMED:

- sanitizer shows exact invalid access
- debugger demonstrates invalid object lifetime
- deterministic repro plus fix removes failure

MOST LIKELY:

- stack/lifetime evidence strongly supports cause but direct memory diagnostic is unavailable

UNKNOWN:

- crash stack only, no evidence of corruption source

Do not present allocator or crash-site stack frames as root cause without evidence.
