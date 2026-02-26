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

### IDX Sorting and Binary Search Functions (`lsh/libidx/idx-sort.lsh`)

The `idx-sort.lsh` library provides compiled C (via dhc-make) heap sort and
binary search functions for vectors.  These are critical building blocks for
columnar database operations.

**Binary search** (requires sorted ascending input):
- `(idx-d1bsearch <vec> <val>)` -- index of element <= val in double vector
- `(idx-f1bsearch <vec> <val>)` -- same for float
- `(idx-i1bsearch <vec> <val>)` -- same for int

**In-place sorting:**
- `(idx-d1sortup <vec>)` / `(idx-d1sortdown <vec>)` -- double ascending/descending
- `(idx-f1sortup <vec>)` / `(idx-f1sortdown <vec>)` -- float
- `(idx-i1sortup <vec>)` / `(idx-i1sortdown <vec>)` -- int

**Paired sort (grade/argsort)** -- sorts data vector and carries an
accompanying int index vector with the same permutation.  To compute a
grade (permutation vector): fill the int vector with 0..n-1, copy the data
column, then call the paired sort.  The int vector becomes the grade.
- `(idx-d1i1sortup <data> <grade>)` / `(idx-d1i1sortdown ...)` -- **double** (timestamps, reals)
- `(idx-f1i1sortup <data> <grade>)` / `(idx-f1i1sortdown ...)` -- float
- `(idx-i1i1sortup <data> <grade>)` / `(idx-i1i1sortdown ...)` -- int

**Note:** `idx-d1i1sortup` is the primary grade function for timestamp and
real columns.  Since timestamps are stored as doubles (seconds since epoch),
this is the timestamp grade sort.  Added 2026-02-23.

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
column.  The `idx-sort.lsh` library provides compiled C paired sort
functions that serve exactly this purpose.  For double/timestamp columns,
`idx-d1i1sortup` sorts a double vector and carries an int index vector
with the same permutation.

```lisp
(defmethod DataTable sort-by (column-name &optional descending)
  ;; Compute permutation vector using compiled paired sort
  (let* ((col (==> this get-column column-name))
         (nrows (==> this num-rows))
         (data-copy (double-matrix nrows))
         (grade (int-matrix nrows)))
    ;; Copy data (paired sort is destructive)
    (for (i 0 (1- nrows)) (data-copy i (col i)))
    ;; Fill grade with 0..n-1
    (for (i 0 (1- nrows)) (grade i i))
    ;; Compiled C heap sort: O(n log n), carries grade with same permutation
    (if descending
      (idx-d1i1sortdown data-copy grade)
      (idx-d1i1sortup data-copy grade))
    ;; grade now contains the permutation vector
    (setq sort-order grade)
    (setq sort-column column-name)))
```

For int columns, use `idx-i1i1sortup`/`idx-i1i1sortdown` instead.

**Binary search on sorted columns:** The `idx-d1bsearch` function from
`idx-sort.lsh` provides compiled C binary search on sorted double vectors.
It returns the index of the element <= the target value.  The columnardb
range query functions (`_cdb-bsearch-left-double`, `_cdb-bsearch-right-double`)
currently implement this in interpreted Lush and should be migrated to use
`idx-d1bsearch` as the core search, with thin wrappers for lower-bound
(>=) semantics.

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

### Stage 5: StringColumn — Dictionary-Encoded Interned Strings

String columns require special treatment. Storing millions of Lush string
objects in an AT-storage idx1 would cost ~24 bytes per reference plus
per-string heap allocation — prohibitive for large tables. Kerf solves
this with per-column string interning, and we adopt the same approach.

#### Kerf's String Interning Model (reference)

From Kerf's source and the blog post
(https://getkerf.wordpress.com/2016/02/22/string-interning-done-right/):

- An "intern" object is a HASH with two sub-objects:
  - `INDEX`: a hashset containing the unique strings (the "pool")
  - `KEYS`: an integer vector where each entry is a code pointing into INDEX
- `cow_intern_add(x, y)` inserts string `y` into the hashset, gets back
  an integer position `p`, then appends `p` to the integer KEYS vector
- `klook_intern(z, i)` retrieves row `i`: reads integer code from
  `KEYS[i]`, then looks up the actual string in `INDEX[code]`
- Key design choice: **per-column local pools**, not a global pool.
  Each intern column is self-contained — the pool travels with the data.
  This means serialization/mmap requires zero translation: the same
  structure works in memory, on disk, and over the network.
- Overhead for the pool is small: ~3,000 NASDAQ ticker symbols × ~10
  bytes average = ~30KB for the entire pool. Even with high cardinality
  (many unique strings), the overhead of the hashset is negligible
  compared to the savings from deduplication.

#### Lush StringColumn Design

The `StringColumn` is a **new data structure** that acts like an idx1
of strings at the Lush command line, but internally stores:

1. A **string pool** (htable + reverse-lookup array) containing only
   the unique strings
2. An **idx1 of int32 codes** (one per row) indexing into the pool

```lisp
(defclass StringColumn object
  ;; Forward map: string -> integer code
  ;; Uses Lush's existing HTable for O(1) lookup
  ((-obj- (HTable)) str-to-code)

  ;; Reverse map: integer code -> string
  ;; An AT-storage idx1 holding the unique strings
  ;; str-to-code and code-to-str are always consistent mirrors
  ((-idx1- (-obj-)) code-to-str)

  ;; Per-row data: each row stores an int32 code, not the string itself
  ;; Same capacity-doubling growth strategy as other DataTable columns
  ((-idx1- (-int-)) codes)

  ;; Pool bookkeeping
  ((-int-) pool-size)       ;; number of unique strings in pool
  ((-int-) pool-capacity)   ;; allocated capacity of code-to-str
  ((-int-) num-rows)        ;; logical row count
  ((-int-) capacity)        ;; allocated capacity of codes
)
```

#### Core Operations

**Append a string value:**
```lisp
(defmethod StringColumn append (s)
  ;; 1. Check if string is already in pool
  (let ((code (==> str-to-code get s)))
    (when (not code)
      ;; 2. New string: add to pool
      (when (>= pool-size pool-capacity)
        (grow-pool))              ;; double code-to-str capacity
      (setq code pool-size)
      (==> str-to-code put s code)
      (code-to-str code s)       ;; store string in reverse map
      (incr pool-size))
    ;; 3. Append code to per-row array
    (when (>= num-rows capacity)
      (grow-codes))              ;; double codes capacity
    (codes num-rows code)
    (incr num-rows)))
```

**Read row i (looks like a string to the user):**
```lisp
(defmethod StringColumn get (i)
  ;; Two-step lookup: row -> code -> string
  ;; Same pattern as Kerf's klook_intern
  (code-to-str (codes i)))
```

**Equality test (fast path — compare codes, not strings):**
```lisp
(defmethod StringColumn rows-equal (i j)
  ;; Integer comparison, O(1)
  (= (codes i) (codes j)))
```

**Filter by string value (WHERE column = "BTC"):**
```lisp
(defmethod StringColumn where-eq (s)
  ;; 1. Look up code for target string: O(1)
  (let ((target-code (==> str-to-code get s)))
    (when target-code
      ;; 2. Scan codes array comparing integers, not strings
      ;; Much faster than strcmp on every row
      (let ((result (int-array 0)) (n 0))
        (idx-bloop ((c codes) (i (range num-rows)))
          (when (= c target-code)
            ;; collect matching row index
            ...))
        result))))
```

**GROUP BY (partition by string value):**
```lisp
(defmethod StringColumn group-by ()
  ;; Returns a list of (string . row-indices) pairs
  ;; Since codes are small integers, we can use them as array indices
  ;; for O(n) grouping — no hashing needed at group time
  (let ((groups (make-array pool-size)))
    (idx-bloop ((c codes) (i (range num-rows)))
      ;; Append row index i to groups[c]
      ...)
    groups))
```

#### Memory Layout

For a 10-million row table with a "symbol" column containing 3,000
unique ticker symbols (~10 chars average):

| Component | Kerf (reference) | Lush StringColumn |
|-----------|-----------------|-------------------|
| Per-row storage | 4 bytes (int32 code) | 4 bytes (int32 code) |
| Total row data | 40 MB | 40 MB |
| Pool (unique strings) | ~30 KB hashset | ~30 KB HTable + ~30 KB AT array |
| **Total** | **~40.03 MB** | **~40.06 MB** |

Compare with naive AT-storage idx1: 10M × 24 bytes (pointer) + 10M ×
~26 bytes (string objects with headers) = ~500 MB. **Dictionary encoding
saves 12x memory.**

Even with high cardinality (e.g., 1M unique strings out of 10M rows),
the pool is ~10 MB and total is ~50 MB, still far less than the naive
500 MB approach. The overhead of maintaining the pool is acceptable in
all cases, as the user specified.

#### Integration with DataTable

The DataTable class treats StringColumn as a column type alongside idx1:

```lisp
(defmethod DataTable add-column (name type)
  (cond
    ((= type 'string)
     ;; Create StringColumn instead of idx1
     (let ((col (new StringColumn)))
       (==> columns add col)
       ...))
    ((= type 'stamp)
     ;; idx1 of ST_I64
     ...)
    (t
     ;; numeric: idx1 of appropriate type
     ...)))
```

When the user accesses a string column, the StringColumn's `get` method
returns a string, so it **looks like an idx1 of strings at the Lush
prompt**:

```lisp
(setq sym-col (==> t get-column "symbol"))
(==> sym-col get 0)          ;; => "AAPL"
(==> sym-col get 1)          ;; => "BTC"
(==> t print 5)
;; time                    symbol  price    volume
;; 2026.02.19T14:30:00.123 AAPL    150.25   1000
;; 2026.02.19T14:30:00.456 BTC     42000.0  200
```

The user never sees integer codes. The pretty-printer resolves codes
to strings transparently.

#### Persistence

StringColumn serializes to two files per column:

```
col-2-codes.bin     ;; raw int32 codes array (mmappable)
col-2-pool.lsh      ;; string pool as S-expression list
```

The codes file is a flat binary that can be mmap'd directly as an
idx1 of int32 — same as any numeric column. The pool file is small
(kilobytes for typical cardinality) and loaded into memory on open.

This matches Kerf's self-contained design: the pool travels with the
column. No global interning state, no external dependencies.

#### Sorting StringColumn

Sorting a StringColumn can work two ways:

1. **Lexicographic:** Sort the pool alphabetically, remap codes to
   reflect the new order, then sort the codes array. Or simpler:
   compute grade vector by comparing `code-to-str[codes[i]]` values.

2. **By insertion order:** Just sort the integer codes directly.
   Since codes are assigned in insertion order, this groups identical
   strings together (useful for GROUP BY optimization).

The ATTR_SORTED flag applies to the codes array. If codes are sorted,
WHERE-eq can use binary search on the codes (find the target code,
then binary search for the range of that code in the sorted array).

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
| 5 | StringColumn (interned dict-encoded strings) | Pure Lush | Stage 1 |
| 6a | Read-only mmap persistence | Pure Lush | Stages 1-5 |
| 6b | Read-write mmap persistence | C changes | Stage 6a |

**Recommended order:** 1 -> 2 -> 3a -> 4 -> 5 -> 3b -> 6a -> 6b

---

## Part 5: Show-Stopper Analysis

### Definite Show-Stoppers: None

There are no fundamental blockers. The design works entirely within
Lush's existing capabilities.

### Significant Challenges

1. **~~No native argsort / grade_up in Lush.~~** **RESOLVED.**
   The `idx-sort.lsh` library provides paired sort functions that serve
   as grade/argsort: `idx-d1i1sortup`, `idx-f1i1sortup`, `idx-i1i1sortup`
   (and their sortdown variants).  These sort a data vector and carry an
   accompanying int index vector with the same permutation.  To get a
   grade vector: fill an int vector with 0..n-1, copy the data, call the
   paired sort.  The int vector becomes the permutation.  These are
   compiled C (heap sort via dhc-make), not interpreted Lush.
   `idx-d1i1sortup` is the primary grade function for timestamp and real
   columns (added 2026-02-23).  No new C primitives needed.

2. **storage_realloc only grows, never shrinks.**
   This means tables can only grow. Deleting rows requires either:
   - Copying to a new smaller table
   - Maintaining a "deleted" bitmap and periodically compacting
   Not a show-stopper; Kerf has the same constraint for disk tables.

3. **String columns are expensive without dictionary encoding.**
   Lush AT-storage holds Lush `at*` pointers (24 bytes each + string
   heap allocation). For millions of rows this is heavy. **Resolved:**
   Stage 5 defines the StringColumn class with per-column interned
   dictionary encoding inspired by Kerf's HASH/intern type. This
   reduces 500 MB of string data to ~40 MB for typical use cases.

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
| HASH (intern) | int+dict | StringColumn (HTable + idx1 of ST_I32) | Per-column pool, see Stage 5 |
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

2. **~~Should string columns default to dictionary encoding or AT storage?~~**
   **Resolved:** All string columns use the StringColumn class with
   per-column dictionary encoding (local string pool + int32 codes).
   The overhead is acceptable even for high-cardinality columns, and
   the user explicitly requested this as a new data structure rather
   than an idx1 of strings.

3. **How should we handle NULL/missing values?** Options:
   - NaN for float columns, MIN_INT64 for integer columns (Kerf approach)
   - Separate null bitmap per column (Arrow approach)
   - Lush NIL for AT-storage columns

4. **Should the query API use method calls or a mini-DSL?**
   Method calls are simpler initially; a DSL could come later.

5. **Naming: DataTable, Table, or something else?** "Table" conflicts
   with common usage. "DataTable" is explicit. "Frame" (like R/pandas)?

---

## Part 9: ColumnarDB Implementation (COMPLETED)

### Package: `packages/columnardb/`

**Status:** Fully implemented and tested (101 tests, 0 failures).

**Files:**
```
packages/columnardb/
  columnardb.lsh          Main module: save, load, load-mem, append, range queries
  columnardb-io.lsh       Compiled C bridge: native-endian binary I/O
  C/columnardb_io.c       Auto-generated DH stubs
  tests/run-all.lsh       101-test staged test suite
```

### Architecture Decisions

#### 1. Native-Endian Binary Format (not IDX)

**Problem discovered:** Lush's standard IDX file format stores data in
big-endian (Sparc standard) byte order, with `fwrite-int`/`fwrite-real`
byte-swapping on little-endian machines. The `storage-mmap` function
maps files directly without byte-swapping.  This means `mmap-idx1-real`
and friends produce garbage on x86/x86-64 because the mapped data is
big-endian but the CPU reads it as little-endian.

The `idx-map.lsh` code has a check for this but it's defeated by
`fread-int` always byte-swapping (even when checking the magic number),
so it incorrectly concludes the file is same-endian.

**Solution:** A custom native-endian file format ("LCDB") with a 24-byte
header followed by raw native-byte-order element data:

```
Bytes  0-3:  Magic "LCDB" (0x4C434442)
Bytes  4-7:  Version (uint32 native) = 1
Bytes  8-15: Element count (int64 native)
Bytes 16-19: Type code (uint32 native): 1=double, 2=int32
Bytes 20-23: Reserved (zeros)
Bytes 24+:   Raw element data, native byte order
```

The 24-byte header is exactly 3 doubles wide, giving natural alignment
for the data section.  `storage-mmap` with offset 24 maps the data
directly into usable idx storage.

**Tradeoff:** Files are not portable across architectures (little-endian
files won't read correctly on big-endian machines and vice versa).  This
is acceptable because:
- The files live on the same machine they were created on
- Kerf has the same constraint (its K0 format is always native endian)
- Cross-architecture portability can be added later via an export/import
  function that converts to IDX or another portable format

The compiled C functions use `dhc-make` to generate native code at load
time.  This is the first use of compiled I/O in the columnardb package;
the datatable's CSV reader already established this pattern.

#### 2. Splayed Directory Layout

Each DataTable persists as a directory with one file per column:

```
tabledir/
  meta.lsh              S-expression: schema, row count, sort info
  col-0.col             Native binary for column 0
  col-1.col             Native binary for column 1
  col-2-codes.col       String column codes (int32 binary)
  col-2-pool.lsh        String column pool (S-expression list)
```

**Why S-expression for metadata and string pools?**
- Small files (metadata is a few hundred bytes, pools are a few KB)
- Human-readable and editable
- Lush's `read`/`print` handle arbitrary nested lists natively
- No custom parser needed

**Why binary for column data?**
- Column data can be large (millions of rows × 8 bytes = tens of MB)
- Must be mmappable for zero-copy access
- Native binary is the only format that supports mmap

**Column naming convention:** `col-N.col` using the column index (not
name) to avoid filesystem special character issues in column names.
Names are stored in `meta.lsh`.

#### 3. mmap Loading (Zero-Copy)

`columnardb-load` uses `storage-mmap` with offset 24 (skipping the LCDB
header) to map column files directly.  The OS kernel handles paging:
- Only accessed pages are loaded from disk
- Pages can be evicted and re-loaded transparently
- Multiple processes can share the same physical pages

This matches Kerf's `MAP_SHARED` approach.  The loaded DataTable is
effectively read-only (mmap'd storages have `STF_RDONLY` flag set).

**Empty table handling:** 0-row tables don't write column files (you can't
create a 0-element Lush storage).  Loading creates placeholder 1-element
arrays that are never accessed.

#### 4. Sorted Column Tracking

On save, each numeric/stamp column is scanned to check if it's sorted
(ascending, non-strict <=).  Sorted column indices are stored in the
metadata under the `sorted` key.

**Design choice: metadata-level tracking, not per-column attribute.**
Unlike Kerf's `ATTR_SORTED` bit on the vector itself, we store sort info
in the table metadata.  This is because:
- Lush idx objects don't have user attribute slots
- The metadata travels with the data (it's in the same directory)
- Sort status can change on append (tracked across operations)

**Sort preservation on append:** When appending, a previously-sorted
column stays sorted if:
1. The appended data is itself sorted
2. The first value of the new data >= the last value of the old data

This handles the common case of appending chronologically-ordered data
to a time-series table.

#### 5. Binary Search for Range Queries

`columnardb-range` checks the metadata for sort status and uses binary
search (O(log n)) on sorted columns vs. linear scan (O(n)) on unsorted.

The binary search uses two helper functions:
- `_cdb-bsearch-left-double`: finds leftmost index where value >= target
- `_cdb-bsearch-right-double`: finds rightmost index where value <= target

These are standard lower-bound/upper-bound binary searches.  The range
`[lo, hi]` maps to indices `[left, right]` where all values in that
index range satisfy `lo <= value <= hi`.

**Typical query pattern (time-series + symbol filter):**
```lisp
(let ((meta (columnardb-meta path))
      (dt (columnardb-load path)) )
  ;; Step 1: binary search on sorted timestamp column -> O(log n)
  (let ((indices (columnardb-range dt "time" start-time end-time meta)))
    ;; Step 2: extract sub-table for the time range
    (let ((sub (==> dt select-rows indices)))
      ;; Step 3: filter by interned string (integer comparison) -> O(k)
      (==> sub where '= "symbol" "AAPL") ) ) )
```

This gives O(log n + k) where k is the number of rows in the time range,
compared to O(n) for a full table scan.

#### 6. String Column Persistence

String columns serialize as two files:
- `col-N-codes.col`: integer codes in LCDB native binary format
- `col-N-pool.lsh`: string pool as an S-expression list

On load, the pool is read into memory (it's small — thousands of unique
strings at most) and the codes are mmap'd.  A new StringColumn is
reconstructed via `build-from-codes`.

On append, the existing pool is read, new strings are checked against it,
new unique strings are added if needed, and the pool file is rewritten.
This maintains the Kerf-style "pool travels with the data" property.

### Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Save | O(n) per column | One pass to copy + write |
| Load (mmap) | O(1) startup + O(k) for k accessed rows | Pages loaded on demand |
| Load (memory) | O(n) per column | One pass to read |
| Append | O(m) for m new rows | Plus O(p) for pool merge on string cols |
| Range query (sorted) | O(log n + k) | Binary search + result construction |
| Range query (unsorted) | O(n) | Linear scan |
| String filter after range | O(k) | Integer comparison on dict codes |

### Known Issue: String Pool Memory Accumulation

String column pools (the `str-to-code` htable and `code-to-str` list)
are always read into heap memory, even for mmap-loaded tables.  The
integer codes array is mmap'd, but the pool — which maps codes back
to strings — is an S-expression file that gets parsed into Lush objects.

For typical cardinalities this is negligible: 3,000 unique ticker
symbols at ~10 bytes average = ~30KB for the pool.  But in a long
session loading many tables with high-cardinality string columns (e.g.,
millions of unique user IDs across dozens of tables), the pools would
accumulate on the heap and never be paged out by the OS (since they're
malloc'd, not mmap'd).

**Potential remedies:**

1. **Flat binary pool format:** Store pools as a concatenated null-
   terminated string buffer + an int32 offset array, both in LCDB
   native-endian format.  Both files could be mmap'd.  Reverse lookup
   (code → string) becomes: read offset[code] from mmap'd array, read
   string starting at that offset from mmap'd buffer.  Forward lookup
   (string → code) still needs an in-memory htable, but this could be
   built lazily on first write, since read-only tables only need
   reverse lookup (for display/export — WHERE filtering compares codes).

2. **Lazy pool loading:** Don't load the pool at all on initial mmap
   load.  Most query operations (range on timestamp, filter by code)
   never need string values — they work entirely on integer codes.
   Only load the pool when `get` is called on a string column or when
   `print-table` formats output.

3. **Pool LRU eviction:** Track which pools have been accessed recently
   and drop unreferenced ones (set pool slots to nil, rebuild from disk
   on next access).  This requires reference counting or weak references
   which Lush doesn't have natively, so it would need to be managed
   manually at the session/query level.

Remedy #1 is the most Kerf-like (everything mmap'd) but requires a new
pool file format.  Remedy #2 is simpler and handles the common case
(query by time range + code comparison, only format results at the end).

### Design Question: Copy-on-Write vs. Separate Load Paths

The current design has two load functions:
- `columnardb-load` — mmap, read-only, OS manages paging
- `columnardb-load-mem` — malloc, read-write, all in heap

This is awkward.  The user has to decide upfront whether they'll need to
mutate data.  A better model would be copy-on-write (COW): load
everything mmap'd, and transparently copy individual columns to heap
memory only when a write is attempted.

**How Kerf does COW:**

Kerf's K0 header has a reference count field `r`.  Objects that are
sub-parts of an mmap'd file have `r = TENANT_REF_SIGNAL (-1)`.  The
`CAN_WRITE(x)` macro checks: if `r == 1` (sole owner) or `IS_DISK(x)`
(mapped file), writes go through directly.  For mapped files, writes
go straight to the mmap (which is `MAP_SHARED`, so they persist).
For shared in-memory objects (`r > 1`), `cow(x)` copies the object
to a new malloc'd allocation with `r = 1`, making it privately writable.

This works because Kerf owns the entire memory system: the pool
allocator, the reference counting, the mmap region.  Everything is K0
structs all the way down.

**What we have in Lush:**

Lush storages have a `flags` field with `STF_RDONLY` and `STS_MMAP` bits.
The runtime function `(writablep <idx-or-storage>)` exposes this:

```lisp
(writablep col-mmap)  ;; => ()   (read-only, mmap'd)
(writablep col-mem)   ;; => t    (writable, malloc'd)
```

When you try to write to a read-only storage, Lush raises an error:
`"STORAGE is read only"`.  There's no try/catch mechanism to intercept
this.

**Proposed COW scheme for DataTable:**

Since writes go through DataTable methods (`set`, `append-row`), we can
check `writablep` *before* the write and copy on demand:

```lisp
(defmethod DataTable _ensure-writable (col-idx)
  ;; If column col-idx is mmap'd (read-only), copy it to malloc'd storage.
  ;; This is the COW trigger.
  (let ((col (nth col-idx columns))
        (type (nth col-idx col-types-list)) )
    (when (not (writablep col))
      ;; For string columns, codes might be mmap'd
      (if (= type 'string)
        ;; StringColumn: copy codes array, pool is already in memory
        (let ((old-codes (==> col get-codes))
              (new-codes (int-matrix (idx-dim old-codes 0))) )
          (for (i 0 (1- (idx-dim old-codes 0)))
            (new-codes i (old-codes i)) )
          ;; Rebuild StringColumn with writable codes
          (let ((sc (new StringColumn n-rows)))
            (==> sc build-from-codes new-codes (==> col get-pool))
            (setq columns (_dt-list-replace columns col-idx sc)) ) )
        ;; Numeric/stamp: copy the idx1
        (let ((new-col (if (= type 'int)
                         (int-matrix n-rows)
                         (double-matrix n-rows) )))
          (for (i 0 (1- n-rows))
            (new-col i (col i)) )
          (setq columns (_dt-list-replace columns col-idx new-col)) ) ) ) ) )
```

Then `set` becomes:

```lisp
(defmethod DataTable set (row col value)
  (let ((idx (if (stringp col) (col-names-ht col) col)))
    (==> this _ensure-writable idx)
    ...existing write logic... ))
```

And `append-row` calls `_ensure-writable` on every column it touches.

**The "write-back and re-mmap" idea:**

A more ambitious version: after COW-copying a column to memory and
modifying it, the user (or the system) calls something like
`(columnardb-flush dt path)` which:

1. Writes dirty (malloc'd) columns back to their .col files
2. Re-mmaps them as read-only
3. Replaces the malloc'd idx with the mmap'd one

This gets the modified data back to the efficient mmap'd state, frees
the heap copy, and persists the changes.  The column is only "hot"
(in heap memory) while mutations are happening.

The lifecycle would be:
```
  mmap'd (cold, OS-paged) --> COW copy (hot, in heap)
       ^                           |
       |   flush: write + re-mmap  |
       +---------------------------+
```

This is analogous to what a database buffer pool does, just at column
granularity.

**Tradeoffs:**

- COW is transparent to the user (no need to choose load path upfront)
- Per-column granularity means only touched columns go to heap
- `append-row` needs to COW every column, which for a wide table means
  copying all columns on first append — but this only happens once
- The flush step could be automatic (on save) or manual
- Without flush, modified data lives only in memory (lost on exit)
- With flush, there's a brief window where the table is inconsistent
  on disk (partially written columns) — needs an atomic-rename strategy

**What Kerf avoids by owning the allocator:**

Kerf's `MAP_SHARED | PROT_READ | PROT_WRITE` mapping means writes to
mmap'd data go straight to disk via the page cache — no explicit
copy or flush.  Growing a column requires `ftruncate` + `mremap` (or
remap), but existing data is never copied.  This is the ideal, but it
requires either:
- Adding `PROT_WRITE` to Lush's `storage_mmap` (a small C change), or
- A new `storage_mmap_rw` function

With writable mmap, the COW scheme above becomes unnecessary for
in-place modifications.  It would still be needed for growing columns
(append), because mmap'd storages can't be realloc'd.

### Potential Future Improvements

1. **Copy-on-write at DataTable level:** As described above.  Check
   `writablep` before writes, transparently copy mmap'd columns to
   heap.  Eliminates the need for separate `columnardb-load-mem`.

2. **Writable mmap (Stage 6b):** Add `PROT_READ|PROT_WRITE` to
   `storage_mmap` (or a new `storage_mmap_rw`).  This would allow
   in-place modification of mapped columns.  Combined with COW for
   growth (append), this gives near-Kerf write behavior.

3. **~~Use compiled binary search:~~** **DONE.** All binary search and
   sort-check functions in columnardb are now compiled C.  The interpreted
   `_cdb-bsearch-left-double`, `_cdb-bsearch-right-double`,
   `_cdb-is-sorted-double`, and `_cdb-is-sorted-int` have been removed
   and replaced with `idx-d1bsearch-left`, `idx-d1bsearch-right`,
   `idx-d1sortedp`, and `idx-i1sortedp` from `idx-sort.lsh`.

4. **Compression:** Add optional LZ4 or zstd compression for cold
   columns.  Compressed columns would be decompressed on load (not
   mmappable), but would save disk space.  Kerf uses delta-delta
   encoding for timestamps which compresses very well.

5. **Partitioned tables:** Split large tables by time range into
   multiple directories (e.g., one per day/month).  This is Kerf's
   PARTABLE concept.  Queries would scan only relevant partitions.

6. **Column indexes:** Hash index on string columns for O(1) lookup
   by value (vs. O(n) scan).  Kerf uses Robin Hood hashing.

7. **ASOF joins:** Time-series join where the right table matches the
   closest preceding timestamp.  Critical for financial data.  Kerf's
   implementation exploits sorted order with a two-pointer chase.

8. **Concurrent access:** Multiple readers can mmap the same files
   safely (MAP_SHARED is designed for this).  A writer-lock protocol
   (e.g., flock) would be needed for concurrent writes.

### Test Coverage

101 tests across 10 stages:

| Stage | Tests | What |
|-------|-------|------|
| 1 | 15 | Basic save/load round-trip (mem + mmap) |
| 2 | 10 | String column persistence |
| 3 | 6 | Timestamp column persistence |
| 4 | 5 | Sorted column detection |
| 5 | 14 | Binary search range queries |
| 6 | 12 | Append to persisted table |
| 7 | 11 | Mixed types, 1000-row table |
| 8 | 9 | Metadata integrity |
| 9 | 11 | Edge cases (empty, single row, negative, wide) |
| 10 | 8 | Timestamp range queries (primary use case) |

All 101 tests pass.  No regressions in existing test suites:
- 623 datatable package tests: all pass
- 116 core interpreter tests: all pass
- 52 csvread tests: all pass (new test harness)

---

## Part 10: IDX Sort Integration and Status (2026-02-23)

### What was missing from the original plan

The plan originally claimed "No native argsort / grade_up in Lush" (Part 5,
Significant Challenge #1).  This was incorrect.  The `idx-sort.lsh` library
in `lsh/libidx/` provides compiled C (via dhc-make) sort and binary search
functions that were overlooked during the initial design.

### Available IDX sort primitives (all compiled C, heap sort)

| Function | Type | Purpose |
|----------|------|---------|
| `idx-d1sortup/down` | double | In-place sort |
| `idx-f1sortup/down` | float | In-place sort |
| `idx-i1sortup/down` | int | In-place sort |
| `idx-d1i1sortup/down` | double+int | **Grade/argsort for timestamps and reals** |
| `idx-f1i1sortup/down` | float+int | Grade/argsort for float columns |
| `idx-i1i1sortup/down` | int+int | Grade/argsort for int columns |
| `idx-d1bsearch` | double | Binary search (element <= target) |
| `idx-f1bsearch` | float | Binary search |
| `idx-i1bsearch` | int | Binary search |
| `idx-d1bsearch-left` | double | **Lower bound (leftmost i where a[i] >= target)** |
| `idx-d1bsearch-right` | double | **Upper bound (rightmost i where a[i] <= target)** |
| `idx-d1sortedp` | double | **Check if sorted ascending (returns 1/0)** |
| `idx-i1sortedp` | int | **Check if sorted ascending (returns 1/0)** |

### What was added

**`idx-d1i1sortup` and `idx-d1i1sortdown`** (added 2026-02-23 to
`lsh/libidx/idx-sort.lsh`):  Paired sort for double data + int index
vectors.  These are the grade/argsort functions for timestamp and real
columns.  Since timestamps are stored as doubles (seconds since epoch),
`idx-d1i1sortup` IS the timestamp grade sort.  No separate timestamp
sort function is needed.

Usage pattern for grade (argsort):
```lisp
;; Given a double column 'col' of length n:
(let ((data-copy (double-matrix n))
      (grade (int-matrix n)))
  (for (i 0 (1- n)) (data-copy i (col i)))  ;; copy (sort is destructive)
  (for (i 0 (1- n)) (grade i i))             ;; fill with 0..n-1
  (idx-d1i1sortup data-copy grade)            ;; compiled C heap sort
  ;; grade now contains the permutation vector:
  ;; grade[0] = index of smallest element in original
  ;; grade[n-1] = index of largest element in original
  )
```

The paired sort functions previously existed only for float+int and int+int.
The double+int variants were the gap -- timestamp and real columns are double
but had no compiled grade function.

### Current implementation status

| Component | Status | Notes |
|-----------|--------|-------|
| DataTable class (Stage 1) | **Done** | packages/datatable/ |
| Amortized growth (Stage 2) | **Done** | Capacity doubling |
| Timestamp utilities (Stage 3a) | **Done** | packages/timedate/ (double seconds) |
| Timestamp C primitives (Stage 3b) | Not done | Low priority, Lush-level works |
| StringColumn dict encoding (Stage 5) | **Done** | Per-column pool + int codes |
| ColumnarDB persistence (Stage 6a) | **Done** | 101 tests, native-endian LCDB format |
| Sort detection on save | **Done** | idx-d1sortedp/idx-i1sortedp (compiled C) |
| Binary search range query | **Done** | idx-d1bsearch-left/right (compiled C) |
| Grade/argsort functions | **Done** | idx-d1i1sortup/down (compiled C) |
| DataTable sort-by method | **Not done** | Needs to wire up idx-d1i1sortup |
| COW for mmap'd tables | **Not done** | Designed in Part 9, not implemented |
| Writable mmap (Stage 6b) | **Not done** | Requires C changes to storage_mmap |
| ASOF joins | **Not done** | Future |
| Partitioned tables | **Not done** | Future |

### Completed: All interpreted searches removed (2026-02-23)

All interpreted Lush search and sort-check functions have been removed from
`columnardb.lsh` and replaced with compiled C functions from `idx-sort.lsh`:

| Removed (interpreted) | Replaced with (compiled C) |
|----------------------|---------------------------|
| `_cdb-is-sorted-double` | `idx-d1sortedp` |
| `_cdb-is-sorted-int` | `idx-i1sortedp` |
| `_cdb-bsearch-left-double` | `idx-d1bsearch-left` |
| `_cdb-bsearch-right-double` | `idx-d1bsearch-right` |

The `columnardb.lsh` file now `libload`s `libidx/idx-sort` and uses:
- `idx-d1sortedp` / `idx-i1sortedp` in `columnardb-save` and `columnardb-append`
- `idx-d1bsearch-left` / `idx-d1bsearch-right` in `columnardb-range`

All 101 columnardb tests, 623 datatable tests, and 116 core tests pass.

### Near-term work items

1. **Wire up DataTable `sort-by` method** using `idx-d1i1sortup`.
   The grade function now exists as compiled C; we just need the DataTable
   method to call it and store the resulting permutation vector.

2. **Implement COW** for mmap'd tables (designed in Part 9).
   This would eliminate the awkward `columnardb-load` vs `columnardb-load-mem`
   split.

3. **Add `select-rows` using grade vectors.**
   Apply a grade permutation to reorder all columns simultaneously, returning
   a new DataTable in sorted order.  This enables ORDER BY semantics.
