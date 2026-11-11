# CLAUDE.md

## How to work
- Before editing, read the code around the change and find the existing pattern. Copy it rather than inventing a new one.
- For any change over ~30 lines or touching more than 2 files: write a short plan first, then implement.
- Make the smallest change that solves the problem. Don't refactor, rename, or reformat unrelated code.
- If the request is ambiguous and a wrong guess is expensive, ask one question. Otherwise pick the sensible default and say what you assumed.
- When you're unsure whether an API/flag/method exists, check the source or docs in the repo. Never guess.
- Put throwaway test code (repros, experiments, one-off scripts) in `.scratch/`. It's gitignored. Tests worth keeping go in the real test suite.
- Git worktrees live at `~/worktrees/GoPlusPlus/<name>`. Create them with `git worktree add`, not the Agent tool's worktree isolation.

## Code style
- Write code as clean and simple as the problem needs, and no more. No dead code, unused helpers, needless abstractions, or defensive checks for cases that can't happen.
- Keep comments to a minimum. Names and structure should explain *what* the code does. Comment only the *why* the code can't show: a non-obvious invariant, a workaround, a reference to a spec/ADR section.
- Never write comments that repeat the code, narrate a change ("added X", "fixed Y"), or explain obvious steps. Doc comments on public items stay short: one line unless the contract really needs more.
- Comments never mention the plan: no plan IDs (`P00`, `D0.13`), step or phase numbers, task names, or review rounds. Say what the code is, nothing about how the work was organized.

## Dev loop (speed)
Budgets and design: plan P00 D0.13 (`plans/2026-11-11-goplusplus-language/00-foundations/README.md`) and ADR-0001 (100% Rust -> Clang-23 C23). A slow loop is a bug.
- After an edit, run the narrowest test first (one crate or test filter, e.g. `timeout 60 cargo test -p gopp-syntax`). Run the full suite (`timeout 120 cargo test --workspace`) once, before finishing the task.
- If one command takes more than 2 minutes, stop and fix the slowness (a narrower target, a cache, a test that stops earlier) instead of waiting. Report it.
- Run long jobs (full suite, cdep builds, benchmarks) in the background and keep working.
- Never delete build folders (`target/`). Each worktree has its own `target/` folder; all of them share one `sccache`.
- New tests stop as early as they can (AST/GIR/C23 snapshot or dump check). A test that needs more than 2 s is marked `#[ignore]` / long.

## Definition of done
A task is not done until:
1. It compiles / the app boots.
2. Relevant tests pass. Add a test for any bug you fix (it must fail before your fix).
3. You've re-read your own diff (`git diff`) and removed debug prints, dead code, and stray TODOs.
4. You report: what changed, how you verified it, and anything you did NOT verify.

## Commits
- Commit once per task, when the reviewer returns CLEAN. No commits for intermediate steps (merge, review rounds, fixes). Exception: each `coder` commits its own `-c1/-c2/-c3` branch so the PM can read it.
- Commit messages say what changed, and why if it isn't obvious. Same as comments: no plan IDs, step or phase numbers, task names, or review rounds.
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
- Architecture (`ADR-0001`, `ADR-0002`): `gopp` is **100% Rust (Edition 2024)** inside a Cargo workspace (`crates/gopp-*`), lowering GIR (with `-fpass-plugin` middle-IR plugin support) to **Clang-23 GNU C23** (`.c`, or `.cpp` `-std=c++20` when importing C++ headers) and cross-compiling / cross-linking via **Clang 23.1.0 + LLD 23.1.0** (`-fuse-ld=lld`).
- Tools come from the project's Nix flake (`flake.nix`: `rustc`, `cargo`, `clang` 23.1.0, `lld` 23.1.0, `sccache`): run commands as `nix develop -c <cmd>`.
- Commands (run from the repo root; the dev shell turns on `sccache` with a 100 GB cache):
  - Build: `nix develop -c cargo build --workspace` (`--release` for optimized release binary)
  - Test (all): `nix develop -c timeout 120 cargo test --workspace` (long tests: `cargo test --workspace -- --ignored`)
  - Test (one crate): `nix develop -c timeout 60 cargo test -p gopp-gir` (or `-p gopp-codegen-c`, `-p gopp-plugin-host`, etc.)
  - Test (one filter): `nix develop -c timeout 60 cargo test -p gopp-sema exact_rational`
  - Lint/format: `nix develop -c cargo clippy --workspace --all-targets -- -D warnings` and `nix develop -c cargo fmt --all -- --check`
  - Run tests (`tests/run`) run their programs through the fast runner (`gopp test-run`: batch C23 compile + `dlopen` shared-object execution in pooled `test-exec` processes; `docs/design/test-runner.md`). Real binaries instead: `GOPP_FAST_RUN=0` or `--real`.
  - Run locally: `nix develop -c cargo run -p gopp-cli -- version` (also `gopp targets`, `gopp explain E0111`, `gopp build -target=linux/arm64 -fpass-plugin=./plugin.so -emit=c ...`)

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
- Every new analysis gets a generated stress test (thousands of nested and sibling cases) that proves its check time stays near-linear.
- Speed checks in the normal test suite count work (steps), never wall time; timing-ratio checks only run in long tests with a quadratic control.
- Wrap every timing or benchmark run in `timeout 60` (or less), and never wait on a run that has no time limit.
- Never wait in a polling loop (`until … sleep`) without a timeout; run checks in the foreground with `timeout`, or the agent can wait forever.
- When a rule must also hold in generic code, record each use in the generic body and decide it at instantiation with the same function plain code uses; prove it with a generated plain-vs-generic table test.
- `docs/spec/examples/` are test inputs: run `timeout 120 cargo test --workspace` before committing any docs change, even a "docs-only" one.
- A step test for a cached or memoized query asks in every order: start to end, far end first, and with invalidations between the asks. Asking only start to end hides quadratics and missed errors.
- Any loop that grows its input ("double N until measurable") needs hard bounds on size, rounds, CPU and wall time. Run size-scaling tests and repros under `ulimit -f` and `timeout`: /tmp is RAM, and an unbounded loop once filled it.
- Before removing a worktree, check that no gate list, corpus or baseline points at files inside it (its build/ and .scratch/ too); a removed worktree once took 23,439 gate inputs with it.
- A checker that accepts an output change must compare what the code does (the old body against the new one), never only names: a name-only classifier once passed two miscompiles on 189,674 files.
- Never use `git stash` in a worktree: the stash list is shared by all worktrees, and a pop once applied another task's changes. To test with a change reverted, use a throwaway worktree or a patch file in `.scratch/`.
