---
name: test-design
description: Design, implement, improve, and run tests for new features, bug fixes, refactors, and behavior changes. Use when deciding appropriate unit, regression, integration, concurrency, protocol, or performance tests, or when existing test coverage is insufficient. Prefer behavior-based tests that prevent regressions rather than tests created only to increase coverage.
---

# Test Design

Act as a senior software engineer responsible for test strategy and regression prevention.

## Goal

Create the smallest effective test suite that proves the intended behavior and prevents likely regressions.

Coverage percentage is not the primary goal.

## Workflow

### 1. Understand changed behavior

Inspect:

- requirement
- implementation
- git diff
- public interface
- callers
- existing tests

Identify observable behavior.

### 2. Identify risk

Classify relevant risks:

- correctness
- boundary behavior
- state transition
- concurrency
- compatibility
- protocol
- performance
- recovery

Use risk to determine test depth.

### 3. Select test levels

Choose only relevant levels.

#### Unit Test

Use for isolated:

- algorithms
- state machines
- validation
- conversions
- utility classes

#### Regression Test

Required for bug fixes whenever feasible.

The test should:

fail before the fix

and:

pass after the fix

#### Integration Test

Use when behavior depends on:

- multiple modules
- database
- filesystem
- network protocol
- media pipeline
- external process

#### Concurrency Test

Use for:

- queues
- worker threads
- condition variables
- shutdown
- callbacks
- shared state

#### Performance Test

Use only when performance is a requirement or regression risk.

### 4. Build test matrix

Consider relevant:

- happy path
- boundary values
- invalid input
- empty input
- duplicate input
- timeout
- reconnect
- partial failure
- restart
- concurrent access
- shutdown

Avoid irrelevant combinations.

### 5. Prefer behavioral tests

Test externally observable behavior.

Avoid excessive testing of:

- private implementation details
- exact internal call order
- mock interactions without behavioral value

### 6. Avoid fragile tests

Avoid:

- arbitrary sleep
- test-order dependency
- external network dependency without need
- random timing assumptions
- duplicated production algorithms

For concurrency prefer deterministic synchronization.

### 7. C++ tests

Follow the project's existing framework.

If GoogleTest is used, follow existing conventions.

For lifecycle-sensitive code consider:

- create/destroy
- repeated start/stop
- callback during shutdown
- queue shutdown
- worker termination

### 8. Test naming

Use behavior-oriented names.

Good:

ShouldRejectPacketWithInvalidTimestamp

ShouldResetTimestampStateAfterReconnect

ShouldWakeConsumerWhenFrameArrives

Bad:

Test1

TestFunction

### 9. Implement tests

Follow existing repository test structure.

Avoid creating new test infrastructure unless necessary.

### 10. Run focused tests

Run the smallest relevant test target first.

Record:

- command
- result
- failures

### 11. Run broader regression

When appropriate run the surrounding test suite.

Never claim success without execution.

## Output

# Test Report

## Behavior Under Test

## Risk Analysis

## Test Strategy

## Test Matrix

| Scenario | Level | Expected Result |
|----------|-------|-----------------|
| ... | Unit | ... |

## Tests Added

## Tests Modified

## Commands Run

## Results

## Remaining Coverage Gaps

## Exit Criteria

- critical behavior is tested
- bug fixes have regression tests when feasible
- tests are deterministic
- focused tests pass
- important integration risk is covered or explicitly documented