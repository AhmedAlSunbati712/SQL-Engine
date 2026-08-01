# Repository Guidelines

## Project Structure & Module Organization
This repository is currently design-first. The project goal is an educational SQL engine inspired by SQLite, focused primarily on backend storage-engine concerns rather than a parser or virtual machine. Core material lives under [Docs](/Volumes/youwhat/projects/SQL-Engine/Docs), with system-level constraints in [Docs/System Constraints](/Volumes/youwhat/projects/SQL-Engine/Docs/System%20Constraints) and implementation planning in [Docs/Technical Design Docs](/Volumes/youwhat/projects/SQL-Engine/Docs/Technical%20Design%20Docs). The reference text [SQLite Database System Design and Implementation (Sibsankar Haldar) (Z-Library)-2.pdf](/Volumes/youwhat/projects/SQL-Engine/SQLite%20Database%20System%20Design%20and%20Implementation%20%28Sibsankar%20Haldar%29%20%28Z-Library%29-2.pdf) at the repository root should be treated as background material, not executable project content.

When source code is added, keep it in a dedicated top-level directory such as `src/`, with matching tests in `tests/`. Keep document names aligned with subsystem names, for example `Pager + Cache.md`.

## Build, Test, and Development Commands
There is no build or test pipeline checked in yet. For now, contributors should use lightweight validation commands while editing docs:

```sh
git status
rg --files Docs
markdownlint Docs AGENTS.md
```

`git status` confirms the working tree, `rg --files Docs` shows the tracked documentation set, and `markdownlint` is the preferred Markdown check if available locally.

## Coding Style & Naming Conventions
Use concise Markdown with clear section hierarchies and short paragraphs. Prefer title-style headings, bullet lists for requirements, and fenced code blocks for pseudocode or command examples. Keep filenames descriptive and stable; preserve the existing subsystem naming pattern used in the docs.

For future C++ work implied by the design docs, use consistent 4-space indentation, `PascalCase` for types (`Page`, `PCache`), and `snake_case` for functions and fields (`page_num`, `commit_phase_one`).

## Testing Guidelines
Until an automated test suite exists, validate contributions by checking internal consistency across design docs and examples. When code is introduced, add tests alongside the implementation and name them after the unit under test, for example `pager_test.cpp` or `pcache_test.cpp`.

## Commit & Pull Request Guidelines
Existing history uses short, descriptive messages such as `TDD for first version of Pager + Cache` and `adding gitignore and pager + cache system constraints doc`. Follow that pattern: state the subsystem and the change in plain language.

Pull requests should include a brief summary, the affected paths, and any design decisions or tradeoffs. If a change updates architecture or behavior, update the relevant document in `Docs/` in the same PR.

## Agent Collaboration Notes
This repository is being developed as an educational systems project. The assistant’s role is to act as an architecture and design partner: clarify tradeoffs, challenge assumptions, compare approaches, and help refine subsystem boundaries and invariants.

Unless explicitly requested, do not default to implementing production code. Prefer design discussion, pseudocode, interface sketches, review of technical documents, and structured reasoning grounded in the SQLite reference PDF and the repository’s own design notes.

## Mandatory Agent Handoff

The handoff is a completion requirement, not an optional documentation step.
An agent MUST NOT claim that a task is complete or send its final response
until every step below has been performed.

After every task in this repository, including implementation, testing,
review, and documentation tasks:

1. Re-read the affected parts of `design-review.md`. If the task changed an
   architectural decision, invariant, interface, binary format, recovery rule,
   concurrency rule, or implementation sequence, update `design-review.md` in
   the same task. Do not leave it describing behavior that the repository no
   longer implements or intends to implement.
2. Prepend a new handoff entry immediately below the `# Project Status`
   heading in `STATUS.md`. Newest entries MUST remain at the top; never append
   a new entry below older history and never delete prior entries.
3. The new status entry MUST contain:
   - The task just completed and the concrete changes made.
   - The validation performed and its result.
   - Whether `design-review.md` changed and why.
   - One bounded **Next Task** for the next agent, based on the current
     repository state.
4. Keep **Next Task** concise. It is a handoff description, not a detailed
   implementation plan; the next agent owns planning and execution.

Use this entry shape:

```markdown
## YYYY-MM-DD — Short Task Name

### Completed

Concise description of the completed work.

### Validation

Commands or checks run and their result.

### Design Review

State what changed in `design-review.md`, or state that no update was needed
because no design contract changed.

### Next Task

One concise, bounded task for the next agent.
```

`STATUS.md` and `design-review.md` are local agent handoff files listed in
`.gitignore`. Their ignored status does not waive this requirement. If either
file cannot be updated, the task is not complete; report the handoff as a
blocker instead of claiming completion.
