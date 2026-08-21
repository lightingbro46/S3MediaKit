---
name: codebase-exploration
description: Explore and explain an existing codebase before design, implementation, debugging, or review. Use when the agent needs to understand current architecture, execution flow, entry points, callers, callees, data models, thread boundaries, configuration, dependencies, or existing tests. This is a read-only investigation skill and must not modify production source code.
---

# Codebase Exploration

Act as a senior engineer investigating an unfamiliar codebase.

## Goal

Build an evidence-based understanding of the relevant implementation before proposing changes.

This workflow is READ ONLY.

Do not modify production source code.

## Workflow

### 1. Understand the investigation target

Identify:

- feature
- behavior
- subsystem
- protocol
- API
- class
- function

Do not explore the entire repository unless necessary.

### 2. Find entry points

Search for likely entry points such as:

- API controller
- command handler
- event handler
- protocol handler
- service
- scheduler
- callback
- main function

Record exact files and symbols.

### 3. Trace execution flow

Follow:

entry point
→ caller/callee
→ service
→ state
→ output

Inspect both callers and callees where relevant.

Avoid reasoning from a single isolated function.

### 4. Identify important types

Find relevant:

- classes
- interfaces
- structs
- enums
- configuration objects
- database entities
- messages
- events

Explain their responsibilities.

### 5. Trace data flow

Identify:

input
→ transformation
→ storage/state
→ output

For media pipelines also consider:

packet
→ demux
→ decode
→ transform
→ encode
→ mux
→ transport

Only include stages that actually exist.

### 6. Identify ownership and lifecycle

For C++ investigate:

- object creation
- ownership
- destruction
- shared_ptr
- weak_ptr
- raw pointers
- callbacks
- async lifetime

### 7. Identify thread boundaries

Look for:

- thread creation
- event loops
- thread pools
- async callbacks
- mutex
- condition_variable
- atomics
- queues

Describe where execution moves between threads.

### 8. Identify external dependencies

Find relevant:

- libraries
- services
- protocols
- databases
- files
- network endpoints

### 9. Identify configuration

Locate configuration affecting behavior.

Record:

- config file
- environment variable
- constant
- runtime option

### 10. Identify existing tests

Search for:

- unit tests
- integration tests
- mocks
- test utilities

Explain what behavior is already covered.

### 11. Identify extension points

Look for:

- interfaces
- factories
- strategies
- plugins
- callbacks
- event buses
- existing patterns

Prefer extending existing architecture over introducing parallel mechanisms.

## Output

# Codebase Exploration

## Investigation Target

## Entry Points

## Execution Flow

Use a concise flow such as:

A
→ B
→ C
→ D

## Important Components

For each component:

- file
- symbol
- responsibility

## Data Flow

## Ownership / Lifecycle

## Thread Model

## Configuration

## External Dependencies

## Existing Tests

## Extension Points

## Important Findings

## Unknowns

Clearly distinguish:

CONFIRMED
INFERRED
UNKNOWN

## Recommended Next Step

Recommend one of:

- requirement-analysis
- architecture-design
- implementation-planning
- bug-investigation

Do not implement changes.