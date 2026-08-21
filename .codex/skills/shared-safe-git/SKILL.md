---
name: shared-safe-git
description: "Use when an agent must edit, inspect, stage, or validate files in a Git repository while preserving existing user work. Apply before broad edits, conflict resolution, cleanup, rebases, resets, or any potentially destructive operation."
---

# Shared safe Git workflow

Protect repository state and user ownership throughout the task.

## Before editing

- Run `git status --short` and inspect relevant diffs.
- Identify ignored/generated paths and existing untracked files before creating new files.
- Confirm the exact requested target. Never treat the repository root or a broad glob as a deletion target.

## During editing

- Keep changes scoped and use patch-based edits for hand-authored files.
- Do not overwrite unrelated modifications, restore deleted files, or normalize line endings without authorization.
- Avoid `git reset --hard`, `git checkout --`, recursive deletion, force push, and history rewriting unless the user explicitly authorizes the exact operation.
- Before resolving a conflict, preserve both sides in a temporary or patchable form and understand ownership.

## Before handoff

- Inspect `git diff --check`, `git diff --stat`, and the relevant diff.
- Confirm new files are intentional and generated files are either expected or removed safely.
- Report pre-existing dirty state separately from the files changed for the task.

See [safe-operations.md](references/safe-operations.md) for recovery-oriented alternatives.
