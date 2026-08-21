---
name: implementation-planning
description: Convert an approved requirement or architecture into a concrete repository-level implementation plan before editing code. Use for features, refactors, bug fixes, and cross-module changes where the files, symbols, dependencies, test strategy, or implementation sequence should be identified before coding. Do not modify production source code while creating the plan.
---

# Implementation Planning

Act as a senior engineer preparing an implementation plan.

## Goal

Convert an approved design or understood change into a minimal, ordered, repository-specific implementation plan.

This workflow is READ ONLY.

Do not modify production source code.

## Workflow

### 1. Confirm the objective

Summarize:

- requested behavior
- approved architecture
- constraints
- compatibility requirements

Do not redesign the feature unless the architecture is invalid or incomplete.

### 2. Inspect the repository

Verify actual:

- file names
- class names
- functions
- interfaces
- tests
- build targets

Never create a plan using invented repository paths.

### 3. Determine change boundaries

Identify:

Files to modify

Files to add

Files to delete

Prefer minimal changes.

Avoid unrelated refactoring.

### 4. Define implementation steps

Each step should contain:

- objective
- files
- symbols
- change
- dependencies
- expected behavior

Order steps so the repository remains understandable and preferably buildable throughout the change.

### 5. Identify interface changes

Explicitly list changes to:

- public C++ interfaces
- REST APIs
- database schemas
- events
- configuration
- network protocols

Mark compatibility impact.

### 6. Identify data changes

If data models change define:

- old format
- new format
- migration
- default values
- compatibility

### 7. Identify concurrency changes

When relevant specify:

- ownership
- synchronization
- thread boundaries
- queue behavior
- shutdown behavior

### 8. Define testing plan

Identify required:

- unit tests
- regression tests
- integration tests
- performance tests

Map important requirements to tests.

### 9. Define verification

Specify commands when known:

- build
- unit test
- integration test
- static analysis
- sanitizer

Do not invent commands if the repository does not provide evidence for them.

### 10. Identify risks

Highlight implementation risks such as:

- concurrency
- compatibility
- migration
- performance
- protocol changes
- state transitions

## Output

# Implementation Plan

## Objective

## Preconditions

## Change Summary

## Files to Modify

## Files to Add

## Implementation Steps

### Step 1

**Objective:**

**Files:**

**Symbols:**

**Change:**

**Verification:**

### Step 2

...

## Interface Changes

## Data / Schema Changes

## Concurrency Changes

## Compatibility

## Test Plan

## Verification Commands

## Risks

## Out of Scope

## Exit Criteria

- implementation scope is clear
- affected files are identified
- compatibility impact is understood
- test strategy is defined
- no unresolved architecture decision blocks implementation