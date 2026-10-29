# CLAUDE.md

## How to work
- Before editing, read the code around the change and find the existing pattern. Copy it rather than inventing a new one.
- For any change over ~30 lines or touching more than 2 files: write a short plan first, then implement.
- Make the smallest change that solves the problem. Don't refactor, rename, or reformat unrelated code.
- If the request is ambiguous and a wrong guess is expensive, ask one question. Otherwise pick the sensible default and say what you assumed.
- When you're unsure whether an API/flag/method exists, check the source or docs in the repo. Never guess.
- Put throwaway test code (repros, experiments, one-off scripts) in `.scratch/`. It's gitignored. Tests worth keeping go in the real test suite.
- Git worktrees live at `~/worktrees/GoPlusPlus/<name>`. Create them with `git worktree add`, not the Agent tool's worktree isolation.

## Dev loop (speed)
Budgets and design: plan P00 D0.13 (`plans/2026-10-27-goplusplus-language/00-foundations/README.md`). A slow loop is a bug.
- After an edit, run the narrowest test first (one file, or one stage target such as `check-parser`). Run the full suite once, before finishing the task.
- If one command takes more than 2 minutes, stop and fix the slowness (a narrower target, a cache, a test that stops earlier) instead of waiting. Report it.
- Run long jobs (full suite, cdep builds, benchmarks) in the background and keep working.
- Never delete build folders. Each worktree has its own build folder; all of them share one sccache.
- New tests stop as early as they can (AST/GIR dump + FileCheck). A test that needs more than 2 s is marked long.

## Definition of done
A task is not done until:
1. It compiles / the app boots.
2. Relevant tests pass. Add a test for any bug you fix (it must fail before your fix).
3. You've re-read your own diff (`git diff`) and removed debug prints, dead code, and stray TODOs.
4. You report: what changed, how you verified it, and anything you did NOT verify.

## Commits
- Commit once per task, when the reviewer returns CLEAN. No commits for intermediate steps (merge, review rounds, fixes). Exception: each `coder` commits its own `-c1/-c2/-c3` branch so the PM can read it.
- Never push (see Guardrails).

## Guardrails (never without explicit permission)
- No `git push`, force-push, rebase of shared branches, or `git reset --hard`.
- No running migrations against anything except the local dev DB.
- No deleting files outside the task's scope.
- No new dependencies without saying why and naming the alternative.
- Never print, log, or commit secrets, API keys, private keys, or wallet seeds. Treat `.env*`, `config/credentials*`, `*.pem`, `*.key` as off-limits.
- Money/balances: never use floats. Use `BigDecimal` (Ruby) / integer minor units. Flag any arithmetic on amounts for review.

## Environment
- OS: NixOS (Linux). Shell: bash for commands (the login shell is fish). Use Linux paths and commands.
- Tools come from the project's Nix flake (P00.02 creates it): run commands as `nix develop -c <cmd>`.
- Commands (run from the repo root; the dev shell turns on sccache with a 100 GB cache; default build type `Dev` = `-O1 -g`, assertions on, shared LLVM/Clang):
  - Configure (once per build dir): `nix develop -c cmake -S . -B build -G Ninja` (`-DCMAKE_BUILD_TYPE=Release` for static LLVM/Clang/LLD, `-DCMAKE_BUILD_TYPE=Debug -DGOPP_USE_SANITIZER="Address;Undefined"` for sanitizers)
  - Build: `nix develop -c ninja -C build gopp`
  - Test (all): `nix develop -c ninja -C build check` (every `check-<stage>` target plus the front-end include check; slow tests: `check-long`)
  - Test (one stage): `nix develop -c ninja -C build check-driver` (or `check-unit`)
  - Test (one file): `nix develop -c lit -sv build/tests/driver/version.test` (lit test), or `nix develop -c build/bin/GoppUnitTests --gtest_filter='HostToolchain.*'` (unit tests)
  - Lint/format: `nix develop -c ninja -C build format-check` (fix with `... format`); header rule: `... check-includes`
  - Suite-time gate: `nix develop -c ninja -C build check-suite-time` (fails if >10% slower than `tests/suite-time-baseline.json`; refresh with `nix develop -c ninja -C build check-suite-time-update`, key from `-DGOPP_SUITE_TIME_KEY`, default `dev`)
  - Run locally: `nix develop -c build/bin/gopp version` (also `gopp host-toolchain`)
  - Assertions-enabled LLVM (compiles LLVM/Clang/LLD from source the first time): `nix develop .#assertions -c ...` with its own build dir, e.g. `-B build-assert`

## Stack notes
### Ruby / Rails
- Follow existing service-object / concern patterns in the repo.
- Wrap multi-row balance or order updates in a transaction with row locks (`lock!` / `SELECT ... FOR UPDATE`).
- Every new query on a large table: check it uses an index (`EXPLAIN`).

### Kafka
- Consumers must be idempotent: assume every message can arrive twice or out of order.
- Don't change topic names, message schemas, or keys without listing every producer and consumer affected.

### PostgreSQL
- Migrations must be safe on a live table: add columns nullable, backfill separately, `CREATE INDEX CONCURRENTLY`.
- Every migration needs a working `down`.

### C++ / Rust / Go
- C++: RAII only, no raw `new`/`delete`. Build with warnings as errors.
- Rust: no `unwrap()`/`expect()` in non-test code without a comment explaining why it can't fail.
- Go: handle every error; wrap with context (`fmt.Errorf("...: %w", err)`).

## Debugging
1. Reproduce first. Get the exact error or a failing test before changing code.
2. Form one hypothesis, test it, then the next. Don't shotgun-edit several things at once.
3. After 2 failed attempts, hand the debugging to Opus: launch a `coder` with `model: "opus"` and give it everything you've tried and ruled out.
4. After 3 failed attempts in total, stop and summarize what you've tried and what you've ruled out.

## Models
- Sonnet: normal daily tasks (project default in `.claude/settings.json`), the 3 coders, early review rounds, the coder fixing findings.
- Opus: the `/team-build` manager, the PM merge, the final reviewer on risky code, debugging after 2 failed tries.
- Haiku: code search (`searcher` agent), summarizing logs or test output (`summarizer` agent).

## Communication
- Be brief. Lead with the result, then details.
- Use code blocks for commands and diffs.
- Say clearly when something is a guess.

## Lessons learned
<!-- Add one line every time Claude makes a mistake you had to correct. This is the "memory" part of the harness. -->
- When you fix a checker bug, test both sides: the bad code now fails, AND the nearby good code still passes. A fix that breaks good code is a new bug and must be fixed too.
- In parallel code, anything computed once and shared must not depend on which thread computes it first: no errors, positions or depth limits taken from the asking thread.
- For a safety rule, first list every way the language could break it, then block them all in one place; don't patch one reported case at a time.
- Make safety decisions from the checker's real types, never from a guess made before the code is type-checked. A guess is wrong both ways: it rejects good code and lets bad code through.
- Wrap every timing or benchmark run in `timeout 60` (or less), and never wait on a run that has no time limit.
- `docs/spec/examples/` are test inputs: run `ninja check` before committing any docs change, even a "docs-only" one.
