#!/usr/bin/env bash
# PreToolUse hook: the `reviewer` subagent may only write inside .scratch/.
# Other agents and the main session are not affected.
set -euo pipefail

input=$(cat)

agent_type=$(jq -r '.agent_type // empty' <<<"$input")
[[ "$agent_type" == "reviewer" ]] || exit 0

deny() {
  jq -n --arg reason "$1" '{hookSpecificOutput: {hookEventName: "PreToolUse", permissionDecision: "deny", permissionDecisionReason: $reason}}'
  exit 0
}

path=$(jq -r '.tool_input.file_path // .tool_input.notebook_path // empty' <<<"$input")
[[ -n "$path" ]] || deny "reviewer: write has no file path; only .scratch/ is allowed"

project_dir=${CLAUDE_PROJECT_DIR:-$(jq -r '.cwd' <<<"$input")}
cwd=$(jq -r '.cwd // empty' <<<"$input")
[[ "$path" == /* ]] || path="${cwd:-$project_dir}/$path"

# Resolve .. and symlinks so paths like .scratch/../src/x can't slip through.
scratch=$(realpath -m "$project_dir/.scratch")
worktree_root=$(realpath -m "$HOME/worktrees/GoPlusPlus")
target=$(realpath -m "$path")

# Allowed: <project>/.scratch/... or <worktree_root>/<name>/.scratch/...
[[ "$target" == "$scratch"/* ]] && exit 0
if [[ "$target" == "$worktree_root"/* ]]; then
  rel=${target#"$worktree_root"/}
  [[ "$rel" =~ ^[^/]+/\.scratch/ ]] && exit 0
fi
deny "reviewer may only write inside .scratch/ (blocked: $target)"
