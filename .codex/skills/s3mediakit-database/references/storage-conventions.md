# Storage conventions

- Nullable values use the project `Optional<T>` from `manager/Storage/DbSchema.h`, not `std::optional`.
- Entities use `DECLARE_ENTITY`/`DECLARE_ENTITY_NO_PK`; repositories extend `SqliteRepository<T>` with the correct database tag.
- Use parameterized query APIs. Do not concatenate user input into SQL.
- Use a shared transaction executor for multi-table atomic changes.
- New tables that cross a wire or are logged should provide `toJson()` and `fromJson()` consistent with nearby entities.
- Sync registration belongs in the existing `SyncManager` startup registration path; do not add table-name conditionals to the generic sync engine.
- Keep FTS virtual tables, backfill statements, and insert/update/delete triggers consistent with the source table.
