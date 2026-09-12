---
alwaysApply: false
description: 当被要求生成plan时启用。
---
# Strict Planning Protocol (Agent Rule)

## 1. Core Constraint
You **MUST NOT** write any plan until you have thoroughly analyzed the existing source code.  
This rule is absolute. Any deviation will produce an invalid plan.

## 2. Analysis Strategy: Checkpoint-Driven Parallel Search

**Main Agent** must first perform a **high-level analysis** using its existing knowledge (e.g., architecture, module responsibilities, known interfaces) to identify **critical checkpoints** that require source-code verification. For each such checkpoint, the Main Agent **must dispatch a Search Agent sub-agent** to perform a **targeted, parallel search** of the relevant source files.

### 2.1 What constitutes a “checkpoint”?
- A public interface or function signature that the planned change will rely on.
- A call graph or dependency that must be confirmed (e.g., who calls this function?).
- The exact handling of a boundary condition (e.g., how does the code treat empty input?).
- The location and implementation of a specific class, method, or macro.
- Any statement in the plan that asserts “the current code does X” – that assertion must be verified by a Search Agent.

### 2.2 Search Agent responsibilities
Each Search Agent sub‑agent shall:
- Retrieve the **exact content** of the specified file(s) and relevant surrounding context (e.g., enclosing class, function body).
- Return a **compact citation** with file path, line range, and a short code snippet that supports the analysis.
- For dependencies, locate the definition of called functions and their signatures.

### 2.3 Parallelism & Coordination
- Multiple Search Agents **may run in parallel** for different checkpoints (e.g., verifying several separate interfaces at once).
- The Main Agent **must not** block the entire analysis on one search; it can continue with non-conflicting parts, but **must wait** for all search results before finalising the **Analysis Report**.

### 2.4 Prohibited behaviours
- **Do not** perform a full‑code‑base scan unless specifically required (e.g., for global refactoring impact analysis).
- **Do not** rely on memory or training data to describe current source code – always dispatch a Search Agent to verify.
- **Do not** skip a checkpoint because you assume it is trivial; if it affects the plan, it must be verified.

### 2.5 Output of the analysis phase
The Main Agent shall produce a structured **Analysis Report** that:
- Lists all checkpoints and the Search Agent findings for each (with citations).
- Summarises the current state of the relevant codebase (architecture, pain points, technical debt) based on these validated findings.
- Identifies any missing information that still requires further searching – if so, dispatch additional Search Agents before finalising.

## 3. Boundary Condition Coverage
After completing the checkpoint‑based analysis, explicitly list all identified boundary conditions. This list is mandatory and must cover:

- **Input Boundaries**: null, empty, oversized, malformed, injection attempts, type mismatches, unicode/special chars.
- **State Boundaries**: uninitialized state, disconnected, expired sessions, cold caches, race conditions (concurrency/reentrancy).
- **External Dependency Boundaries**: timeout, network failure, rate limiting, 4xx/5xx responses, slow responses, retry storms.
- **Resource Boundaries**: memory exhaustion, disk full, file descriptor limits, connection pool exhaustion.
- **Authorization Boundaries**: missing permissions, revoked tokens, cross-tenant access.
- **Business Logic Boundaries**: impossible timestamps, negative quantities, circular references, conflicting updates.

You **MUST** explain how the current code handles (or fails to handle) each boundary condition, and **every such statement must be backed by a Search Agent citation** from the analysis phase. If a condition is not relevant, justify its omission.

## 4. Plan Document Template
Once the Analysis Report is complete, produce the plan using **exactly this structure**:

### 4.1 Title & Metadata
- Plan Title
- Author/Agent
- Date
- Related modules/packages

### 4.2 Objectives
- 1–3 sentences summarising what the plan achieves and why.

### 4.3 Current State Summary (from validated analysis)
- Key findings (architecture, pain points, technical debt).
- Reference to the Analysis Report and relevant Search Agent citations.

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
- No placeholder text like “TODO” or “will be filled later”—all sections must be complete.

## 6. Enforcement
If the analysis or plan fails to meet any requirement above, you MUST flag it as a **Non-Compliant Plan** and request a revision. Never proceed to implementation with a non-compliant plan.