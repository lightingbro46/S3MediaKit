# Storage conventions

- Nullable values use the project `Optional<T>` from `manager/Storage/DbSchema.h`, not `std::optional`.
- Entities use `DECLARE_ENTITY`/`DECLARE_ENTITY_NO_PK`; repositories extend `SqliteRepository<T>` with the correct database tag.
- Use parameterized query APIs. Do not concatenate user input into SQL.
- Use a shared transaction executor for multi-table atomic changes.
- New tables that cross a wire or are logged should provide `toJson()` and `fromJson()` consistent with nearby entities.
- Sync registration belongs in the existing `SyncManager` startup registration path; do not add table-name conditionals to the generic sync engine.
- Keep FTS virtual tables, backfill statements, and insert/update/delete triggers consistent with the source table.

## Repository and Imp responsibilities

Repository methods may:

- build parameterized `SELECT`, `INSERT`, `UPDATE`, and `DELETE` statements;
- map DB rows to entities and accept persistence-ready entities/fields;
- provide compare-and-set updates, claims, and multi-table transactions when atomicity must be enforced by SQLite;
- expose query/count/existence operations whose result comes directly from stored rows.

Repository methods must not:

- generate IDs or read the current clock implicitly;
- parse policy/domain JSON or infer business meaning from serialized payloads;
- calculate range splits, retention decisions, effective policies, or state-transition targets;
- normalize caller input, select defaults, perform external I/O, or mutate runtime caches.

Imp methods validate and normalize caller data, assign IDs/timestamps, parse serialized domain data, perform calculations, choose the requested transition, and call one or more Repository operations. Keep cross-service validation and external I/O in the manager/service layer. Search production callers after a refactor and replace direct `*Repository` use with the corresponding `*Imp` facade.

## Schema integrity review

For every new or changed table, explicitly review:

- whether references can use a real column and foreign key instead of JSON/text;
- delete/update behavior for parent rows and whether history must survive (`RESTRICT`, `CASCADE`, or soft delete);
- allowed enum/status values and whether DB `CHECK` constraints can remain forward-compatible;
- uniqueness and partial uniqueness required for idempotency or active-work exclusion;
- indexes against actual optional-filter, ordering, TTL, overlap, and claim queries; remember that a composite index is only useful from its leftmost columns;
- secrets and personal data: ciphertext format, key identifier/version, redaction, rotation, and migration from legacy plaintext;
- timestamps, units, nullability, defaults, and ownership of generated values;
- concurrent read-modify-write paths that require a transaction or atomic SQL expression.

If a relation remains embedded in JSON, Repository loads/stores the payload only. Imp parses it and performs exact field matching; never enforce references with substring matching. Document that SQLite cannot provide foreign-key integrity or efficient relational lookup for that field, and prefer a normalized child table when the relation participates in delete guards, joins, or frequent queries.
