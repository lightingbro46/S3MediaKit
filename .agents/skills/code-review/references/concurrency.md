# Concurrency Code Review Reference

Use this reference when reviewing multithreaded, asynchronous, queued, callback-based, or event-driven code.

## Core questions

For every shared object determine:

1. Which thread creates it?
2. Which thread reads it?
3. Which thread writes it?
4. Which synchronization primitive protects it?
5. Who owns its lifetime?
6. How does shutdown work?

If these answers are unclear, treat the design as high-risk.

## Data races

Check all shared mutable state.

A race exists when multiple threads access shared memory concurrently and at least one access writes without valid synchronization.

Inspect:

- plain booleans used as stop flags
- counters
- shared pointers
- container mutation
- cached state
- state machines
- callbacks
- lifecycle flags

Do not assume a variable is safe because reads/writes appear atomic on a specific CPU.

## Mutex review

Check:

- lock scope
- lock ordering
- recursive locking
- nested locks
- callbacks while locked
- IO while locked
- expensive computation while locked
- exception/early-return safety

Prefer minimal lock scope.

Avoid:

- network send under lock
- disk IO under lock
- invoking arbitrary user/library callbacks under lock
- waiting for another thread while holding unrelated locks

## Deadlocks

Look for:

- opposite lock acquisition order
- lock + callback cycles
- joining a worker while holding a lock the worker needs
- waiting on a condition while holding another required mutex
- recursive calls into the same subsystem

Document lock ordering when more than one mutex may be held.

## `recursive_mutex`

Treat `recursive_mutex` as a signal to inspect architecture carefully.

It can hide:

- re-entrant control flow
- unclear ownership
- callback cycles
- excessive lock scope

Do not recommend replacing it blindly; first understand why re-entry occurs.

## condition_variable

Correct usage normally follows:

```cpp
std::unique_lock<std::mutex> lock(mutex);
cv.wait(lock, [&] {
    return predicate();
});
```

Check:

- predicate exists
- predicate reads protected state
- state is changed before notification
- spurious wakeups are handled
- shutdown is part of the predicate
- wait is not based only on notification
- no lost wakeup assumption

Preferred pattern:

```cpp
{
    std::lock_guard<std::mutex> lock(mutex);
    queue.push(item);
}
cv.notify_one();
```

Notification may be inside or outside the lock depending on design, but correctness must depend on the state predicate, not notification timing.

## Shutdown

Shutdown is one of the highest-risk paths.

Inspect:

1. stop flag update
2. worker wakeup
3. queue draining policy
4. callback cancellation
5. thread join
6. destruction order

Common bugs:

- worker waits forever after stop
- callback runs after owner destruction
- thread is joined while a required mutex is held
- queue accepts work after shutdown begins
- worker accesses state already destroyed

## Atomics

Check whether an atomic is sufficient for the invariant.

An atomic protects the variable itself, not a multi-variable invariant.

Example risk:

```cpp
if (running.load()) {
    use(resource);
}
```

`resource` may be destroyed by another thread after `running` is read.

Check:

- atomic vs mutex suitability
- compound state transitions
- memory ordering
- publication of initialized objects

Prefer default sequential consistency unless weaker ordering is necessary and justified.

## Thread ownership

Prefer clear ownership models:

- one thread owns mutable state
- other threads communicate via message passing
- producers/consumers use queues
- lifecycle is managed by a clear owner

Shared mutable state across many threads should receive extra scrutiny.

## Queues

Review:

- bounded vs unbounded
- backpressure
- shutdown behavior
- producer after close
- consumer wakeup
- queue draining
- overflow/drop policy

For media systems, unbounded queues can turn transient slowness into memory exhaustion and high latency.

## Backpressure

Check what happens when consumer throughput is lower than producer throughput.

Possible policies:

- block producer
- drop newest
- drop oldest
- reduce quality
- disconnect slow consumer

The policy must be intentional.

For live media, unlimited buffering is usually dangerous because latency grows continuously.

## Asynchronous callbacks

For every callback inspect:

- callback owner
- registration lifetime
- target object lifetime
- cancellation
- execution thread
- whether callback may be concurrent
- whether callback may re-enter the component

Raw `this` capture is high risk unless lifecycle is guaranteed.

## Futures/promises/tasks

Check:

- abandoned futures
- broken promises
- exceptions stored but never observed
- waiting on worker thread from same worker pool
- deadlock due to limited executor threads

## Thread pools

Inspect:

- task may block
- task may wait for another task in same pool
- pool starvation
- queue growth
- shutdown semantics

Media processing should avoid blocking control-plane or IO pools with expensive decode/transcode work unless intentionally provisioned.

## Lock-free structures

Do not assume lock-free means correct or faster.

Inspect:

- memory ordering
- ABA problems
- object lifetime
- reclamation
- producer/consumer assumptions
- single-producer/single-consumer contract

For SPSC queues, verify there is actually only one producer and one consumer.

## Concurrency test expectations

Recommend deterministic tests for:

- startup
- shutdown
- repeated start/stop
- producer/consumer wakeup
- callback cancellation
- queue close
- concurrent destruction
- reconnect
- timeout

Avoid arbitrary `sleep_for`-based tests when explicit synchronization can be used.

## Severity guidance

CRITICAL/HIGH:

- use-after-free across threads
- deadlock
- data race causing corruption/crash
- worker lifecycle race
- unbounded queue under sustained load

MEDIUM:

- excessive lock contention
- weak shutdown handling
- avoidable blocking on critical path
- non-deterministic test coverage gap
