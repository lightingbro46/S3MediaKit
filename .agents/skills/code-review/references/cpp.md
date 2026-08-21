# C++ Code Review Reference

Use this reference when reviewing C or C++ production code.

## Review priorities

Review in this order:

1. correctness
2. undefined behavior
3. ownership and lifetime
4. resource safety
5. concurrency interaction
6. error handling
7. performance
8. maintainability

Do not spend review time on formatting that should be enforced by tooling.

## Ownership and lifetime

Inspect every pointer, reference, callback, and asynchronous operation.

Check for:

- dangling pointers
- use-after-free
- double free
- memory leaks
- references to temporary objects
- references invalidated by container mutation
- object destruction while callbacks are still registered
- `this` captured by asynchronous lambdas without lifecycle guarantees
- `shared_ptr` cycles
- incorrect `weak_ptr` promotion
- raw pointer ownership that is not documented
- ownership transfer hidden in helper functions

Prefer:

- RAII
- explicit ownership
- `unique_ptr` for single ownership
- `shared_ptr` only when ownership is genuinely shared
- `weak_ptr` to break ownership cycles or protect asynchronous callbacks

When a callback captures `this`, determine:

1. who stores the callback
2. how long it can remain registered
3. whether it may run after object destruction
4. whether unregister/cancel is guaranteed
5. whether weak ownership is required

## Resource management

Check all resources with lifecycle requirements:

- heap memory
- file descriptors
- sockets
- threads
- mutexes
- condition variables
- FILE handles
- database handles
- FFmpeg objects
- library-specific contexts

Prefer resource ownership bound to object lifetime.

Look for early returns that bypass cleanup.

## Undefined behavior

Check for:

- out-of-bounds access
- invalid pointer arithmetic
- invalid iterator use
- data races
- uninitialized reads
- signed integer overflow
- misaligned access
- use-after-move
- invalid downcasts
- returning references to locals
- modifying containers while iterating incorrectly

Treat undefined behavior as a correctness defect even when the current build appears stable.

## Integer and size handling

Pay special attention to:

- `int` vs `size_t`
- signed/unsigned comparisons
- narrowing conversions
- timestamp arithmetic
- byte counts
- multiplication before allocation
- integer overflow in duration/bitrate/size calculations

Prefer explicit checked conversions when truncation is possible.

## Move and copy semantics

Check whether expensive or ownership-sensitive types are copied unnecessarily.

Review:

- pass-by-value
- return-by-value
- `std::move`
- move-after-use
- accidental copies in range loops
- container insertion behavior

Do not recommend `std::move` mechanically.

Correctness and clarity are more important than speculative micro-optimization.

## STL container safety

Check:

- iterator/reference invalidation
- concurrent access
- vector reallocation
- erased elements still referenced elsewhere
- map access via `operator[]` when insertion is unintended
- stale indices after mutation

For shared containers, identify which thread owns mutation.

## Error handling

Check every operation that can fail.

Examples:

- allocation
- file IO
- socket IO
- parser calls
- decoder/encoder calls
- database calls
- thread creation
- library APIs

Look for:

- ignored return values
- partial initialization
- cleanup after failure
- misleading success states
- retry loops without limits
- error swallowing
- logs without enough context

Do not replace explicit error propagation with silent fallback unless fallback is intended behavior.

## Exception safety

If exceptions are used:

- verify destructors are safe
- avoid throwing from destructors
- inspect partially constructed state
- inspect lock/resource release
- ensure exception boundaries are intentional

If exceptions are not used by project convention, do not introduce them casually.

## API contracts

For changed interfaces inspect:

- ownership semantics
- nullability
- thread-safety expectations
- valid lifetime
- units
- time base
- buffer ownership
- whether returned references remain valid

Ambiguous contracts are a defect risk.

## Logging

Check that logs:

- identify the affected object/stream/camera where useful
- include actionable error context
- avoid secrets
- avoid per-frame/per-packet logging on hot paths
- use appropriate severity

Temporary debug logging should not remain after the issue is resolved.

## Performance-sensitive C++

On hot paths inspect:

- per-frame allocations
- repeated vector growth
- repeated string formatting
- unnecessary packet/frame copies
- shared_ptr churn
- excessive virtual dispatch only if proven material
- locks held across expensive work
- synchronous disk/network IO
- repeated parsing
- repeated codec/context construction

Prefer evidence-based performance findings.

## Media-server specific checks

For streaming or recording paths verify:

- no unexpected blocking on media threads
- packet/frame ownership is explicit
- callbacks do not outlive owners
- timestamps preserve required monotonicity
- reconnect resets appropriate state
- stream shutdown does not race with callbacks
- high-frequency code avoids excessive allocation/logging
- error paths do not leak decoder, encoder, muxer, socket, or file resources

## Review finding examples

Good finding:

> HIGH — `StreamSession::onPacket`: asynchronous callback captures raw `this`, while session destruction can occur before the worker queue drains. This creates a use-after-free path. Capture a `weak_ptr`, unregister the callback before destruction, or otherwise guarantee lifecycle.

Weak finding:

> Consider using modern C++ here.

Only report findings with concrete impact.
