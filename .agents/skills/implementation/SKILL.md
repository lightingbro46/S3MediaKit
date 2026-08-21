---
name: implementation
description: Implement an approved software change in an existing repository. Use when requirements, current code behavior, and the implementation approach are sufficiently understood and the user wants source code changed. Follow the approved implementation plan when available, keep changes minimal, build and test affected components, and inspect the final diff before completion.
---

# Implementation

Act as a senior software engineer implementing an approved change.

## Goal

Implement the smallest correct change that satisfies the requirement.

Do not perform unrelated refactoring.

## Workflow

### 1. Understand the task

Before editing confirm:

- intended behavior
- implementation scope
- affected components
- compatibility constraints

Read the implementation plan when available.

### 2. Verify repository state

Inspect:

- relevant source files
- current implementation
- existing interfaces
- existing tests

Never blindly apply a plan if the repository has changed.

### 3. Preserve existing architecture

Prefer:

- existing abstractions
- existing interfaces
- existing utilities
- existing patterns

Avoid introducing duplicate mechanisms.

### 4. Implement incrementally

Make logically small changes.

After significant changes:

- inspect compilation impact
- inspect callers
- inspect tests

### 5. Avoid scope creep

Do not:

- rename unrelated code
- reformat unrelated files
- redesign unrelated components
- upgrade dependencies unnecessarily
- perform speculative optimization

If unrelated problems are discovered, report them separately.

### 6. Error handling

Handle meaningful failure paths.

Do not silently ignore:

- errors
- invalid state
- failed allocations
- failed IO
- failed external calls

Follow existing project error-handling conventions.

### 7. Concurrency

When touching concurrent code verify:

- ownership
- synchronization
- lock scope
- callback lifetime
- shutdown behavior

Avoid:

- blocking IO under locks
- calling unknown callbacks under locks
- unnecessary shared mutable state

### 8. Performance

For hot paths avoid unnecessary:

- allocations
- copies
- locking
- string conversions
- disk IO
- network IO

Do not micro-optimize cold paths without evidence.

### 9. Build

Build the smallest affected target first.

If successful, run broader builds when appropriate.

When compilation fails:

- understand the error
- fix the actual cause
- do not weaken type safety simply to compile

### 10. Test

Run relevant existing tests.

Then use `test-design` when additional tests are required.

Never claim tests passed unless they were actually executed.

### 11. Inspect final diff

Before completion inspect:

git diff

Check for:

- unrelated changes
- debug code
- temporary logs
- accidental formatting
- missing cleanup
- commented-out code
- generated files

### 12. Verify against requirement

Confirm each relevant acceptance criterion.

## Output

# Implementation Summary

## Changes Made

## Files Changed

## Important Design Decisions

## Build

Command:

Result:

## Tests

Command:

Result:

## Requirement Verification

## Known Limitations

## Follow-up Findings

List unrelated issues separately.

## Exit Criteria

Before declaring completion verify:

- implementation matches the requirement
- implementation follows the approved design
- affected target builds
- relevant tests pass
- no unrelated changes remain
- final diff has been inspected