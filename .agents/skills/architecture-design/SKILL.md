---
name: architecture-design
description: Design or review software architecture for significant features and cross-module changes. Use when choosing between implementation approaches, changing component boundaries, APIs, data flow, concurrency models, storage models, media pipelines, distributed-system behavior, or external integrations. Prefer running codebase-exploration first when the current implementation is not well understood.
---

# Architecture Design

Act as a senior software architect.

## Goal

Design the simplest architecture that satisfies the requirements while minimizing implementation, operational, and regression risk.

Prefer evidence from the repository over assumptions.

## Inputs

Use available:

- requirements
- codebase exploration
- existing architecture
- constraints

If important information is missing, state the uncertainty.

## Workflow

### 1. Understand current architecture

Before proposing changes identify:

- current components
- responsibilities
- interfaces
- execution flow
- data flow
- state ownership
- deployment topology

If this is unknown, recommend or perform codebase exploration first.

### 2. Define constraints

Identify relevant:

- performance constraints
- latency
- throughput
- CPU
- memory
- network
- storage
- compatibility
- technology constraints
- deployment constraints
- security
- operational constraints

Do not invent numeric constraints.

### 3. Identify design options

For meaningful architectural decisions produce at least:

Option A
Option B

Consider Option C when it represents a genuinely different architecture.

Do not manufacture meaningless alternatives.

### 4. Evaluate options

Compare:

- implementation complexity
- runtime complexity
- CPU
- memory
- network
- storage
- scalability
- availability
- failure behavior
- maintainability
- observability
- security
- backward compatibility
- migration effort
- testability

### 5. Analyze data flow

Describe:

source
→ processing
→ state
→ destination

Identify where data is:

- copied
- transformed
- persisted
- cached
- transmitted

### 6. Analyze concurrency

When relevant inspect:

- thread ownership
- thread boundaries
- queues
- locks
- callbacks
- blocking operations
- shutdown behavior
- object lifetime

Avoid introducing unnecessary shared mutable state.

### 7. Analyze failures

Consider relevant failures:

- timeout
- service unavailable
- partial failure
- network disconnect
- process restart
- duplicate events
- stale state
- resource exhaustion

Define expected recovery behavior.

### 8. Select recommended architecture

Choose a preferred option.

Explain:

- why it is preferred
- what trade-offs are accepted
- why alternatives were rejected

Do not stop at listing alternatives.

### 9. Define component boundaries

Identify:

- modules to modify
- modules to add
- interfaces to introduce
- interfaces to preserve
- API changes
- database changes
- configuration changes

### 10. Compatibility

Analyze:

- backward compatibility
- protocol compatibility
- data compatibility
- rolling upgrade concerns

### 11. Migration and rollback

When existing behavior changes define:

- migration strategy
- rollout strategy
- rollback strategy

### 12. Observability

Identify required:

- logs
- metrics
- traces
- alerts

for validating the architecture in production.

## Output

# Architecture Design

## Context

## Current Architecture

## Requirements and Constraints

## Design Options

### Option A

#### Description

#### Advantages

#### Disadvantages

#### Risks

### Option B

#### Description

#### Advantages

#### Disadvantages

#### Risks

### Option C

Only when meaningful.

## Comparison

## Recommended Architecture

## Rationale

## Data Flow

## Component Responsibilities

## Concurrency Model

## Failure Handling

## Compatibility

## Observability

## Migration Strategy

## Rollback Strategy

## Risks

## Recommended Next Step

Recommend `implementation-planning`.