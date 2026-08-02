---
alwaysApply: false
description: 当被要求生成plan时启用。
---
# Strict Planning Protocol (Agent Rule)

## 1. Core Constraint
You MUST NOT write any plan until you have fully analyzed the existing source code.
This rule is absolute. Any deviation will produce an invalid plan.

## 2. Mandatory Pre-Planning Analysis
Before drafting the plan, execute these steps and output the results as an **Analysis Report**:

- **Codebase Scan**: List all relevant files, directories, and their responsibilities.
- **Dependency Map**: Show imports, call graphs, data flow, and external dependencies (APIs, databases, configs).
- **Interface Inventory**: Enumerate every public function, class, method, API endpoint, and its contract (params, return types, side effects).
- **Business Logic Extraction**: Summarize the core behaviors and decision points found in the code.
- **State & Side Effects**: Identify mutable state, transactions, caches, file I/O, and event emitters.

If the codebase is large, you may sample key modules but must explicitly note the sampling scope and justify that the missing parts are irrelevant to the planned changes. An incomplete analysis invalidates the plan.

## 3. Boundary Condition Coverage
After the analysis, explicitly list all identified boundary conditions. This list is mandatory and must cover:

- **Input Boundaries**: null, empty, oversized, malformed, injection attempts, type mismatches, unicode/special chars.
- **State Boundaries**: uninitialized state, disconnected, expired sessions, cold caches, race conditions (concurrency/reentrancy).
- **External Dependency Boundaries**: timeout, network failure, rate limiting, 4xx/5xx responses, slow responses, retry storms.
- **Resource Boundaries**: memory exhaustion, disk full, file descriptor limits, connection pool exhaustion.
- **Authorization Boundaries**: missing permissions, revoked tokens, cross-tenant access.
- **Business Logic Boundaries**: impossible timestamps, negative quantities, circular references, conflicting updates.

You MUST explain how the current code handles (or fails to handle) each boundary condition found. If a condition is not relevant, justify its omission.

## 4. Plan Document Template
Once analysis is complete, produce the plan using **exactly this structure**:

### 4.1 Title & Metadata
- Plan Title
- Author/Agent
- Date
- Related modules/packages

### 4.2 Objectives
- 1–3 sentences summarizing what the plan achieves and why.

### 4.3 Current State Summary (from analysis)
- Key findings (architecture, pain points, technical debt).
- Link to the detailed Analysis Report section.

### 4.4 Proposed Changes
- For each change: **What** (precise description), **Where** (file/path + line ranges if possible), **Why** (reason linked to objective or boundary condition).
- Include a migration strategy if data/schema changes exist.

### 4.5 Impact Analysis
- Affected components, APIs, and contracts.
- Breaking changes clearly marked with `⚠️ BREAKING`.
- Upgrade/downgrade compatibility notes.

### 4.6 Boundary Condition Handling Strategy
A table that maps each boundary condition from §3 to the planned mitigation:

| Boundary Condition          | Current Handling              | Planned Handling                | Test Strategy                  |
| --------------------------- | ----------------------------- | ------------------------------- | ------------------------------ |
| (e.g., empty input array)   | (e.g., crashes with NPE)      | (e.g., return empty list)       | (e.g., unit test + fuzz)       |

### 4.7 Test Plan
- Unit tests (list concrete test cases).
- Integration tests (scenarios, environment setup).
- Edge-case specific tests (each boundary condition mapped to a test).
- Regression risk areas and how they will be guarded.

### 4.8 Implementation Steps (Ordered)
- Atomic, verifiable steps. Each step states its expected outcome and required approvals.
- Rollback step for each critical change.

### 4.9 Risks & Mitigations
- Known unknowns, assumptions, and fallback plans.

## 5. Formatting & Readability Requirements
- Use **Markdown** throughout, with proper headings, bullet lists, and code blocks.
- Code references must use `file.ts:42` style.
- Complex flows can be illustrated with Mermaid.js diagrams (optional but encouraged).
- The document must be self-contained; a human and another agent should be able to understand it without external context.
- No placeholder text like "TODO" or "will be filled later"—all sections must be complete.

## 6. Enforcement
If the analysis or plan fails to meet any requirement above, you MUST flag it as a **Non-Compliant Plan** and request a revision. Never proceed to implementation with a non-compliant plan.