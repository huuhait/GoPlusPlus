---
name: team-build
description: Run a task through the multi-agent team (3 coders, PM merge, reviewer/coder loop until CLEAN). Use when the user types /team-build or asks to run a task through the team.
model: opus
---

# Team build workflow

You are the MANAGER. You do not write the code yourself. You run the team, keep the findings ledger, and decide when it's done. Subagents can't spawn other subagents, so every handoff goes through you.

## Models
Each agent's default model is set in its definition. Only pass the Agent tool's `model` parameter for the two escalations below.

| Role | Model | How |
|---|---|---|
| Manager (you) | Opus | this skill's `model: opus` |
| 3 coders, coder fixing findings | Sonnet | `coder` definition |
| PM merge | Opus | `pm` definition |
| Early review rounds | Sonnet | `reviewer` definition |
| Final fresh reviewer on risky code | Opus | launch `reviewer` with `model: "opus"` |
| Debugging a finding after 2 failed fixes | Opus | launch a fresh `coder` with `model: "opus"` |
| Code search | Haiku | `searcher` agent |
| Summarizing logs or test output | Haiku | `summarizer` agent |

Risky code means changes that touch concurrency, the runtime or memory management, unsafe code or C/C++ interop, codegen, security, money/balances, or migrations.

Use `searcher` instead of reading many files yourself, and `summarizer` for long build or test output, so your context stays small.

## 0. Spec
Restate the task as a short spec: the goal, acceptance criteria, files likely involved, and what is out of scope. If something important is ambiguous, ask the user ONE question now. Pick a slug for the task.

## 1. Three coders, in parallel
First create one git worktree per coder, so they can't overwrite each other and every worktree shows up in `git worktree list` and VS Code:

```
git worktree add ~/worktrees/GoPlusPlus/<slug>-c1 -b task/<slug>-c1
git worktree add ~/worktrees/GoPlusPlus/<slug>-c2 -b task/<slug>-c2
git worktree add ~/worktrees/GoPlusPlus/<slug>-c3 -b task/<slug>-c3
```

Do NOT use the Agent tool's `isolation: "worktree"`; it puts worktrees somewhere else and cleans them up on its own. Then launch 3 `coder` subagents in ONE message so they run at the same time, and give each one its worktree path.

Give each one the same spec, Job A, and a DIFFERENT approach hint so the versions actually differ. For example:
- c1: "Simplest possible change that meets the spec. Minimal new code."
- c2: "Most robust version: handle every edge case and failure mode, defensive tests."
- c3: "Best fit with existing architecture: reuse existing abstractions; refactor locally if it makes the change cleaner."

Adjust the hints to the task. Different algorithms or data structures are also good hints.

## 2. PM merge
Create the merge worktree:

```
git worktree add ~/worktrees/GoPlusPlus/<slug>-merged -b task/<slug>-merged
```

Launch the `pm` subagent with the spec, all 3 coder reports, and the merge worktree path. It builds `task/<slug>-merged` there. The fixing coder and the reviewer also work in this worktree for every round.

## 3. Review loop
Keep a FINDINGS LEDGER in your context, and also write it to `.claude/team/<slug>-ledger.md` inside the merge worktree, so it lands in the final commit:

```
| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
```

Each round:
1. `reviewer` (Mode 1 on the first round, Mode 2 after that) gets the merged branch, the spec, and the ledger. It reviews the uncommitted changes (`git diff`).
2. If it returns CLEAN, go to "4. Finish".
3. `coder` (Job B) gets the merged branch and every OPEN item (new, REOPENED, UPHELD), each with the reviewer's `repro:` line copied exactly. It works on `task/<slug>-merged` without committing, verifies each one, then marks it FIXED or DISPUTED.
4. Back to the reviewer for Mode 2: it confirms the fixes, withdraws or upholds the disputes, and checks for regressions.

Nothing is committed during the loop. The only commit is in "4. Finish", after CLEAN.

An item is closed only when it is CONFIRMED-FIXED or WITHDRAWN. Nothing moves forward while an item is open.

Use the SAME coder agent across rounds (continue it with SendMessage) so it keeps context. Exception: if the same item has been REOPENED twice (two failed fixes), hand that item to a fresh `coder` launched with `model: "opus"`, give it the item's full history, and keep using it for that item.

For the final pass, use a FRESH reviewer that hasn't seen the earlier rounds, so the code isn't graded only by someone who got used to it. If the change is risky (see "Models"), launch that final reviewer with `model: "opus"`.

## Stop conditions (so the loop can't run forever)
- **CLEAN from a fresh reviewer:** done.
- **DEADLOCK** on any item, meaning the same dispute upheld twice with no new evidence: stop and show the user both sides. The human decides.
- **5 rounds without CLEAN:** stop and report the open ledger. Something is wrong with the spec or the approach, and more loops won't fix it.
- **The open count goes UP two rounds in a row:** stop, because fixes are causing new bugs.

## 4. Finish
Only after a fresh reviewer returns CLEAN: run the full test suite, build, and lint one last time yourself. If everything passes, commit the whole task (code, tests, and ledger) as ONE commit on `task/<slug>-merged`. If a stop condition ended the loop instead, do not commit; leave the changes uncommitted for the user.

After the CLEAN commit, remove all four worktrees and keep their branches for history:

```
git worktree remove ~/worktrees/GoPlusPlus/<slug>-c1
git worktree remove ~/worktrees/GoPlusPlus/<slug>-c2
git worktree remove ~/worktrees/GoPlusPlus/<slug>-c3
git worktree remove ~/worktrees/GoPlusPlus/<slug>-merged
```

Never use `--force`. If a removal fails because the worktree has uncommitted or untracked files, leave it and tell the user. Gitignored files such as `.scratch/` don't block removal. If a stop condition ended the loop, keep every worktree so the user can inspect it.

Then report to the user:
- The merged branch name and final commit SHA
- Which coder's version was the base and what was ported in
- The ledger summary: N found, N fixed, N withdrawn, plus anything escalated
- What wasn't verified

Do not push or open a PR unless the user asks.

## Lessons
After each run, if the reviewer found the same KIND of bug that a coder should have avoided, propose one line for the "Lessons learned" section of CLAUDE.md. Ask the user before adding it.
