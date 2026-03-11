# Merge DataTable and ColumnarDB into One Package

## Context

The `datatable/` and `columnardb/` packages have grown into a single logical
unit with a circular dependency. `columnardb-compiled.lsh` contains general-
purpose compiled functions (gather, sort, filter, CSR build, binary search)
that datatable already depends on via `(functionp _cdb-...)` guards.
`columnardb-query.lsh`, `columnardb-groupby.lsh`, and `columnardb-multiindex.lsh`
operate entirely on in-memory DataTable objects. Meanwhile, `datatable-join.lsh`
explicitly loads `columnardb/columnardb-compiled`, creating a true circular
dependency. Cross-dependencies are inevitable, so everything belongs in one place.

**Goal**: The `columnardb/` package directory goes away entirely. Everything
lives under `datatable/`. Two user-facing entry points:

- `(libload "datatable/datatable")` -- full in-memory DataTable with query
  language, joins, group-by, multi-index, and all compiled hot-paths
- `(libload "datatable/columnardb")` -- adds the persistence layer (save,
  load, mmap, append, compress, partitions, database catalog, mutate)

## Complete File Mapping

### Files that merge into existing datatable/ files

| Source | Destination | Action |
|---|---|---|
| `columnardb/columnardb-compiled.lsh` | `datatable/datatable-compiled.lsh` | Merge ~80 compiled functions. Rename `_cdb-*` -> `_dt-*`. Drop duplicate `_cdb-l1i1sortup`/`_cdb-l1i1sortdown`. Single `dhc-make`. |

### Files that move into datatable/ (in-memory operations, rename)

| Source | Destination | Rename |
|---|---|---|
| `columnardb/columnardb-query.lsh` | `datatable/datatable-query.lsh` | `_cdb-*` -> `_dt-*`. Keep `db-select`/`db-count` macro names. |
| `columnardb/columnardb-groupby.lsh` | `datatable/datatable-groupby.lsh` | `columnardb-group-by` -> `datatable-group-by`. `_cdb-*` -> `_dt-*`. |
| `columnardb/columnardb-multiindex.lsh` | `datatable/datatable-multiindex.lsh` | `columnardb-where-multi*` -> `datatable-where-multi*`. `_cdb-*` -> `_dt-*`. |

### Files that move into datatable/ (persistence layer, keep names)

| Source | Destination | Notes |
|---|---|---|
| `columnardb/columnardb.lsh` | `datatable/columnardb.lsh` | Becomes the persistence entry point: `(libload "datatable/columnardb")`. Update all internal libloads from `"columnardb/X"` to `"datatable/X"`. |
| `columnardb/columnardb-io.lsh` | `datatable/columnardb-io.lsh` | LCDB binary I/O. Minimal changes. |
| `columnardb/columnardb-compress.lsh` | `datatable/columnardb-compress.lsh` | Column compression. Update `_cdb-*` -> `_dt-*` if any. |
| `columnardb/columnardb-database.lsh` | `datatable/columnardb-database.lsh` | Database catalog class. |
| `columnardb/columnardb-mutate.lsh` | `datatable/columnardb-mutate.lsh` | Delete/update on persisted tables. |
| `columnardb/columnardb-partition.lsh` | `datatable/columnardb-partition.lsh` | Partitioned table support. |

### Tests that move

| Source | Destination |
|---|---|
| `columnardb/tests/test-query.lsh` | `datatable/tests/test-query.lsh` |
| `columnardb/tests/test-query-ext.lsh` | `datatable/tests/test-query-ext.lsh` |
| `columnardb/tests/test-compiled-query.lsh` | `datatable/tests/test-compiled-query.lsh` |
| `columnardb/tests/bench-compiled-query.lsh` | `datatable/tests/bench-compiled-query.lsh` |
| `columnardb/tests/test-database.lsh` | `datatable/tests/test-database.lsh` |
| `columnardb/tests/run-all.lsh` | Merge into `datatable/tests/run-all.lsh` |

### C/ directory

`columnardb/C/` contains auto-generated dhc stubs (`columnardb_io.c`,
`columnardb_compiled.c`, `columnardb_compress.c`). These regenerate
automatically when dhc-make runs. No manual move needed -- new stubs will
appear in `datatable/C/` after first compilation.

### Design notes

| Source | Destination |
|---|---|
| `columnardb/design-notes.md` | `datatable/design-notes-persistence.md` |
| `datatable/design-notes.md` | `datatable/design-notes.md` (expand with query/groupby docs) |

## Detailed Steps

### Step 1: Expand datatable-compiled.lsh

Merge all compiled functions from `columnardb-compiled.lsh` into
`datatable/datatable-compiled.lsh`. This is the biggest single change.

- Copy all function definitions from `columnardb-compiled.lsh`
- Rename every `_cdb-` prefix to `_dt-` throughout
- Remove the duplicate sort functions (`_cdb-l1i1sortup`/`_cdb-l1i1sortdown`)
  since `_dt-l1i1sortup`/`_dt-l1i1sortdown` already exist with identical code
- Merge into a single `(dhc-make ...)` call at the bottom
- Functions being absorbed (~80 compiled functions):
  - Aggregation: `_dt-d1sum-perm`, `_dt-d1mean-perm`, `_dt-d1min-perm`, `_dt-d1max-perm`, etc.
  - Null-aware variants: `_dt-d1sum-perm-nullskip`, `_dt-d1mean-perm-nullskip`, etc.
  - Gather: `_dt-d1gather`, `_dt-f1gather`, `_dt-l1gather`, `_dt-i1gather`
  - Sort checks: `_dt-d1sortedp-safe`, `_dt-f1sortedp`, `_dt-l1sortedp`
  - Binary search: `_dt-d1bsearch-ge`, `_dt-d1bsearch-le`, `_dt-l1bsearch-ge`, `_dt-l1bsearch-le`
  - CSR: `_dt-i1count-codes`, `_dt-i1csr-fill`
  - Filters: `_dt-i1where-eq`, `_dt-d1where-*`, `_dt-l1where-*`, `_dt-f1where-*`
  - Set ops: `_dt-i1sorted-intersect`, `_dt-i1sorted-union`
  - String ops: `_dt-like-match`
  - IN filters: `_dt-d1in-filter`, `_dt-l1in-filter`, `_dt-f1in-filter`
  - Utility: `_dt-i1iota`, `_dt-d1i1sortup`, `_dt-d1i1sortdown`, etc.

### Step 2: Fix references in existing datatable/*.lsh

Update all `_cdb-*` references to `_dt-*` in:
- `datatable/datatable.lsh` (lines 607-624: gather operations)
- `datatable/stringcol.lsh` (lines 319-340: `_cdb-i1count-codes`, `_cdb-i1csr-fill`)
- `datatable/intcol.lsh` (lines 254-275: `_cdb-i1count-codes`, `_cdb-i1csr-fill`)
- `datatable/datatable-join.lsh`: Remove `(libload "columnardb/columnardb-compiled")`
  on line 19. Replace `_cdb-d1sortedp-safe`, `_cdb-f1sortedp`, `_cdb-l1sortedp`
  with `_dt-` equivalents.

The `(functionp _cdb-...)` guards become `(functionp _dt-...)` guards. Since
the compiled functions are now always loaded, these could become unconditional,
but keeping the guards is harmless -- clean up in a follow-up pass.

### Step 3: Create new datatable in-memory modules

**datatable-query.lsh**: Copy `columnardb-query.lsh` (742 lines).
- Remove `(libload "columnardb/columnardb-compiled")` (already loaded via datatable)
- Update all `_cdb-*` calls to `_dt-*`
- Keep `db-select` and `db-count` macro names unchanged
- Keep `_query-*` internal helper names unchanged

**datatable-groupby.lsh**: Copy `columnardb-groupby.lsh`.
- Rename `columnardb-group-by` -> `datatable-group-by`
- Update `_cdb-*` calls to `_dt-*`

**datatable-multiindex.lsh**: Copy `columnardb-multiindex.lsh`.
- Rename `columnardb-where-multi` -> `datatable-where-multi`
- Rename `columnardb-where-multi-idx1` -> `datatable-where-multi-idx1`
- Update `_cdb-*` calls to `_dt-*`

### Step 4: Update datatable.lsh libload chain

At the bottom of `datatable/datatable.lsh`, add:
```lisp
(libload "datatable/datatable-join")
(libload "datatable/datatable-groupby")
(libload "datatable/datatable-multiindex")
(libload "datatable/datatable-query")
```

This makes `(libload "datatable/datatable")` the complete in-memory package.

### Step 5: Move persistence files into datatable/

Move all remaining columnardb files into `datatable/`:
- `columnardb.lsh` -> `datatable/columnardb.lsh`
- `columnardb-io.lsh` -> `datatable/columnardb-io.lsh`
- `columnardb-compress.lsh` -> `datatable/columnardb-compress.lsh`
- `columnardb-database.lsh` -> `datatable/columnardb-database.lsh`
- `columnardb-mutate.lsh` -> `datatable/columnardb-mutate.lsh`
- `columnardb-partition.lsh` -> `datatable/columnardb-partition.lsh`

In each file, update any `(libload "columnardb/...")` to `(libload "datatable/...")`.

**datatable/columnardb.lsh** (the persistence entry point) becomes:
```lisp
(libload "datatable/datatable")        ;; pulls in everything in-memory
(libload "datatable/columnardb-io")
(libload "datatable/columnardb-partition")
(libload "datatable/columnardb-mutate")
(libload "datatable/columnardb-compress")
(libload "datatable/columnardb-database")
```

Remove the old loads that are now part of datatable.lsh: `columnardb-compiled`,
`columnardb-groupby`, `columnardb-multiindex`, `columnardb-query`,
`datatable/datatable-join`. Also update `_cdb-*` -> `_dt-*` in any persistence
files that reference compiled functions (e.g. columnardb.lsh range queries use
`_cdb-*bsearch-*` and `_cdb-*sortedp`).

### Step 6: Update external references

All files outside datatable/ that load columnardb:
- `ltor/coinbase-hdb-writer.lsh` line 13: `"columnardb/columnardb"` -> `"datatable/columnardb"`
- `ltor/coinbase-hdb-reader.lsh` line 14: `"columnardb/columnardb"` -> `"datatable/columnardb"`
- `ltor/ltor-hdb-writer.lsh` line 14: `"columnardb/columnardb"` -> `"datatable/columnardb"`

Scan each for any `columnardb-group-by` or `columnardb-where-multi` calls and
update to the `datatable-` names.

Other dependents (no changes needed -- they only load datatable):
- `ltor/coinbase-rdb.lsh`, `ltor/coinbase-analytics.lsh`, `ltor/ltor-monitor.lsh`
- `wire/wire-serialize.lsh`, `json/json.lsh`
- `mapper/mapper-viz.lsh` (loads datatable-csvread only)

### Step 7: Migrate all tests into datatable/tests/

Move all test files from `columnardb/tests/` to `datatable/tests/`:
- `test-query.lsh`, `test-query-ext.lsh`, `test-compiled-query.lsh` -> update libloads
- `bench-compiled-query.lsh` -> update libloads
- `test-database.lsh` -> update libloads to `"datatable/columnardb"`
- Merge `columnardb/tests/run-all.lsh` content into `datatable/tests/run-all.lsh`

Update all `(libload "columnardb/columnardb")` references in tests to
`(libload "datatable/columnardb")`.

### Step 8: Update design notes

- Move `columnardb/design-notes.md` -> `datatable/design-notes-persistence.md`
  Trim to persistence-only: LCDB format, mmap, splayed layout, compression,
  partitions, database catalog, file locking, append semantics.

- Expand `datatable/design-notes.md` to cover query language, group-by,
  multi-index, compiled hot-paths. Document `db-select`/`db-count` macro
  syntax, query planner (CSR -> binary search -> scan), aggregation framework.

### Step 9: Delete columnardb/ package directory

After everything works and tests pass:
- Delete the entire `packages/columnardb/` directory

## Execution Order

1. Step 1 (compiled merge) -- foundation, everything depends on this
2. Step 2 (fix _cdb refs) -- datatable self-contained
3. Step 3 (new in-memory modules) -- query, groupby, multiindex
4. Step 4 (datatable libload chain) -- wire in new modules
5. **Test checkpoint**: `(libload "datatable/tests/run-all")` -- verify in-memory layer
6. Step 5 (move persistence files) -- into datatable/
7. Step 6 (external refs) -- update ltor etc.
8. Step 7 (migrate tests) -- all tests under datatable/
9. **Test checkpoint**: run full test suite
10. Step 8 (design notes)
11. Step 9 (delete columnardb/)

## Risk Notes

- **dhc-make size**: ~80 compiled functions in one `dhc-make` call. The existing
  columnardb-compiled already has ~75 -- Lush handles it fine.
- **Name collisions**: `_dt-l1i1sortup`/`_dt-l1i1sortdown` exist in both (identical
  code). Remove the columnardb copy.
- **Function renames in persistence layer**: `columnardb.lsh` itself uses `_cdb-*`
  functions for range queries (binary search, sorted checks). These must also
  become `_dt-*`.
- **Test string literals**: `columnardb/tests/run-all.lsh` has subprocess commands
  with `(libload "columnardb/columnardb")` as string literals (~8 occurrences).
  These must become `"datatable/columnardb"`.
