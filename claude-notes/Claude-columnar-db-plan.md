# Columnar Database for Lush: Design Plan

## Inspired by Kerf1, adapted to Lush's idx/storage model

---

## Part 1: Kerf1 Architecture Analysis

### How Kerf Works

Kerf is a columnar tick database where **every value is a K0 struct** with an
inline type tag, reference count, and power-of-2 slab allocation. This is
fundamentally different from Lush.

**Kerf's memory model:**
- Custom pool/slab allocator: all allocations rounded to power-of-2 sizes
- Every object carries a `K0` header: `{m, a, h, t, r, payload}` where
  `m` = log2(allocated_bytes), `t` = type tag, `r` = refcount
- Vectors are `K0` header + flexible array member `g[]`, with logical
  count `n` and capacity derived from `POW2(m)`
- Amortized O(1) append: when full, double allocation (m -> m+1)
- Copy-on-write via reference counting

**Kerf's table model:**
- A TABLE is a MAP with type overridden to TABLE
- MAP internals: `[INDEX, KEYS, VALUES, TRAITS]`
  - KEYS = list of column name strings
  - VALUES = list of column vectors (each independently typed/allocated)
  - INDEX = hash index for O(1) column name lookup
  - TRAITS = metadata (primary keys, foreign keys, defaults)
- Pure columnar: no row storage, each column is an independent typed vector
- Row count = max column length

**Kerf's timestamp:**
- Stored as `int64_t` nanoseconds since Unix epoch
- Type tag STAMP (scalar) / STAMPVEC (vector)
- Smart pretty-printing: date-only, time-only, or full datetime
- Component extraction: year, month, day, hour, minute, second, nano
- `bars()` function for time bucketing
- Arithmetic with relative datetime intervals

**Kerf's sort indexes:**
- `ATTR_SORTED` bit flag maintained incrementally on appends
- BTREE (AVL tree) indexes for O(log n) lookups
- HASH indexes for dictionary-encoded string columns
- "Pre-whered" optimization: WHERE on sorted/indexed columns uses
  binary search instead of linear scan
- `grade_up(x)` returns permutation vector for indirect sorting

**Kerf's persistence:**
- Entire virtual address space reserved at startup via mmap(PROT_NONE)
- Files mapped with MAP_SHARED|MAP_FIXED into reserved range
- Same K0 struct layout on disk as in memory (zero serialization cost)
- "Striped" (splay) tables: each column in a separate file/directory
- Growing disk objects: ftruncate + extend mapping + memmove inner data

### Why Kerf's Memory Model Is Incompatible with Lush

1. **Kerf's K0 is self-describing**: every allocation carries type/size
   metadata inline. Lush's `at` objects are untyped containers; the type
   lives in the storage class, not the data pointer.

2. **Kerf's pool allocator owns all memory**. Lush uses standard
   malloc/realloc/mmap. Kerf's reference counting and COW assume the
   pool allocator. You can't mix them.

3. **Kerf vectors are flexible array members** in the K0 struct.
   Lush idx objects are views into separate storage objects. An idx can
   be a strided view, a submatrix, or share storage with other indices.

4. **Kerf's persistence = mmapping K0 structs directly**. This only
   works because the K0 layout IS the file format. Lush storages already
   support mmap but the idx/storage split means the metadata (dimensions,
   strides) is separate from the data.

**Conclusion: We cannot embed Kerf as a library or share its allocator.
We build Kerf-inspired functionality on top of Lush's native idx/storage
system.**

---

## Part 2: Lush idx/Storage System Summary

### Key Structures

```c
struct storage {
  short srg_type;     // ST_F, ST_D, ST_I32, ST_I64, ST_U8, etc.
  short srg_flags;    // STS_MALLOC, STS_MMAP, STF_RDONLY, STF_UNSIZED
  intg  srg_size;     // number of elements
  gptr  srg_data;     // raw data pointer
  // ... alloc info (malloc addr or mmap addr/len)
};

struct index {
  int ndim;            // number of dimensions (1 for idx1)
  intg dim[MAXDIMS];   // size in each dimension
  intg mod[MAXDIMS];   // stride in each dimension
  intg offset;         // offset into storage
  struct storage *st;  // underlying storage
};
```

### Storage Types Available
- `ST_D` (real/double, 8 bytes)
- `ST_F` (flt/float, 4 bytes)
- `ST_I64` (int64_t, 8 bytes) -- **this is our timestamp candidate**
- `ST_I32` (int/int32_t, 4 bytes)
- `ST_I16` (short, 2 bytes)
- `ST_I8` (signed char, 1 byte)
- `ST_U8` (unsigned char, 1 byte)
- `ST_AT` (Lush at* pointers)
- `ST_P` (raw bytes)
- `ST_GPTR` (void pointers)

### Key Operations
- `storage_malloc(at*, size, clear)` -- allocate storage
- `storage_realloc(at*, newsize, clear)` -- grow storage (malloc only, not mmap)
- `storage_mmap(at*, FILE*, offset)` -- mmap a file as read-only storage
- `index_read_idx` / `index_write_idx` -- access data
- idx1 sort: `sort_idx` in idx1.c (quicksort variants)
- idx1 search: binary search via `idx_sortedp`, finding operations

### Constraints for Our Design
1. **No dynamic growth for mmap**: `storage_realloc` only works on
   STS_MALLOC storages. Mmap'd storages are read-only and fixed-size.
   Growing a persistent column requires remapping.

2. **idx1 is 1-dimensional but can be non-contiguous**: strides allow
   views, but for columnar data we want contiguous storage.

3. **No built-in idx resize**: Resizing requires `storage_realloc` on
   the underlying storage, then creating a new idx pointing to it.
   There is no `idx_append` primitive.

4. **ST_I64 exists**: Added during the 64-bit cleanup. This is perfect
   for nanosecond timestamps.

---

## Part 3: Design Plan -- Lush DataTable

### Stage 1: Core DataTable Object (Lush-level, no C changes)

Implement as a Lush class in `lsh/datatab/` package:

```lisp
(defclass DataTable object
  ;; Column metadata
  ((-obj- (HTable)) column-names)    ;; string -> column-index htable
  ((-idx1- (-int-)) column-types)    ;; ST_D, ST_I32, etc. per column
  ((-int-) num-columns)
  ((-int-) num-rows)

  ;; Column storage: list of idx1 objects, each independently typed
  ;; Accessed by index or by name through column-names htable
  ((-obj- (Pool)) columns)           ;; or just an AT-storage of idx1 refs

  ;; Sort metadata
  ((-idx1- (-int-)) sort-column)     ;; which column is sorted (-1 = none)
  ((-idx1- (-int-)) sort-order)      ;; permutation vector (grade_up result)
)
```

**Column storage approach:** Each column is a separate idx1 object backed
by its own storage. The DataTable holds a list (Lush list or AT-storage)
of these idx1 objects. Column types:
- `-flt-` columns -> idx1 of (-flt-)
- `-real-` columns -> idx1 of (-real-)  [double]
- `-int-` columns -> idx1 of (-int-)    [int32 or int64 depending on build]
- `-byte-` columns -> idx1 of (-ubyte-)
- Timestamp columns -> idx1 of (-int-) storing nanoseconds (I64 storage)
- String columns -> idx1 of AT storage holding string objects, or
  dictionary-encoded as idx1 of (-int-) + intern table

**Creating a table:**
```lisp
(setq t (new DataTable))
(==> t add-column "time" 'stamp)     ;; idx1 of I64, nanosecond epoch
(==> t add-column "price" 'real)     ;; idx1 of double
(==> t add-column "volume" 'int)     ;; idx1 of int
(==> t add-column "symbol" 'string)  ;; dictionary-encoded
```

**Key methods:**
- `add-column name type` -- add a column, allocate initial storage
- `append-row values` -- append a row (grow all columns)
- `get-column name` -- return the idx1 for a column
- `get-row n` -- return a list of values at row n
- `num-rows` / `num-cols` -- dimensions
- `print-table &optional max-rows` -- pretty-print

### Stage 2: Growth Strategy

**Problem:** Lush idx1 backed by malloc storage can be grown via
`storage_realloc`, but there's no high-level `idx-append`. We need
an amortized growth strategy.

**Solution: Capacity vs. Size**

Each column tracks logical size (num-rows) separately from allocated
capacity. When appending:

```lisp
(defmethod DataTable append-row (values)
  ;; Check capacity
  (when (>= num-rows capacity)
    ;; Double capacity (or use 1.5x growth factor)
    (let ((new-cap (max 64 (* 2 capacity))))
      (each ((col columns))
        (storage-realloc (idx-storage col) new-cap))))
  ;; Write values into row num-rows
  (each ((col columns) (val values) (i (range num-columns)))
    (col num-rows val))
  (incr num-rows))
```

This gives amortized O(1) append, same as Kerf's doubling strategy.

**Alternative for bulk loading:** Pre-allocate to known size, fill via
direct idx1 writes, then mark as populated. This avoids all reallocation
overhead.

### Stage 3: Timestamp Column Type

**Design:** A timestamp column is an idx1 of I64 storage containing
nanoseconds since Unix epoch. At the Lush level, we provide:

```lisp
(defclass Timestamp object
  ;; Wraps an I64 value representing nanos since epoch
  ((-long-) nanos)
)
```

**Functions:**
- `(timestamp-now)` -- returns current time as nanosecond int64
- `(timestamp-from-date y m d)` -- date to nanos
- `(timestamp-from-datetime y m d h min s ns)` -- full datetime to nanos
- `(timestamp-to-string nanos &optional fmt)` -- pretty-print
- `(timestamp-components nanos)` -- return (year month day hour min sec nano)
- `(timestamp-diff a b)` -- subtract, return nanos
- `(timestamp-add nanos interval)` -- add duration
- `(timestamp-bars col interval)` -- bucket timestamps (Kerf-style)

**Pretty-printing integration:** When displaying a DataTable, columns
marked as 'stamp type use `timestamp-to-string` for display instead of
printing raw int64 values.

**C-level helper (optional, Stage 3b):** For performance-critical
timestamp operations (bars, component extraction on entire columns),
add a small C file `src/timestamp.c` with DX functions:

```c
DX(xtimestamp_now)     // clock_gettime -> int64 nanos
DX(xtimestamp_bars)    // vectorized bucketing
DX(xtimestamp_components) // vectorized extraction
```

### Stage 4: Sort Indexes and Querying Primitives

**Sort order (grade vector):**

Kerf's `grade_up` is equivalent to computing a permutation that sorts the
column. Lush already has `idx-sortdown` / `idx-sortup` in idx1.c. We
need a `grade-up` that returns the permutation vector:

```lisp
(defmethod DataTable sort-by (column-name &optional descending)
  ;; Compute permutation vector
  (let* ((col (==> this get-column column-name))
         (grade (int-array (num-rows))))
    ;; Fill grade with 0..n-1
    (idx-bloop ((g grade) (i (range num-rows))) (g i))
    ;; Sort grade by comparing col values
    ;; ... use idx1 argsort or implement via C primitive
    (setq sort-order grade)
    (setq sort-column column-name)))
```

**ATTR_SORTED tracking:** Maintain a per-column boolean flag. Set on
initial sorted load, checked on append (compare with last element),
cleared if append breaks order.

**Filtering (WHERE equivalent):**

```lisp
(defmethod DataTable where (column-name predicate)
  ;; Returns a new DataTable with matching rows
  ;; If column is sorted and predicate is range-based, use binary search
  ;; Otherwise linear scan
  ...)
```

**Selection (SELECT equivalent):**

```lisp
(defmethod DataTable select (column-names &optional where-fn)
  ;; Returns a new DataTable with only the named columns
  ;; Optionally filtered by where-fn
  ...)
```

### Stage 5: Dictionary-Encoded String Columns

For string columns with many repeated values (like stock symbols),
store as:
- An intern table: idx1 of AT storage containing unique strings
- A column of int32 indices into the intern table

This matches Kerf's HASH type (intern/enum encoding). Benefits:
- O(1) equality comparison (compare integers)
- Much less memory for repeated strings
- Fast GROUP BY (partition by integer key)

```lisp
(defclass DictColumn object
  ((-obj- (HTable)) intern-table)    ;; string -> integer
  ((-idx1- (-int-)) codes)           ;; per-row integer codes
  ((-idx1- (-obj-)) strings)         ;; integer -> string (reverse map)
  ((-int-) next-code)
)
```

### Stage 6: Persistence via mmap

**Read-only mapping (simplest):**
Each column saved as a flat binary file. Load via `storage-mmap`:

```
tabledir/
  meta.lsh          ;; column names, types, row count
  col-0.bin         ;; raw doubles for price column
  col-1.bin         ;; raw int64s for timestamp column
  col-2.bin         ;; raw int32s for dict-encoded strings
  col-2-dict.lsh    ;; string intern table for column 2
```

This matches Kerf's "striped" (splay) table concept: each column in
its own file, independently mappable.

**Read-write mapping (harder, future work):**
Lush's `storage_mmap` is read-only (`PROT_READ, MAP_SHARED`). For
writable mmap, we'd need:
1. A new C function `storage_mmap_rw` using `PROT_READ|PROT_WRITE`
2. Growing requires `ftruncate` + `mremap` (Linux) or `munmap`+`mmap`
3. This is the most complex part and should be deferred

---

## Part 4: Implementation Stages Summary

| Stage | What | Scope | Dependencies |
|-------|------|-------|-------------|
| 1 | DataTable class with typed columns | Pure Lush | None |
| 2 | Amortized growth (append rows) | Pure Lush | Stage 1 |
| 3a | Timestamp utilities (Lush level) | Pure Lush | Stage 1 |
| 3b | Timestamp C primitives | Small C file | Stage 3a |
| 4 | Sort/grade, WHERE, SELECT | Lush + maybe C | Stages 1-2 |
| 5 | Dictionary-encoded strings | Pure Lush | Stage 1 |
| 6a | Read-only mmap persistence | Pure Lush | Stages 1-5 |
| 6b | Read-write mmap persistence | C changes | Stage 6a |

**Recommended order:** 1 -> 2 -> 3a -> 4 -> 5 -> 3b -> 6a -> 6b

---

## Part 5: Show-Stopper Analysis

### Definite Show-Stoppers: None

There are no fundamental blockers. The design works entirely within
Lush's existing capabilities.

### Significant Challenges

1. **No native argsort / grade_up in Lush.**
   Lush's `idx-sortup` sorts in-place but doesn't return a permutation
   vector. We need to implement argsort, either:
   - In Lush (slow for large columns)
   - As a new C primitive `idx-argsort` (preferred)
   This is not a show-stopper but is required for Stage 4.

2. **storage_realloc only grows, never shrinks.**
   This means tables can only grow. Deleting rows requires either:
   - Copying to a new smaller table
   - Maintaining a "deleted" bitmap and periodically compacting
   Not a show-stopper; Kerf has the same constraint for disk tables.

3. **String columns are expensive without dictionary encoding.**
   Lush AT-storage holds Lush `at*` pointers (24 bytes each + string
   heap allocation). For millions of rows this is heavy. Dictionary
   encoding (Stage 5) is essential for string-heavy tables.

4. **No writable mmap in Lush.**
   `storage_mmap` uses `PROT_READ, MAP_SHARED`. For persist-on-write
   we need `PROT_READ|PROT_WRITE`. This requires C changes but is
   straightforward (Stage 6b). Read-only persistence (Stage 6a) works
   today.

5. **idx resize requires going through storage.**
   There's no `idx-resize` or `idx-append`. Users must call
   `storage-realloc` on the underlying storage, then the existing idx
   automatically sees the new data. This is clunky but workable;
   the DataTable class encapsulates it.

6. **Lush's intg type is platform-dependent.**
   On 64-bit builds, `intg` is `long` (64-bit). On 32-bit, it's `int`
   (32-bit). The `ST_I64` storage type exists and always gives 64-bit
   integers, which is what we need for timestamps. But Lush's `-int-`
   type in compiled code maps to `intg`, not necessarily `int64_t`.
   For timestamps we specifically need `ST_I64` / `-long-` storage.
   **Status:** This works on 64-bit systems (intg = long = 64 bits)
   which is the only target after the cleanup work.

7. **Performance of Lush-level loops for large tables.**
   Pure Lush `idx-bloop` for filtering/aggregation on million-row
   tables will be slow. Lush's compiled code system (`dhc-make`)
   compiles Lush to C for such cases. Performance-critical operations
   (WHERE scan, aggregation, bars) should eventually be either
   compiled or implemented as C primitives. Not a show-stopper for
   initial implementation.

### Non-Issues

- **Kerf's pool allocator incompatibility:** We don't use it. We
  build on Lush's malloc/mmap.
- **Kerf's COW:** Not needed. Lush has GC-based memory management.
  Tables own their column storages.
- **Thread safety:** Lush is single-threaded. No need for mutexes
  on table operations.

---

## Part 6: Kerf-to-Lush Column Type Mapping

| Kerf Type | Kerf Storage | Lush Equivalent | Notes |
|-----------|-------------|-----------------|-------|
| INT | int64_t vector | idx1 of ST_I64 | Direct mapping via -long- |
| FLOAT | double vector | idx1 of ST_D | Direct mapping via -real- |
| STAMP | int64_t nanos | idx1 of ST_I64 | Same storage, different display |
| CHARVEC | char vector | idx1 of ST_U8 | Or Lush string object |
| HASH (intern) | int+dict | idx1 of ST_I32 + intern table | Dictionary encoding |
| BTREE (index) | AVL tree | idx1 of ST_I32 (grade vector) | Simpler: just precomputed sort |
| ZIP (compressed) | LZ4 blocks | Not planned initially | Could add later |
| LIST (mixed) | K0 pointer array | idx1 of ST_AT | Slow, avoid if possible |
| TABLE | MAP of columns | DataTable class | Our design target |

### Key Differences from Kerf

1. **Kerf columns grow automatically.** Lush columns need explicit
   capacity management (our Stage 2 handles this).

2. **Kerf has one type for everything (K0).** Lush has separate storage
   types and idx views. This is actually an advantage for columnar
   data: no type-tag overhead per element.

3. **Kerf's timestamps are just INT with a type tag.** In Lush, we use
   the same approach: I64 storage + metadata flag. The type distinction
   lives in the DataTable metadata, not in the storage.

4. **Kerf's sort attribute is per-vector.** In Lush, we track it per-
   column in the DataTable metadata.

5. **Kerf's disk format IS the memory format.** In Lush, the storage
   data (raw doubles, raw int64s) is the same in memory and on disk,
   but the metadata (column names, types, dimensions) needs separate
   serialization. This is actually cleaner -- the binary column files
   are portable, and the metadata is a small Lush S-expression file.

---

## Part 7: API Sketch

```lisp
;; ===== Creating tables =====
(setq t (new DataTable))
(==> t add-column "time" 'stamp)
(==> t add-column "symbol" 'string)    ;; dict-encoded
(==> t add-column "price" 'real)
(==> t add-column "volume" 'int)

;; ===== Appending data =====
(==> t append-row (list (timestamp-now) "AAPL" 150.25 1000))
(==> t append-row (list (timestamp-now) "GOOG" 2800.50 500))

;; ===== Bulk loading =====
(let ((times (long-array 1000000))
      (prices (double-array 1000000)))
  ;; ... fill arrays ...
  (==> t set-column "time" times)
  (==> t set-column "price" prices))

;; ===== Accessing columns =====
(setq prices (==> t get-column "price"))   ;; returns idx1
(prices 42)                                 ;; element access

;; ===== Display =====
(==> t print 10)
;; time                    symbol  price    volume
;; 2026.02.19T14:30:00.123 AAPL    150.25   1000
;; 2026.02.19T14:30:00.456 GOOG    2800.50  500
;; ... (10 rows shown of 1000000)

;; ===== Sorting =====
(==> t sort-by "time")
(==> t sort-by "price" t)  ;; descending

;; ===== Filtering =====
(setq filtered (==> t where "price" (lambda (x) (> x 100))))

;; ===== Selection =====
(setq sub (==> t select '("time" "price")))

;; ===== Persistence =====
(==> t save-striped "/data/trades/")
(setq t2 (DataTable-load-striped "/data/trades/"))  ;; mmap'd columns

;; ===== Timestamps =====
(timestamp-to-string (timestamp-now))
;; "2026.02.19T14:30:00.123456789"

(timestamp-bars (==> t get-column "time") '1m)  ;; 1-minute bars
```

---

## Part 8: Open Questions for User Decision

1. **Should we use Lush's compiled code system (dhc-make) for
   performance-critical operations?** This would make WHERE scans and
   aggregations much faster but adds compilation dependency.

2. **Should string columns default to dictionary encoding or AT storage?**
   Dictionary is faster/smaller but adds complexity. AT storage is
   simpler but slow for large tables.

3. **How should we handle NULL/missing values?** Options:
   - NaN for float columns, MIN_INT64 for integer columns (Kerf approach)
   - Separate null bitmap per column (Arrow approach)
   - Lush NIL for AT-storage columns

4. **Should the query API use method calls or a mini-DSL?**
   Method calls are simpler initially; a DSL could come later.

5. **Naming: DataTable, Table, or something else?** "Table" conflicts
   with common usage. "DataTable" is explicit. "Frame" (like R/pandas)?
