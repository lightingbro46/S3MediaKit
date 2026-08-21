---
name: requirement-analysis
description: Analyze software feature requests, user stories, product requirements, and technical requirements before design or implementation. Use when a new feature or change request needs clarification, scope definition, functional and non-functional requirements, edge cases, dependencies, risks, or acceptance criteria. Do not implement code while using this skill unless explicitly requested.
---

# Requirement Analysis

Act as a senior software analyst and software architect.

## Goal

Transform an informal feature request into a precise, testable engineering requirement.

Do not start implementation during this workflow.

## Workflow

### 1. Understand the objective

Identify:

- business goal
- user goal
- expected behavior
- inputs
- outputs
- affected users
- affected subsystems

If the repository contains relevant existing behavior, inspect it before making assumptions.

### 2. Define scope

Separate:

- in scope
- out of scope
- future considerations

Avoid silently expanding the requested scope.

### 3. Identify affected components

Determine whether the change may affect:

- Media Server
- Backend API
- Web
- Desktop
- Mobile
- Database
- Storage
- Network protocols
- External integrations
- Deployment
- Configuration

Only include components supported by evidence or reasonable architectural impact.

### 4. Functional requirements

Create explicit requirements:

FR-001
FR-002
FR-003

Each requirement must be:

- testable
- unambiguous
- implementation-independent where possible

### 5. Non-functional requirements

Consider:

- latency
- throughput
- CPU
- memory
- disk IO
- network bandwidth
- scalability
- availability
- security
- backward compatibility
- observability
- maintainability

Create:

NFR-001
NFR-002
...

Do not invent numeric targets that were not provided.

Mark missing targets as open questions.

### 6. Edge cases

Consider relevant cases involving:

- empty input
- invalid input
- duplicate requests
- timeout
- reconnect
- network interruption
- partial failure
- process restart
- concurrent operations
- stale state
- resource exhaustion

Do not list irrelevant edge cases merely to make the analysis longer.

### 7. Dependencies

Identify:

- internal modules
- APIs
- database schemas
- configuration
- third-party libraries
- external services
- protocols

### 8. Risks

Classify meaningful risks as:

CRITICAL
HIGH
MEDIUM
LOW

For each risk explain:

- cause
- impact
- mitigation

### 9. Open questions

Only ask questions that materially affect:

- behavior
- architecture
- compatibility
- performance
- security
- implementation

If repository evidence can answer the question, inspect the repository first.

### 10. Acceptance criteria

Create testable acceptance criteria.

Prefer Given / When / Then where appropriate.

Example:

Given a camera is online
When an authorized user requests live view
Then the stream should start successfully within the configured latency target.

## Output

# Requirement Analysis

## Objective

## Scope

### In Scope

### Out of Scope

## Affected Components

## Functional Requirements

## Non-functional Requirements

## Edge Cases

## Dependencies

## Risks

## Open Questions

## Acceptance Criteria

## Recommended Next Step

Recommend `codebase-exploration` when understanding the current implementation is required.

Recommend `architecture-design` when architectural decisions are required.