# ColumnarDB Design Notes

Columnar database for Lush, inspired by Kerf1 and adapted to Lush's
idx/storage model.  852 tests across the full test suite.

---

## Architecture

### Design Philosophy

Kerf1 is a columnar tick database where every value is a self-describing K0
struct with inline type tag, reference count, and power-of-2 slab allocation.
Lush cannot share Kerf's allocator or embed Kerf as a library because:

1. Kerf's K0 is self-describing (inline type/size metadata); Lush's `at`
   objects are untyped containers with type in the storage class.
2. Kerf's pool allocator owns all memory; Lush uses standard malloc/mmap.
3. Kerf vectors are flexible array members in K0; Lush idx objects are strided
   views into separate storage objects.
4. Kerf's persistence = mmapping K0 structs directly; Lush's idx/storage split
   means metadata is separate from data.

The package builds Kerf-inspired functionality on top of Lush's native
idx/storage system.

### Storage Model

Each table is a collection of independently-typed columns.  Numeric columns are
idx1 objects backed by their own storage.  String columns use dictionary
encoding (StringColumn class).  The DataTable class holds a list of these
columns plus metadata.

Column types map to Lush storage types:

| Column Type | Lush Storage | Notes |
|-------------|-------------|-------|
| `'real` | idx1 of ST_D (double) | Default numeric |
| `'int` | idx1 of ST_I32 (int32) | Integer data |
| `'stamp` | idx1 of ST_D (double) | Seconds since epoch, displayed as datetime |
| `'string` | StringColumn (HTable + idx1 of int32 codes) | Per-column dictionary encoding |

### StringColumn (Dictionary Encoding)

String columns use per-column interning inspired by Kerf's HASH/intern type:

- **Forward map**: HTable mapping string to integer code (O(1) lookup)
- **Reverse map**: list of unique strings indexed by code
- **Per-row data**: idx1 of int32 codes (one code per row)

Memory for 10M rows with 3,000 unique symbols (~10 chars avg): ~40 MB with
dictionary encoding vs. ~500 MB with naive AT-storage.  The 12x savings holds
even for high-cardinality columns.

Equality filtering compares integer codes (not strings), and GROUP BY uses codes
as array indices for O(n) grouping without hashing at group time.

### Native-Endian Binary Format (LCDB)

Lush's standard IDX file format is big-endian, but `storage-mmap` maps files
without byte-swapping, producing garbage on x86.  The package uses a custom
native-endian format:

```
Bytes  0-3:  Magic "LCDB" (0x4C434442)
Bytes  4-7:  Version (uint32 native) = 1
Bytes  8-15: Element count (int64 native)
Bytes 16-19: Type code (uint32 native): 1=double, 2=int32
Bytes 20-23: Reserved (zeros)
Bytes 24+:   Raw element data, native byte order
```

The 24-byte header is 3 doubles wide for natural alignment.  `storage-mmap`
with offset 24 maps data directly.  Files are not portable across endianness;
this matches Kerf's native-endian K0 format.

### Splayed Directory Layout

Each DataTable persists as a directory with one file per column:

```
tabledir/
  meta.lsh              S-expression: schema, row count, sort info
  col-0.col             Native binary (LCDB) for column 0
  col-1.col             Native binary (LCDB) for column 1
  col-2-codes.col       String column codes (int32 LCDB)
  col-2-pool.lsh        String column pool (S-expression list)
```

Metadata and string pools use S-expressions (small, human-readable, native to
Lush).  Column data uses LCDB binary (large, mmappable).  Column files use
index-based naming (`col-N`) to avoid filesystem character issues.

### mmap Loading (Zero-Copy)

`columnardb-load` uses `storage-mmap` with offset 24 to map column files.  The
OS kernel handles demand paging: only accessed pages load from disk, pages can
be evicted and reloaded, and multiple processes share physical pages.  Loaded
tables are read-only (`STF_RDONLY`).

A separate `columnardb-load-mem` loads into malloc'd storage for read-write
access.

Empty tables (0 rows) do not write column files.  Loading creates placeholder
1-element arrays that are never accessed.

### Sorted Column Tracking

On save, each numeric/stamp column is scanned with compiled C functions
(`idx-d1sortedp` / `idx-i1sortedp`) to detect sort order.  Sorted column
indices are stored in `meta.lsh` under the `sorted` key.

Sort status lives in table metadata, not on the idx itself (Lush idx objects
have no user attribute slots).  On append, a sorted column stays sorted if the
appended data is itself sorted and its first value >= the old last value.

### Binary Search Range Queries

`columnardb-range` checks metadata for sort status and dispatches:
- Sorted columns: O(log n) via `idx-d1bsearch-left` / `idx-d1bsearch-right`
- Unsorted columns: O(n) linear scan

Typical time-series query pattern:
```lisp
(let ((meta (columnardb-meta path))
      (dt (columnardb-load path)))
  ;; Step 1: binary search on sorted timestamp -> O(log n)
  (let ((indices (columnardb-range dt "time" start end meta)))
    ;; Step 2: extract sub-table
    (let ((sub (==> dt select-rows indices)))
      ;; Step 3: filter by interned string (integer comparison) -> O(k)
      (==> sub where '= "symbol" "AAPL"))))
```

This gives O(log n + k) where k is the time-range row count.

### Growth Strategy

Columns track logical size separately from allocated capacity.  On append,
capacity doubles (amortized O(1)), matching Kerf's strategy.  Bulk loading
pre-allocates to known size and fills via direct writes.

---

## API Summary

### Package Files

```
packages/columnardb/
  columnardb.lsh            Main module: save, load, load-mem, append, range
  columnardb-io.lsh         Compiled C bridge: native-endian LCDB binary I/O
  columnardb-compiled.lsh   Compiled C query kernels (filter, aggregate, join)
  columnardb-compress.lsh   Column compression
  columnardb-database.lsh   Multi-table Database class with catalog
  columnardb-groupby.lsh    GROUP BY aggregation
  columnardb-multiindex.lsh Multi-column indexing
  columnardb-mutate.lsh     In-place mutation operations
  columnardb-partition.lsh  Partitioned tables
  columnardb-query.lsh      SQL-like query macro (db-select)
  C/columnardb_io.c         Auto-generated DH stubs
```

### Core Functions

| Function | Purpose |
|----------|---------|
| `columnardb-save` | Save DataTable to splayed directory |
| `columnardb-load` | Load via mmap (read-only, zero-copy) |
| `columnardb-load-mem` | Load into memory (read-write) |
| `columnardb-append` | Append rows to persisted table |
| `columnardb-range` | Range query (binary search on sorted columns) |
| `columnardb-meta` | Read table metadata |
| `columnardb-groupby` | GROUP BY aggregation |

### SQL-Like Query Language (db-select)

The query language uses Lush's `dmd` macro system to provide declarative
SQL-like syntax that expands to imperative method calls.

```lisp
(db-select ("price" "symbol")
  (from db "trades")
  (where (> "price" 100))
  (order-by "time")
  (limit 100))
```

This expands to a pipeline of DataTable method calls:
```lisp
(let ((dt (==> db table "trades")))
  (let ((filtered (==> dt where-rows '> "price" 100)))
    (let ((sorted (==> filtered sort-by "time" ())))
      (let ((limited (==> sorted select-rows (range* 0 100))))
        (==> limited select-columns (list "price" "symbol"))))))
```

Clause types:

| Clause | Expansion | Notes |
|--------|-----------|-------|
| `(from db name)` | `(==> db table name)` | Source table |
| `(where (op col val))` | `(==> dt where-rows op col val)` | Filter rows |
| `(where (and ...))` | Chained filters | Compound predicates |
| `(order-by col)` | `(==> dt sort-by col ())` | Sort ascending |
| `(order-by col 'desc)` | `(==> dt sort-by col t)` | Sort descending |
| `(limit n)` | `(==> dt select-rows (range* 0 n))` | First n rows |
| `(group-by col agg-list)` | `(columnardb-groupby dt col aggs)` | Aggregation |

### Compiled C Primitives

Performance-critical operations use `dhc-make` compiled C:

| Primitive | Purpose |
|-----------|---------|
| `idx-d1sortedp` / `idx-i1sortedp` | Check if column is sorted |
| `idx-d1bsearch-left` / `idx-d1bsearch-right` | Binary search bounds |
| `idx-d1i1sortup` / `idx-d1i1sortdown` | Grade/argsort (double+int paired sort) |
| `idx-i1i1sortup` / `idx-i1i1sortdown` | Grade/argsort (int+int paired sort) |
| LCDB read/write functions | Native-endian binary I/O |

### Existing DataTable Methods Used by Queries

| Method | Purpose |
|--------|---------|
| `DataTable.where-rows` | Predicate filter |
| `DataTable.sort-by` | Sort by column |
| `DataTable.select-columns` | Column projection |
| `DataTable.select-rows` | Row selection by index vector |

### DataTable Creation and Access

```lisp
;; Create
(setq t (new DataTable))
(==> t add-column "time" 'stamp)
(==> t add-column "symbol" 'string)
(==> t add-column "price" 'real)
(==> t append-row (list (timestamp-now) "AAPL" 150.25))

;; Access
(==> t get-column "price")      ;; returns idx1
(==> t print 10)                ;; pretty-print with timestamp/string formatting

;; Persist
(columnardb-save t "/data/trades/")
(setq t2 (columnardb-load "/data/trades/"))   ;; mmap'd
```

---

## Implementation Details

### Implementation Order (as built)

1. DataTable class with typed columns (pure Lush)
2. Amortized growth / append (capacity doubling)
3. Timestamp utilities (packages/timedate/, double seconds)
4. StringColumn with dictionary encoding (per-column pool + int32 codes)
5. ColumnarDB persistence (LCDB format, splayed directories, mmap loading)
6. Compiled C binary search and sort-check (idx-sort.lsh integration)
7. Grade/argsort functions (idx-d1i1sortup/down)
8. Compiled C I/O bridge (columnardb-io.lsh)
9. SQL-like query macro (columnardb-query.lsh)
10. GROUP BY aggregation (columnardb-groupby.lsh)
11. Compiled query kernels (columnardb-compiled.lsh)
12. Database class with catalog (columnardb-database.lsh)
13. Partitioned tables (columnardb-partition.lsh)
14. SQL tokenizer + parser (datatable-sql.lsh) — flat string → clause alist
15. Database.query() method — SQL string → _query-execute pipeline
16. LTOR backend integration (2026-03-16) — all 4 data-holding backends
    create Database wrappers and accept SQL queries via their handle-query functions

### Query Pipeline Architecture

The query planner uses a pipeline of stages:

```
Scan -> Filter -> Project -> Sort -> Limit -> Materialize
```

Plan representation is a nested list (tree):
```lisp
'(limit 100
   (sort "time" asc
     (filter (> "price" 100)
       (scan "trades"))))
```

The executor walks the tree bottom-up, materializing each stage.

Optimization rules applied:
1. Push filter before sort (reduce rows to sort)
2. Push filter before join (filter each side independently)
3. Use index when available (binary search on sorted columns)
4. Eliminate unused columns early (avoid loading unneeded columns)
5. Merge adjacent filters (single compiled scan loop)

### What Was Tried and Did Not Work

**IDX file format for persistence**: Lush's standard IDX format stores data in
big-endian byte order, but `storage-mmap` maps without byte-swapping.  On
x86/x86-64 this produces garbage.  The `idx-map.lsh` endianness check is
defeated by `fread-int` always byte-swapping.  Solution: the LCDB native-endian
format.

**Interpreted binary search**: The original `_cdb-bsearch-left-double`,
`_cdb-bsearch-right-double`, `_cdb-is-sorted-double`, and `_cdb-is-sorted-int`
were pure Lush.  These were replaced with compiled C functions from
`idx-sort.lsh` (`idx-d1bsearch-left`, `idx-d1bsearch-right`, `idx-d1sortedp`,
`idx-i1sortedp`).

**Missing grade/argsort**: The original plan claimed Lush had no native argsort.
The `idx-sort.lsh` library had paired sort functions (float+int, int+int) but
was missing double+int.  Adding `idx-d1i1sortup`/`idx-d1i1sortdown` resolved
this (2026-02-23).

### String Column Persistence Details

String columns serialize as two files:
- `col-N-codes.col`: integer codes in LCDB format (mmappable)
- `col-N-pool.lsh`: string pool as S-expression list (loaded into memory)

On load, the pool is read into heap memory and codes are mmap'd.  On append,
the existing pool is merged with new unique strings and the pool file is
rewritten.

### Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Save | O(n) per column | One pass write |
| Load (mmap) | O(1) startup + O(k) access | Demand-paged |
| Load (memory) | O(n) per column | One pass read |
| Append | O(m) for m new rows | Plus O(p) pool merge for string cols |
| Range query (sorted) | O(log n + k) | Binary search + result construction |
| Range query (unsorted) | O(n) | Linear scan |
| String filter after range | O(k) | Integer code comparison |

---

## Known Issues and Limitations

### String Pool Memory Accumulation

String column pools (HTable + reverse list) are always in heap memory, even for
mmap-loaded tables.  For typical cardinalities this is negligible (~30KB for
3,000 unique symbols).  In sessions loading many tables with high-cardinality
string columns, pools accumulate on the heap and cannot be paged out.

Potential remedies (not yet implemented):
1. **Flat binary pool format**: mmap'd offset array + string buffer; reverse
   lookup via mmap, forward lookup htable built lazily on first write.
2. **Lazy pool loading**: defer pool load until string values are actually
   needed (most queries operate on integer codes).

### Two Load Paths

`columnardb-load` (mmap, read-only) vs. `columnardb-load-mem` (malloc,
read-write) requires the user to decide upfront.  A copy-on-write scheme is
designed but not implemented: check `writablep` before writes, transparently
copy individual mmap'd columns to heap on first mutation, with optional
flush-back to disk + re-mmap.

### No Writable mmap

Lush's `storage_mmap` uses `PROT_READ | MAP_SHARED`.  In-place modification
of mapped columns would require adding `PROT_WRITE` support (a small C change
to `storage_mmap` or a new `storage_mmap_rw` function).  With writable mmap,
COW would only be needed for growth (append), since mmap'd storages cannot be
realloc'd.

### Endianness

LCDB files are native-endian only.  Little-endian files are unreadable on
big-endian machines and vice versa.  Cross-architecture portability would need
an export/import function.  This is acceptable since files live on the same
machine and Kerf has the same constraint.

### NULL Handling

Lush uses NaN for double nulls.  The query language skips NaN rows in
predicates (SQL three-valued logic semantics).  Integer NULL sentinel is not
yet standardized.

### Expression Columns

Computed columns like `(as (* "price" "qty") "notional")` in `db-select` are
not yet supported.  This would require per-row expression evaluation.

### No Row Deletion

`storage_realloc` only grows.  Row deletion requires either copying to a new
smaller table or maintaining a deleted-row bitmap with periodic compaction.
Kerf has the same constraint for disk tables.
