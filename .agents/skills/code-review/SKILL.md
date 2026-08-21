---
name: code-review
description: Review source-code changes for correctness, regression risk, memory and resource safety, concurrency, security, performance, API compatibility, maintainability, and missing tests. Use for git diffs, pull requests, merge requests, or completed implementation work. Focus on concrete defects and meaningful risks rather than formatting or subjective style preferences. Do not modify code unless explicitly requested after the review.
---

# Code Review

Act as a senior engineer performing an independent code review.

## Goal

Identify concrete defects and meaningful regression risks.

Do not manufacture findings to make the review appear thorough.

This workflow is READ ONLY unless the user explicitly requests fixes after reviewing findings.

## Workflow

### 1. Understand intent

Before reviewing implementation understand:

- requirement
- intended behavior
- architecture
- implementation plan

If unavailable infer intent carefully from:

- git diff
- surrounding code
- tests

### 2. Inspect change scope

Inspect:

git status

git diff

and when appropriate:

git diff --stat

Identify:

- modified files
- added files
- deleted files
- API changes
- configuration changes
- schema changes

### 3. Inspect surrounding code

Do not review only changed lines.

Inspect:

- callers
- callees
- ownership
- lifecycle
- related state
- existing tests

### 4. Correctness

Check:

- incorrect conditions
- invalid assumptions
- boundary errors
- state transition errors
- missing validation
- undefined behavior
- error handling

### 5. Resource safety

Check relevant:

- memory
- file descriptors
- sockets
- threads
- locks
- transactions
- temporary resources

Ensure cleanup occurs on failure paths.

### 6. Concurrency

When relevant inspect:

- race conditions
- deadlocks
- lock ordering
- lock scope
- callbacks under lock
- blocking operations under lock
- object lifetime
- shutdown races

### 7. Performance

For performance-sensitive paths check:

- unnecessary allocations
- unnecessary copies
- excessive locking
- blocking IO
- repeated expensive computation
- algorithmic complexity

Do not report speculative micro-optimizations.

### 8. Compatibility

Check:

- API compatibility
- protocol compatibility
- database compatibility
- configuration compatibility
- serialized data compatibility

### 9. Security

When relevant inspect:

- authentication
- authorization
- input validation
- injection
- path handling
- sensitive logging
- unsafe deserialization

### 10. Error handling

Check:

- ignored errors
- incomplete cleanup
- invalid retries
- misleading logs
- swallowed exceptions
- partial initialization

### 11. Tests

Determine whether important behavior is covered.

Look for missing:

- regression tests
- boundary tests
- failure tests
- concurrency tests
- integration tests

Do not demand tests for trivial code without meaningful risk.

### 12. Domain-specific references

When reviewing C++ load relevant C++ guidance if available.

When reviewing concurrency load concurrency guidance.

When reviewing FFmpeg or media pipeline code load FFmpeg guidance.

When reviewing backend, frontend, database, or other domain code load the corresponding reference if available.

### 13. Severity

Use:

CRITICAL

Security compromise, memory corruption, data corruption, major deadlock, catastrophic production failure.

HIGH

Race condition, crash path, significant functional failure, major resource leak.

MEDIUM

Meaningful edge-case failure, compatibility issue, performance problem, weak error handling.

LOW

Concrete maintainability problem or low-impact defect.

Do not use severity for formatting preferences.

### 14. Finding format

Every finding must contain:

- severity
- file
- symbol or line
- problem
- impact
- reasoning
- recommended fix

Prefer precise findings over long explanations.

## Output

# Code Review

## Summary

## Findings

### [HIGH] Short finding title

**File:**

**Symbol:**

**Problem:**

**Impact:**

**Reasoning:**

**Recommended Fix:**

## Missing Tests

## Performance Observations

Only when relevant.

## Concurrency Observations

Only when relevant.

## Compatibility Observations

Only when relevant.

## Final Assessment

Choose one:

APPROVE

APPROVE WITH MINOR CHANGES

REQUEST CHANGES

## Review Confidence

HIGH
MEDIUM
LOW

Explain uncertainty when confidence is not HIGH.

## Domain References

Load references only when relevant.

For C/C++ changes:
- read `references/cpp.md`

For multithreading, async callbacks, queues, mutexes,
condition variables, atomics, or worker lifecycle:
- read `references/concurrency.md`

For FFmpeg, AVPacket, AVFrame, codecs, muxing,
demuxing, filters, timestamps, PTS/DTS or media pipelines:
- read `references/ffmpeg.md`