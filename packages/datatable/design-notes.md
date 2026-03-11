# DataTable Package: Design Notes

Columnar table type for Lush with C-accelerated CSV I/O. Inspired by Kerf1's
table type and pandas' CSV parser, adapted to Lush's idx/storage system.

**Package location:** `packages/datatable/`

## Architecture

### Files

```
-- In-memory core --
datatable.lsh              DataTable class + loads in-memory modules
datatable-compiled.lsh     All compiled C hot-paths (sort, gather, CSR, filter, bsearch, LIKE, IN)
datatable-null.lsh         Null sentinel handling
datatable-join.lsh         Left join, right join, as-of join
datatable-groupby.lsh      Grouped aggregation (count, sum, mean, min, max)
datatable-multiindex.lsh   Multi-condition filter + range queries
datatable-query.lsh        SQL-like db-select/db-count query macros
stringcol.lsh              StringColumn (dictionary-encoded strings)
intcol.lsh                 IntColumn (dictionary-encoded integers)

-- CSV I/O --
datatable-config.lsh       Compiles C code via lushmake
datatable-csv-c.h          C header: function prototypes, constants
datatable-csv-c.c          C implementation: CSV scanner + readers
datatable-csv.lsh          Lush bridge: csv-read public API
datatable-csv-write.lsh    Lush bridge: csv-write public API

-- Persistence layer (loaded via datatable/columnardb) --
columnardb.lsh             Entry point: save, load, mmap, append, range, locking
columnardb-io.lsh          LCDB binary I/O (read/write/append/mmap)
columnardb-compress.lsh    Column compression (delta, RLE, LZ4)
columnardb-database.lsh    Database catalog class
columnardb-mutate.lsh      Delete/update on persisted tables
columnardb-partition.lsh   Partitioned table support

C/                         Auto-generated DH stubs (from dhc-make)
```

### Column Storage

Each column in a DataTable is one of:

| Type     | Symbol    | Storage                   | Element Access           |
|----------|-----------|---------------------------|--------------------------|
| Real     | `'real`   | `idx1` of `(-double-)`    | `(col row)`              |
| Float    | `'float`  | `idx1` of `(-float-)`     | `(col row)`              |
| Integer  | `'int`    | `idx1` of `(-int-)`       | `(col row)`              |
| Stamp    | `'stamp`  | `idx1` of `(-long-)`      | `(col row)`              |
| String   | `'string` | `StringColumn` object     | `(==> col get row)`      |
| IntCol   | `'intcol` | `IntColumn` object        | pool lookup              |

Columns are stored in a flat Lush list (`columns` slot). Column name lookup
goes through an HTable (`col-names-ht`) for O(1) name-to-index mapping.

**Null sentinels:** NaN (real/float), `INT_MIN` (int/intcol), `INT64_MIN`
(stamp), `""` (string). All map to empty field in CSV output.

### StringColumn (Dictionary Encoding)

Inspired by Kerf1's HASH/ENUM type. Each StringColumn maintains:

- `str-to-code` -- HTable mapping string to integer code
- `code-to-str` -- List mapping code to string (reverse lookup)
- `codes` -- int-matrix of per-row codes

**Get:** row -> `codes[row]` -> `nth code code-to-str` (two lookups).
**Equality test:** O(1) integer comparison on codes.
**Memory:** For N rows with K unique strings: `4N + ~K*avg_len` bytes
vs `N*avg_len` bytes for naive storage. Wins big when K << N.

---

## API Summary

### Loading

```lisp
(libload "datatable/datatable-csv")   ;; loads everything (reader + writer)
;; OR for just the DataTable class (no CSV):
(libload "datatable/datatable")
```

### csv-read

```lisp
(setq t (csv-read "/path/to/data.csv"))
```

Reads CSV or TSV file into a DataTable. Auto-detects delimiter (tabs vs
commas in header) and column types (samples first 200 data rows). Supports
`.gz` files via `zcat` pipe.

Type inference ladder: int -> double -> string. Empty/NA/NaN/null/N/A values
are ignored during type inference. Both int and double are stored as `'real`
(double-matrix) to match Lush's native numeric type.

### csv-write

```lisp
(csv-write dt "/tmp/output.csv")
(csv-write dt "/tmp/output.tsv" '(("delim" . "\t")))
(csv-write dt "/tmp/output.csv" '(("stamp-fmt" . "%Y/%m/%d")))
```

Options (via alist):
- `"delim"` -- delimiter string (default `","`)
- `"header"` -- write header row (default `t`)
- `"stamp-fmt"` -- timestamp format (default `"iso"`, or strftime spec)
- `"null"` -- null representation (default `""`, could be `"NA"`)
- `"precision"` -- decimal digits for real/float (default 15/7)

### DataTable Methods

```lisp
;; Dimensions
(==> t num-rows)                       ;; row count
(==> t num-cols)                       ;; column count
(==> t col-names)                      ;; list of column name strings
(==> t col-types)                      ;; list of type symbols

;; Column metadata
(==> t col-name idx)                   ;; name by index
(==> t col-type idx-or-name)           ;; type by index or name
(==> t col-index name)                 ;; index by name (O(1) via HTable)

;; Element access
(==> t get row col)                    ;; col is index or name
(==> t set row col value)              ;; set numeric value

;; Column access (returns raw idx1 or StringColumn)
(==> t get-column idx-or-name)

;; Display
(==> t head n)                         ;; pretty-print first n rows
(==> t print-table n)                  ;; pretty-print first n rows

;; Subset
(==> t select-columns name-list)       ;; new DataTable with selected cols
```

### Building a DataTable Manually

```lisp
(setq t (new DataTable))
(==> t add-column "price" 'real)
(==> t add-column "symbol" 'string)
(==> t add-column "quantity" 'int)
(==> t append-row (list 150.25 "AAPL" 100))
```

Columns must be added before any rows. Numeric columns use capacity-doubling
growth (starting at 64).

### Bulk Construction

```lisp
(datatable-from-columns
  '("x" "y" "label")               ;; column names
  '(real real string)               ;; column types
  (list x-data y-data str-col)     ;; column data objects
  1000)                             ;; number of rows
```

Builds a DataTable in O(n) without repeated list replacement. Used internally
by `csv-read`. Prefer this over `add-column` + `set-column-data` for wide
tables (avoids O(n^2) list ops).

### StringColumn Methods

```lisp
(setq sc (new StringColumn 100))      ;; initial capacity
(==> sc append "AAPL")
(==> sc get 0)                        ;; -> "AAPL"
(==> sc get-code 0)                   ;; -> 0 (integer code)
(==> sc num-rows)                     ;; -> count
(==> sc num-unique)                   ;; -> unique string count
(==> sc rows-equal i j)               ;; -> t/() (O(1) int compare)
(==> sc where-eq "AAPL")              ;; -> (matching row indices)
(==> sc get-codes)                    ;; -> int-matrix of codes
(==> sc get-pool)                     ;; -> list of unique strings
```

---

## Implementation Details

### CSV Reader: Data Flow

```
CSV file
  |
  +-- Phase 1: dt_csv_scan() [C]
  |     Reads entire file once.
  |     Outputs: nrows, ncols, delimiter, col_types[], col_names[]
  |     Type inference: samples first 200 rows per column.
  |     Type promotion ladder: int -> double -> string
  |
  +-- Phase 2a: dt_csv_read_numeric() [C]
  |     Re-reads file, extracts selected columns into one
  |     pre-allocated double matrix (nrows x n_numeric_cols).
  |     Uses strtod per field. Non-numeric -> NaN.
  |
  +-- Phase 2b: dt_csv_read_string_col() [C, once per string col]
  |     Re-reads file, extracts one column with C-level dictionary
  |     encoding. Returns int codes[] + unique string buffer.
  |
  +-- Phase 3: datatable-from-columns [Lush]
        Assembles columns into DataTable in O(n).
```

**Why multiple file passes:** Simpler code, works with pipes/gzip, avoids
allocating a multi-GB buffer for very large files. Tradeoff is slower for
SSD/NVMe where re-reading is cheap. For the NKI dataset (272 x 6363):
1 scan + 1 numeric read + 4 string reads = 6 total file reads, 0.72s total.

**Why strtod per field (not running pointer):** `mapper-tsv-load` uses strtod
on a running pointer, which gets stuck on non-numeric values and corrupts all
subsequent columns in the row. The datatable reader splits fields by delimiter
first, then parses each independently.

### CSV Reader: C Layer

```c
DT_MAX_COLS          65536    // Max columns supported
DT_TYPE_SAMPLE_ROWS  200     // Rows sampled for type inference
DT_HASH_SIZE         8192    // Hash table size for string interning
```

- `dt_csv_scan`: First line = header. Counts tabs vs commas for delimiter.
  Column names stored as concatenated null-terminated strings in `name_buf`
  (avoids allocating Lush string objects in C). Type detection via
  `_dt_detect_field_type` with int(0) -> double(1) -> string(2) promotion.

- `dt_csv_read_numeric`: Builds reverse map `out_map[orig_col] = output_col`
  for O(1) dispatch. Writes into pre-allocated `data[row * n_cols + j]`.
  Short lines -> NaN for missing columns.

- `dt_csv_read_string_col`: C-level open-addressing hash table (djb2 hash,
  linear probing) for O(1) string interning. Unique strings copied into
  `unique_buf`. Lush side builds StringColumn from arrays via
  `build-from-codes`.

### CSV Writer: Strategy

**Streaming, not buffering.** Writes directly to `FILE*` via `fwrite`. Never
builds the entire CSV in memory. Per-row: format each field into a small stack
buffer (~256 bytes), write with quoting/escaping, write delimiter or newline.
No heap allocation in the hot loop.

**String column optimization:** Pre-escapes the string pool before the row
loop. For each unique string, determines whether it needs CSV quoting
(contains `,`, `"`, `\n`, `\r`) and builds escaped versions. In the row loop,
just looks up pre-escaped string by code. This turns N*K string-scanning
operations into P*K (P unique strings, P << N typically).

**Timestamp fast path:** For ISO 8601 output, uses pure integer arithmetic
(Howard Hinnant civil_from_days algorithm) instead of `strftime`. Avoids
locale lookup, timezone conversion, and format string parsing on every call.
Expected 20-50x faster than strftime. Falls back to strftime for non-ISO
format strings.

### Compilation Pipeline

Two-stage compilation:

1. **lushmake** (`datatable-config.lsh`): Compiles `datatable-csv-c.c` into
   `.o` and loads it. Provides raw C functions.

2. **dhc-make-with-libs** (`datatable-csv.lsh`): Compiles Lush bridge
   functions (`_csv-scan-inner`, `_csv-read-numeric-inner`,
   `_csv-read-string-inner`) containing inline C (`#{{ ... }#}`) that call
   the functions from step 1.

`C/datatable_csv.c` is auto-generated by the DH compiler from the inline C.

### Relationship to Other CSV Packages

- **csvread** (`packages/csvread/`): Pure-Lush CSV parser. Returns
  `(list double-matrix col-names)`. All data as doubles. Simple but slow,
  no type detection. Still in use by existing code.

- **mapper/csvread** (`packages/mapper/`): C-accelerated TSV/CSV reader.
  Returns `(list double-matrix col-names)`. Fast but all data as doubles,
  gets stuck on non-numeric fields. Used by mapper-viz.

- **datatable/datatable-csv**: Replaces both for new code. Returns a
  DataTable with typed columns. Correctly handles mixed-type files.

---

## Known Issues / Limitations

### Performance

- `csv-read` (0.72s on NKI dataset) is slower than `mapper-tsv-load` (0.28s)
  due to multiple file passes, but handles mixed-type files correctly.
- Phase breakdown (NKI 272x6363): scan ~0.1s, numeric ~0.3s, strings
  ~0.05s x 4 = ~0.2s, assembly ~0.1s.

### Potential Optimizations (Not Implemented)

- **Single-pass reading:** Buffer all data in Phase 1, parse from memory in
  Phase 2. Eliminates re-reads but requires large memory buffer.
- **mmap instead of getline:** Map file into memory, walk bytes directly
  (Kerf1 approach). Eliminates I/O overhead.
- **Column copy elimination:** Phase 2a writes into a big matrix, then Phase 3
  copies each column to individual idx1s. Could write directly into individual
  arrays with a changed C interface.
- **Parallel string column reads:** Each string column re-reads the file
  independently. Could read all string columns in one pass.

### Potential New Column Types (Not Implemented)

- **`'ubyte`** (unsigned 8-bit): 1 byte/row, covers booleans and small
  categoricals. Main gap: no sorting support in Lush's idx-sort (trivially
  solved with counting sort over 256 buckets). Null sentinel: 255.
- **`'short`** (signed 16-bit): 2 bytes/row, 2x savings over int for
  medium-range values. Null sentinel: -32768.
- Boolean columns should use `'ubyte` with 0/1 values rather than bitpacking
  (1 byte/row is cheap enough; avoids custom bit-manipulation infrastructure).

### Design Constraints

- `datatable-from-columns` exists because naive per-column `add-column` +
  `set-column-data` is O(n^2) for wide tables (Lush lists don't support O(1)
  indexed replacement).
- File is re-opened for each phase to avoid seeking issues with pipe sources
  (gzip).
- One hash table per string column in C keeps code simple and hash tables
  small (8192 entries each).
