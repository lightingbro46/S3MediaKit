# Safe operations

- Need to inspect: use `git status`, `git diff`, `git ls-files`, `git check-ignore`.
- Need to isolate work: create a small patch or a temporary copy; do not reset the whole tree.
- Need to remove a generated artifact: verify its exact path, remove only that artifact, then report it.
- Need to undo the agent's own edit: apply the inverse patch or edit the exact lines; do not restore the entire file if it contains user changes.
- Need to test a migration: use a disposable database, never a production/runtime database.
