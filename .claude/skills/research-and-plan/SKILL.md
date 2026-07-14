---
name: research-and-plan
description: Research a technical problem deeply, then interview the user about tradeoffs to produce an implementation plan. Use when facing a non-trivial design decision, performance problem, or architectural question. Triggers include "research", "research and plan", "explore options for", "how should we solve", or when the user wants to make an informed technical decision before writing code.
argument-hint: "<topic or problem description>"
---

# Research & Plan

Data-driven research followed by a structured tradeoff interview, producing an implementation plan.

```
FRAME -> SYNERGY-SCAN -> RESEARCH -> INTERVIEW -> GAP-RESEARCH -> YAGNI -> PLAN
```

This skill produces a plan and durable research artifacts. It **never modifies production code**. Implementation belongs in a separate session.

## HARD RULES

1. **Research before opinions.** Never present options to the user until agents have explored the solution space. Uninformed questions waste the user's time.
2. **Write incrementally.** After every AskUserQuestion response, immediately write what was learned to a file. Never accumulate more than one round without writing.
3. **Recommend when research points one way; mix prose with menus.** Bring ideas to the table. Use AskUserQuestion for decisions with clear options; use prose for nuanced follow-ups. A menu-only interview feels robotic.
4. **Check for synergies before researching.** Existing docs, code comments, and headers may already cover part of the problem. Don't re-research what's documented.
5. **Durable artifacts.** Research goes to `.claude/Documents/research/` (or the project's equivalent docs location). Interview decisions go to `.claude/Documents/interviews/<topic>-<date>/` with `_summary.md` as final synthesis. Both outlive the current task.
6. **No code changes.** This skill produces a plan, not an implementation.

---

## Phase 1: Frame the Problem

**Goal:** Understand the current state and define what needs solving.

1. Read project-level docs (`README.md`, `CLAUDE.md`, `AGENTS.md`, `CONTRIBUTING.md`, or equivalents) and a handful of relevant source files to understand the system being discussed.
2. If the problem is performance-related, suggest gathering measurements (profiles, benchmarks, traces) **before** planning. A plan without data is a guess.
3. Create the interview output directory:
   ```
   .claude/Documents/interviews/<topic>-<date>/
   ```
4. Write `_interview-index.md`:
   ```markdown
   # Interview: <Topic>
   **Date:** <today>
   **Status:** In Progress

   ## Context
   <Problem statement with data from code analysis>

   ## Themes Discovered
   (updated as interview progresses)

   ## Files Created
   (updated as files are written)
   ```

---

## Phase 1.5: Synergy Scan

**Goal:** Find what the project already knows before launching research agents. Prevents duplicate work and surfaces collisions with in-progress changes.

1. Search existing artifacts (research notes, design docs, ADRs, RFCs, prior interview folders) under `.claude/Documents/`, `docs/`, `design/`, or wherever the project keeps them.
2. Search code headers and comments for keywords related to the problem. Codebases often cite the techniques or papers they implement; follow those breadcrumbs.
3. Check the local source-control state for uncommitted or in-flight changes that overlap the area under discussion (e.g. `git status`, `git diff`, open branches, or the equivalent for the project's VCS).
4. Read 1-2 source files most likely affected, so you know the current architecture before proposing changes.
5. Write findings to `_interview-index.md` under a **Synergies** section:
   - Existing docs that apply (and what they already answer)
   - In-progress changes that overlap
   - Code systems this will touch
6. If a synergy eliminates the need for research (the answer is already documented or implemented), skip to Phase 3 and say so explicitly.

---

## Phase 2: Research (Parallel Agents)

**Goal:** Explore the solution space broadly before asking the user to decide.

1. Launch **2-3 research agents in parallel** (single message, multiple Agent tool calls).
2. Each agent gets a distinct search focus. Split the space so the agents do not overlap. Common axes:

   - **Theory / academic** — relevant papers, formal analyses, textbook techniques, known tradeoffs.
   - **Production / industry** — how mature systems, libraries, or large open-source projects have solved this. GDC/conference talks, postmortems, engineering blogs.
   - **Ecosystem / framework-specific** — how the project's stack (language, framework, runtime, platform) shapes the available options. Existing libraries, idiomatic patterns, integration points, threading/async model.

   For non-technical-design problems, adapt the axes — e.g. "competitor approaches" / "first-principles analysis" / "constraints from our codebase". The point is **breadth without overlap**.

3. Agents should:
   - Search the web for relevant papers, talks, RFCs, blog posts, and reference implementations.
   - Recursively follow references — if a source cites something relevant, search for that too.
   - Note concrete numbers (benchmarks, complexity, memory usage, latency) when available.
   - Be honest about uncertainty — flag when sources disagree or when no authoritative answer exists.

4. Compile findings into `.claude/Documents/research/<topic>.md`:
   ```markdown
   # Research: <Topic>
   **Date:** <today>

   ## Problem Statement
   <what we're trying to solve>

   ## Option Comparison
   | Option | Complexity | Risk | Quality / Fit | Integration Cost |
   |--------|-----------|------|---------------|-----------------|
   | ...    | ...       | ...  | ...           | ...             |

   ## Detailed Findings

   ### <Option 1>
   - Description
   - When it's best suited
   - Known limitations
   - References: <papers, talks, implementations, repos>

   ### <Option 2>
   ...

   ## Production References
   <How specific projects or teams solve this>

   ## Integration Considerations
   <How the project's stack, conventions, and constraints affect the choice>
   ```

---

## Phase 3: Interview (Tradeoff Discussion)

**Goal:** Walk through the options with the user, making decisions that shape the implementation.

### Opening Round
Use AskUserQuestion to establish:
- Scale / scope constraints (input size, target platform, budget, deadlines)
- Strategy preference (quick fix vs proper architecture, prototype vs production)
- Hard requirements the user already knows

### Adaptive Deepening
For each major decision point:
1. Present 2-4 concrete options informed by the research.
2. Include pros, cons, and relevant data from the research phase.
3. Use AskUserQuestion with descriptive options.
4. After the user answers, **immediately** write the decision to a file:
   - `scope-and-strategy.md`, `approach-choice.md`, `data-model.md`, etc.
   - Each file: Key Points → Details → Open Questions
5. Identify what the answer implies for the next decision, ask that.

### Bringing ideas to the table

Throughout the interview:
- Volunteer recommendations when research points one way ("My read: option B — here's why...").
- Raise angles the user didn't ask about if research surfaced them.
- Push back when the user's direction collides with the existing architecture ("That would require changing the public API of X — worth it?").

### Progress Updates
Every 3 rounds, briefly note:
```
**Interview Progress:** X/Y themes covered, N files created, ~Z rounds remaining
```

### Synthesis
When all decisions are made, **do not write the summary yet** — first run Phase 3b.

---

## Phase 3b: Gap Research (Post-Interview)

**Goal:** Fill knowledge gaps that emerged during the interview.

1. Review everything discussed. Identify:
   - New concepts or techniques the user raised that weren't in Phase 2.
   - Changed scope — if the user pivoted, the original research may no longer cover the right ground.
   - Open questions flagged during the interview that need data to resolve.
2. If gaps exist, launch targeted research agents.
3. Update `.claude/Documents/research/<topic>.md` with new findings.
4. If new findings affect decisions already made, flag them to the user before proceeding.
5. If no gaps (rare), note that explicitly and proceed.

---

## Phase 3c: YAGNI Pass

**Goal:** Cut speculative scope before it becomes a plan.

1. Read the draft plan with fresh eyes. For each proposed phase:
   - Does the *current* problem require this, or is it "while we're here"?
   - Is there a smaller version that ships value sooner?
   - Would removing it break anything the user actually asked for?
2. Present cuts to the user directly: "I'd drop phases X and Y — they're speculative. Keep or cut?"
3. Update the plan with the agreed cuts.

---

## Phase 4: Plan & Hand Off

**Goal:** Produce a concrete implementation plan.

1. Write `_summary.md` — the complete decision record:
   ```markdown
   # Summary: <Topic>

   ## Problem
   <what we're solving and why>

   ## Solution
   <high-level approach chosen>

   ## Key Decisions
   | Decision | Choice | Rationale |
   |----------|--------|-----------|
   | ...      | ...    | ...       |

   ## Architecture
   <code structure, file paths, API sketch, data flow>

   ## Implementation Order
   1. Phase 1: <description> — files: <paths>
   2. Phase 2: <description> — files: <paths>
   ...

   ## Expected Results
   <what success looks like, measurable where possible>

   ## Risks
   <what could go wrong, mitigation strategies>
   ```
2. Update `_interview-index.md` with the final file list and status = Complete.
3. Ask the user how they want to proceed:
   - **Defer:** save state for a later session (e.g. via a handover doc or by leaving the artifacts in place).
   - **Start now:** create tracking items in the project's issue tracker / task list and begin work in a new session. Don't start implementing inside this skill — it's a planning skill.

---

## Output Structure

```
.claude/Documents/
  research/
    <topic>.md                              — Research findings, option comparison
  interviews/<topic>-<date>/
    _interview-index.md                     — Index with status and file list
    _summary.md                             — Final synthesis with all decisions + plan
    scope-and-strategy.md                   — Scale, approach, constraints
    <decision-topic>.md                     — One file per major decision area
```

If the project keeps its docs somewhere other than `.claude/Documents/` (e.g. `docs/adr/`, `design/`), mirror that convention instead — but keep the same file layout within the chosen root.

---

## Anti-Patterns

- **Asking before researching.** Don't present options you haven't explored.
- **Giant text walls instead of AskUserQuestion** for decisions with clear options.
- **Forgetting to write.** Every answer saved to a file before the next question.
- **Speculative phases in the final plan.** Run YAGNI first.
- **Implementation during planning.** No code, no builds. That's separate work.
- **Ignoring existing architecture.** Always read the current code before proposing changes.
- **Generic research.** Tailor agent prompts to the specific problem — "lock-free queue throughput at >1M ops/sec" beats "concurrency techniques".
