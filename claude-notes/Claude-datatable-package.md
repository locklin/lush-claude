# DataTable Package: Implementation Notes

## Overview

The `datatable` package is a columnar table type for Lush with a
C-accelerated CSV reader. Inspired by Kerf1's table type and pandas'
CSV parser, adapted to Lush's idx/storage system.

**Package location:** `packages/datatable/`

**Status:** Core implemented — DataTable, StringColumn, csv-read.
No persistence layer yet (mmap/striped files are a future stage per
the columnar DB plan).

---

## Files

```
packages/datatable/
  datatable-config.lsh    Compiles C code via lushmake
  datatable-csv-c.h       C header: function prototypes, constants
  datatable-csv-c.c       C implementation: CSV scanner + readers
  datatable-csv.lsh       Lush bridge: csv-read public API
  datatable.lsh           DataTable class definition
  stringcol.lsh           StringColumn (dictionary-encoded strings)
  C/datatable_csv.c       Auto-generated DH stubs (from dhc-make)
```

---

## Architecture

### Data Flow (csv-read)

```
CSV file
  │
  ├── Phase 1: dt_csv_scan() [C]
  │     Reads entire file once.
  │     Outputs: nrows, ncols, delimiter, col_types[], col_names[]
  │     Type inference: samples first 200 rows per column.
  │     Type promotion ladder: int → double → string
  │
  ├── Phase 2a: dt_csv_read_numeric() [C]
  │     Re-reads file, extracts selected columns into one
  │     pre-allocated double matrix (nrows × n_numeric_cols).
  │     Uses strtod per field. Non-numeric → NaN.
  │
  ├── Phase 2b: dt_csv_read_string_col() [C, once per string col]
  │     Re-reads file, extracts one column with C-level dictionary
  │     encoding. Returns int codes[] + unique string buffer.
  │
  └── Phase 3: datatable-from-columns [Lush]
        Assembles columns into DataTable in O(n) — one pass over
        the column list. No O(n²) list replacement.
```

### Column Storage

Each column in a DataTable is one of:

| Type     | Symbol    | Storage             | Element Access     |
|----------|-----------|---------------------|--------------------|
| Real     | `'real`   | `idx1` of `(-double-)` | `(col row)`        |
| Integer  | `'int`    | `idx1` of `(-int-)`    | `(col row)`        |
| String   | `'string` | `StringColumn` object  | `(==> col get row)` |

Columns are stored in a flat Lush list (`columns` slot). Column name
lookup goes through an HTable (`col-names-ht`) for O(1) name→index.

### StringColumn (Dictionary Encoding)

Inspired by Kerf1's HASH/ENUM type. Each StringColumn maintains:

- `str-to-code` — HTable mapping string → integer code
- `code-to-str` — List mapping code → string (reverse lookup)
- `codes` — int-matrix of per-row codes

**Get:** row → `codes[row]` → `nth code code-to-str` (two lookups)
**Equality test:** O(1) integer comparison on codes
**Memory:** For N rows with K unique strings: `4N + ~K*avg_len` bytes
vs `N*avg_len` bytes for naive storage. Wins big when K << N.

---

## Public API

### Loading

```lisp
(libload "datatable/datatable-csv")   ;; loads everything
;; OR for just the DataTable class (no CSV reader):
(libload "datatable/datatable")
```

### csv-read

```lisp
(setq t (csv-read "/path/to/data.csv"))
```

Reads CSV or TSV file into a DataTable. Auto-detects:
- **Delimiter:** counts tabs vs commas in the header line
- **Column types:** samples first 200 data rows per column
  - All values parse as integer → `'real` (stored as double)
  - All values parse as double → `'real`
  - Anything else → `'string` (dictionary-encoded)
  - Empty/NA/NaN/null/N/A values are ignored during type inference

Supports `.gz` files (via `zcat` pipe).

Prints progress: `csv-read: /path/to/data.csv -> 272 rows x 6363 cols`

### DataTable Methods

```lisp
;; Dimensions
(==> t num-rows)                    ;; → 272
(==> t num-cols)                    ;; → 6363
(==> t col-names)                   ;; → ("Patient" "Gene1" "Gene2" ...)
(==> t col-types)                   ;; → (string real real ...)

;; Column metadata
(==> t col-name 0)                  ;; → "Patient"
(==> t col-type 0)                  ;; → string
(==> t col-type "Gene1")            ;; → real
(==> t col-index "Gene1")           ;; → 1

;; Element access
(==> t get 0 0)                     ;; → "s122" (string col)
(==> t get 0 1)                     ;; → -0.954246 (numeric col)
(==> t get 0 "Gene1")              ;; → -0.954246 (by name)
(==> t set 0 1 99.0)               ;; set numeric value

;; Column access (returns raw idx1 or StringColumn)
(==> t get-column 1)               ;; → idx1 of 272 doubles
(==> t get-column "Patient")       ;; → StringColumn object

;; Display
(==> t head 10)                     ;; pretty-print first 10 rows
(==> t print-table 20)             ;; pretty-print first 20 rows

;; Subset
(==> t select-columns '("Gene1" "Gene2" "Gene3"))  ;; → new DataTable
```

### Building a DataTable Manually

```lisp
(setq t (new DataTable))
(==> t add-column "price" 'real)
(==> t add-column "symbol" 'string)
(==> t add-column "quantity" 'int)
(==> t append-row (list 150.25 "AAPL" 100))
(==> t append-row (list 2800.00 "GOOG" 50))
(==> t head)
```

Columns must be added before any rows are appended. Numeric columns
use capacity-doubling growth (starting at 64, doubles when full).

### Bulk Construction (for programmatic use)

```lisp
(datatable-from-columns
  '("x" "y" "label")              ;; column names
  '(real real string)              ;; column types
  (list x-data y-data str-col)    ;; column data objects
  1000)                            ;; number of rows
```

This builds a DataTable in O(n) without repeated list replacement.
Used internally by `csv-read`. Prefer this over `add-column` +
`set-column-data` for wide tables (avoids O(n²) list ops).

### StringColumn Methods

```lisp
(setq sc (new StringColumn 100))   ;; initial capacity
(==> sc append "AAPL")
(==> sc append "GOOG")
(==> sc append "AAPL")             ;; reuses existing code
(==> sc get 0)                     ;; → "AAPL"
(==> sc get-code 0)                ;; → 0 (integer code)
(==> sc num-rows)                  ;; → 3
(==> sc num-unique)                ;; → 2
(==> sc rows-equal 0 2)            ;; → t (O(1) int compare)
(==> sc where-eq "AAPL")           ;; → (0 2) (matching row indices)
(==> sc get-codes)                 ;; → int-matrix of codes
(==> sc get-pool)                  ;; → ("AAPL" "GOOG")
```

---

## C Layer Details

### Constants

```c
DT_MAX_COLS          65536    Max columns supported
DT_TYPE_SAMPLE_ROWS  200      Rows sampled for type inference
DT_HASH_SIZE         8192     Hash table size for string interning
```

### dt_csv_scan

Reads the entire file. First line = header (column names). Counts
tabs vs commas to detect delimiter. Parses header into field names
stored as concatenated null-terminated strings in `name_buf`. For
data rows, splits fields and calls `_dt_detect_field_type` on each
field in the first 200 rows. Type promotion: `int(0) → double(1) →
string(2)`. Counts all data rows. Returns dimensions, delimiter,
types, and names.

### dt_csv_read_numeric

Re-reads the file. Takes an array of original column indices to
extract. Builds a reverse map (`out_map[orig_col] = output_col`)
for O(1) dispatch during parsing. For each data row, walks fields
left-to-right, calls `strtod` on selected fields, writes into
pre-allocated `data[row * n_cols + j]`. Non-numeric fields → NaN.
Short lines → NaN for missing columns.

Key difference from mapper-tsv-load: this function splits fields
by delimiter first, then parses each independently. mapper-tsv-load
uses strtod on a running pointer, which gets stuck on non-numeric
values and corrupts all subsequent columns in the row.

### dt_csv_read_string_col

Re-reads the file to extract a single string column. Uses a
C-level open-addressing hash table (djb2 hash, linear probing)
for O(1) string interning. Each unique string is copied into
`unique_buf` with its offset recorded in `unique_offsets`. Per-row
codes are written to `codes[row]`. The Lush side then builds a
StringColumn from these arrays via `build-from-codes`.

### File Handling

Both `.csv` and `.csv.gz` are supported. Gzipped files are opened
via `popen("zcat '...'", "r")`. The file is opened and closed for
each phase (scan, read_numeric, read_string_col × N) — this is
intentional to avoid seeking issues with pipes.

---

## Performance

Tested on NKI dataset (272 patients × 6363 columns, 6359 real +
4 string):

- **csv-read load time:** 0.72 seconds
- **mapper-tsv-load time:** 0.28 seconds (but incorrect for mixed-type files)
- **Data integrity:** 272,000 values verified (1000 columns × 272 rows)
  with zero difference vs mapper-tsv-load

csv-read is slower than mapper-tsv-load because it makes multiple
file passes (scan + read_numeric + read_string × N_string_cols).
For the NKI file: 1 scan + 1 numeric read + 4 string reads = 6
total file reads. The tradeoff is correct handling of mixed-type
files and per-column type inference.

### Where Time Goes

- Phase 1 (scan): ~0.1s — one full file read, light per-field work
- Phase 2a (numeric): ~0.3s — one full file read, strtod per field
- Phase 2b (strings): ~0.05s × 4 = ~0.2s — partial reads
- Phase 3 (assembly): ~0.1s — column copy from matrix to individual idx1s

### Potential Optimizations (Not Yet Done)

- **Single-pass reading:** Buffer all data in Phase 1, parse from memory
  in Phase 2. Eliminates re-reads.
- **mmap instead of getline:** Map file into memory, walk bytes directly
  (kerf1 approach). Eliminates I/O overhead.
- **Column copy elimination:** Phase 2a writes into a big matrix, then
  Phase 3 copies each column out to individual idx1s. Could write
  directly into individual arrays if we changed the C interface.
- **Parallel string column reads:** Each string column re-reads the
  file independently. Could read all string columns in one pass.

---

## Compilation

The C code is compiled via two mechanisms:

1. **lushmake** (`datatable-config.lsh`): Compiles `datatable-csv-c.c`
   into a `.o` file and loads it. This provides the raw C functions.

2. **dhc-make-with-libs** (`datatable-csv.lsh`): Compiles the three
   Lush bridge functions (`_csv-scan-inner`, `_csv-read-numeric-inner`,
   `_csv-read-string-inner`) that contain inline C (`#{{ ... }#}`)
   calling the functions from step 1.

The `C/datatable_csv.c` file is auto-generated by the DH compiler
from the inline C in `datatable-csv.lsh`. It contains the stubs
that marshal Lush types (srg, idx) to/from C arguments.

---

## Relationship to Other Packages

- **csvread** (`packages/csvread/csvread.lsh`): Pure-Lush CSV parser.
  Returns `(list double-matrix col-names)`. All data as doubles.
  Simple but slow, no type detection. Still in use by existing code.

- **mapper/csvread** (`packages/mapper/csvread.lsh`): C-accelerated
  TSV/CSV reader. Returns `(list double-matrix col-names)`. Fast but
  all data as doubles, gets stuck on non-numeric fields. Used by
  mapper-viz for dataset loading.

- **datatable/datatable-csv**: Replaces both for new code. Returns a
  DataTable with typed columns (real + string). Correctly handles
  mixed-type files. Will be the standard CSV reader going forward.

---

## Design Decisions

1. **Int and double both stored as `'real` (double-matrix).** The C
   scanner detects int vs double, but the Lush side promotes both to
   double. This matches Lush's native numeric type and avoids needing
   separate int column arrays for numeric data. The type distinction
   is preserved in `col_types` for future use (e.g., display formatting).

2. **Multiple file passes rather than buffering.** Simpler code, works
   with pipes/gzip, avoids allocating a multi-GB buffer for very large
   files. Tradeoff: slower for SSD/NVMe where re-reading is cheap.

3. **One hash table per string column in C.** Each string column gets
   its own interning hash table (8192 entries, open addressing). This
   keeps the C code simple and the hash tables small. The Lush-side
   StringColumn then takes ownership of the interned data.

4. **Column names stored as concatenated null-terminated strings.**
   Avoids allocating Lush string objects in C. The Lush side extracts
   them with `_csv-extract-string` after the C call returns.

5. **`datatable-from-columns` for bulk construction.** The naive approach
   of `add-column` + `set-column-data` per column is O(n²) for wide
   tables because Lush lists don't support O(1) indexed replacement.
   The bulk constructor sets all slots in one pass.
