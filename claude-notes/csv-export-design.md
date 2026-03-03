# CSV Export Design for DataTable / ColumnarDB

## Problem

The columnar database has a fast C-accelerated CSV reader (`csv-read`) but no
export path.  Data goes in but doesn't come back out as CSV.  A symmetric
`csv-write` is needed.

## Column Types to Format

| Type | Internal Storage | Format Strategy | Null Output |
|------|-----------------|----------------|-------------|
| `'real` | `idx1 (-double-)` | `sprintf "%.*g"` | empty field |
| `'float` | `idx1 (-float-)` | `sprintf "%.*g"` | empty field |
| `'int` | `idx1 (-int-)` | `sprintf "%d"` | empty field |
| `'stamp` | `idx1 (-long-)` | fast ISO or strftime | empty field |
| `'string` | StringColumn (codes + pool) | pool lookup + CSV escape | empty field |
| `'intcol` | IntColumn (codes + pool) | pool lookup + `sprintf "%d"` | empty field |

Null sentinels: NaN (real/float), `INT_MIN` (int/intcol), `INT64_MIN` (stamp),
`""` (string).  All map to empty field in CSV output (standard convention,
matches R's `write.csv`).

## Performance Analysis

### The Timestamp Bottleneck

For a table with 10M rows and a timestamp column, the naive approach calls
`timestamp-to-string` per row, which uses `strftime`.  `strftime` is
surprisingly heavy:

- Locale lookup on every call
- Timezone conversion (even for UTC)
- Format string parsing on every call
- Typical throughput: ~2-5M calls/sec

For ISO 8601 output (`2024-06-15 14:30:00`), we can avoid `strftime` entirely
with integer arithmetic:

```c
// Fast path: microseconds → ISO 8601 string
// Uses the same epoch arithmetic as timedate.lsh but inlined
static int fast_iso_format(int64_t micros, char *buf, int buflen) {
    int64_t secs = micros / 1000000;
    // Days since epoch via civil_from_days (Howard Hinnant algorithm)
    int64_t days = secs / 86400;
    int rem = (int)(secs % 86400);
    if (rem < 0) { days--; rem += 86400; }
    int h = rem / 3600;
    int m = (rem % 3600) / 60;
    int s = rem % 60;
    // Civil date from day count (era-based algorithm)
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = (int)(yoe + era * 400);
    int doy = doe - (365*yoe + yoe/4 - yoe/100);
    int mp = (5*doy + 2) / 153;
    int d = doy - (153*mp + 2) / 5 + 1;
    int mo = mp + (mp < 10 ? 3 : -9);
    if (mo <= 2) y++;
    return snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d",
                    y, mo, d, h, m, s);
}
```

This is pure integer arithmetic — no locale, no timezone, no format parsing.
Expected throughput: 50-100M calls/sec (20-50x faster than strftime).

For sub-second precision, append `.%06d` for microseconds if nonzero.

A configurable format string can fall back to the slower `strftime` path when
the user requests a non-ISO format.

### Memory Strategy

**Streaming, not buffering.**  Write directly to a `FILE*` via `fwrite`.
Never build the entire CSV in memory.  For a 10M-row, 20-column table the CSV
could be 2-4 GB — must not malloc that.

Per-row approach:
1. Format each field into a small stack buffer (~256 bytes per field)
2. Write the field (with quoting/escaping if needed) to the output FILE*
3. Write delimiter or newline
4. Repeat

Stack buffer per row: ~5 KB for 20 columns.  No heap allocation in the hot
loop.

### String Column Optimization

StringColumn uses dictionary encoding: each row stores an integer code, and
the pool maps codes to strings.  For CSV export:

1. **Pre-escape the pool** before the row loop.  For each unique string in
   the pool, determine whether it needs CSV quoting (contains `,`, `"`, `\n`,
   or `\r`) and build escaped versions.
2. In the row loop, just look up the pre-escaped string by code.

This turns N*K string-scanning operations (N rows, K avg string length) into
P*K (P unique strings, P << N typically).  For a stock symbol column with 5000
unique symbols and 10M rows, that's 2000x less escaping work.

Pre-escaped pool is a flat C array of `char*` pointers, allocated once and
freed after export.

### Batch Flushing

Buffer output in a 64 KB write buffer (like stdio) rather than calling
`fwrite` per field.  This reduces syscall overhead.  `setvbuf` on the FILE*
handles this automatically if we use `fprintf`/`fputs`, but explicit buffering
with a manually managed buffer + `fwrite` at 64 KB boundaries gives better
control.

## API Design

### Lush Interface

```lush
#? (csv-write <dt> <path> [<options>])
;; Write DataTable <dt> to CSV file at <path>.
;; <options> is an optional alist:
;; {<ul>
;; {<li> {<b> "delim"} -- delimiter string (default ",")}
;; {<li> {<b> "header"} -- write header row (default t)}
;; {<li> {<b> "stamp-fmt"} -- timestamp format (default "iso", or strftime spec)}
;; {<li> {<b> "null"} -- null representation (default "", could be "NA" or "NULL")}
;; {<li> {<b> "precision"} -- decimal digits for real/float (default 15 for real, 7 for float)}
;; }
(de csv-write (dt path . rest) ...)
```

Example usage:
```lush
(csv-write dt "/tmp/claude/output.csv")
(csv-write dt "/tmp/claude/output.tsv" '(("delim" . "\t")))
(csv-write dt "/tmp/claude/output.csv" '(("stamp-fmt" . "%Y/%m/%d")))
```

### C Bridge Function

Single C function that does all the work, called from the Lush wrapper:

```c
int lush_csv_write(
    FILE *fp,                     // output file
    int nrows, int ncols,
    int *col_types,               // type code per column (0=real,1=int,2=string,3=stamp,4=float,5=intcol)
    void **col_data,              // array of column data pointers
    char **col_names,             // header names
    // String pool info (per string/intcol column):
    char ***str_pools,            // pool[col] → array of char*
    int *pool_sizes,              // pool sizes
    // Options:
    char delim,
    const char *null_str,
    const char *stamp_fmt,        // "iso" for fast path, else strftime format
    int real_precision,
    int float_precision
);
```

The column data pointers are:
- `'real` → `double*` (from idx1 storage)
- `'int` → `int*`
- `'float` → `float*`
- `'stamp` → `int64_t*`
- `'string` → `int*` (codes array)
- `'intcol` → `int*` (codes array), with `int*` pool

### Integration with ColumnarDB

Also provide a method on the database class:

```lush
(==> db export-csv "tablename" "/path/to/output.csv" options)
```

This loads the table (via mmap if on disk) and calls `csv-write`.

## Implementation Plan

### File: `packages/datatable/datatable-csv-write.lsh`

~150 lines of Lush + ~200 lines of inline C.

**Lush layer** (~80 lines):
- `csv-write` function: validates args, extracts column arrays/pools,
  opens file, calls C bridge, closes file
- Option parsing (delim, header, stamp-fmt, null, precision)
- Marshal StringColumn/IntColumn pools into flat C arrays

**C layer** (~200 lines, via dhc-make):
- `fast_iso_format` — integer-arithmetic ISO 8601 formatter
- `csv_escape_field` — quote/escape a string field
- `_csv-write-inner` — the main row loop, compiled via dhc-make
  - For each row: for each column: format field, write delimiter
  - Type switch per column (but column type is known, so the switch
    is on a per-column constant, not per-row — branch predictor friendly)

**Key detail:** The inner loop iterates rows in the outer loop, columns in
the inner loop.  This is row-major output order (CSV is row-oriented) but
column-major storage.  For cache efficiency on very wide tables, could batch
rows (e.g., 1024 at a time) and prefetch column data.  For typical widths
(5-50 columns) this isn't an issue — all column head pointers fit in L1.

### Null handling in the C layer

```c
// Per-column null check (inlined per type)
switch (col_type) {
    case TYPE_REAL:   if (isnan(dval)) { fputs(null_str, fp); break; } ...
    case TYPE_INT:    if (ival == INT_MIN) { fputs(null_str, fp); break; } ...
    case TYPE_STAMP:  if (lval == INT64_MIN) { fputs(null_str, fp); break; } ...
    case TYPE_STRING: if (code == 0 && pool[0][0] == '\0') { fputs(null_str, fp); break; } ...
}
```

### Testing

Test cases:
1. Round-trip: `csv-read` → DataTable → `csv-write` → `csv-read` → compare
2. Null handling: all-null columns, mixed nulls
3. String escaping: fields with commas, quotes, newlines
4. Timestamp precision: microsecond preservation in ISO format
5. Large file: 1M+ rows, verify streaming (no OOM)
6. Tab delimiter
7. Empty table (0 rows, header only)
8. Single column / single row edge cases

## Potential New Column Types

### What Lush Already Supports

Lush's storage layer (src/storage.c) supports more numeric types than
DataTable currently uses:

| Lush Type | C Type | Size | Matrix Constructor | DataTable Uses? |
|-----------|--------|------|--------------------|-----------------|
| `(-double-)` | double | 8B | `double-matrix` | yes (`'real`) |
| `(-float-)` | float | 4B | `float-matrix` | yes (`'float`) |
| `(-int-)` | int32 | 4B | `int-matrix` | yes (`'int`) |
| `(-long-)` | int64 | 8B | `long-matrix` | yes (`'stamp`) |
| `(-short-)` | int16 | 2B | `short-matrix` | **no** |
| `(-byte-)` | int8 | 1B | `byte-matrix` | **no** |
| `(-ubyte-)` | uint8 | 1B | `ubyte-matrix` | **no** |

There is no native boolean type.  Lush uses `t`/`()` (nil) at the
interpreter level and integers in compiled code.

### `'ubyte` Column — Unsigned Byte (0-255)

**Storage:** `idx1 of (-ubyte-)`, 1 byte per row.

**Use cases:**
- Boolean/flag columns (0/1) — 8x smaller than `'int`
- Categorical codes with < 256 levels (e.g., exchange ID, trade condition)
- Raw byte data (protocol flags, bitfields)
- Age, count, or score columns with small range

**Null sentinel:** 255 (0xFF).  Leaves 0-254 as valid values.  Alternatively
use 0 if the column is 1-indexed (but 255 is more conventional — it's the
unsigned equivalent of using INT_MIN / INT64_MIN for signed types).

**Memory savings:** A 10M-row boolean column:
- As `'int`: 40 MB
- As `'ubyte`: 10 MB
- As `'intcol` (dictionary, 2 values): 40 MB codes + 8 bytes pool ≈ 40 MB
  (dictionary encoding doesn't help when cardinality is low but every row
  still needs a 4-byte code)

So `'ubyte` beats both `'int` and `'intcol` for small-range data.

**What's already built out for ubyte:**
- Basic creation: `ubyte-matrix`, `ubyte-matrix-nc`
- Arithmetic: `idx-ub1add`, `idx-ub2add`, `idx-ub2mul` (limited set)
- Fill: `idx-ub1fill`, `idx-ub2fill`
- **No sorting** — `idx-sort.lsh` only has float/double/int sorts.
  Would need `idx-u1sortup` / `idx-u1sortdown` (trivial to add via
  counting sort — only 256 buckets, O(n) instead of O(n log n)).

**CSV format:** `sprintf "%u"` for 0-255 values.  Or for booleans,
optionally output "true"/"false" or "0"/"1".

**DataTable integration points:**
- `add-column`: `(ubyte-matrix capacity)` allocation
- `append-row`: `(ubyte val)` coercion — need bounds checking
- `get` / `set`: direct idx access (same as int)
- `grow-capacity`: `narrow` + `ubyte-matrix` like other numeric types
- `csv-read`: would need a type detection heuristic (all values 0-255
  and integer → promote to ubyte instead of int)
- `csv-write`: `sprintf "%u"` or `sprintf "%d"` for byte

### `'short` Column — Signed 16-bit Integer

**Storage:** `idx1 of (-short-)`, 2 bytes per row.

**Use cases:**
- Port numbers, small counts, year values
- Columns that would waste space as 4-byte int but need > 255 range
- Audio samples (16-bit PCM)

**Null sentinel:** -32768 (INT16_MIN), analogous to INT_MIN for `'int`.

**Memory:** 2 bytes/row vs 4 bytes/row for `'int` — 2x savings.

**Sorting:** Not built out.  Would need `idx-s1sortup` etc.

### `'byte` Column — Signed 8-bit Integer (-128 to 127)

Less useful than `'ubyte` in practice.  Most byte-oriented data is
unsigned (pixel values, flags, ASCII codes).  Could be added for
completeness but `'ubyte` covers the primary use case.

### `'bool` Column — Boolean (Packed Bits?)

No native Lush boolean type exists.  Two options:

**Option A: `'ubyte`-backed boolean.**  Use `'ubyte` with values 0/1
and null=255.  Simple, no new infrastructure.  1 byte per row.

**Option B: Bitpacked boolean.**  Store 8 booleans per byte using bit
operations.  For a 10M-row column: 1.25 MB vs 10 MB for ubyte.  But
requires custom bit-manipulation code for get/set/where, can't use
idx operations directly, and null handling is awkward (need a separate
null bitmap or sacrifice one bit).  The complexity isn't worth it unless
we have many boolean columns on very large tables.

**Recommendation:** Use `'ubyte` for booleans.  1 byte per row is cheap
enough, and it requires zero new infrastructure beyond the ubyte column
type.

### Impact on CSV Export

Adding new column types to `csv-write` is straightforward — just add
cases to the type switch in the C writer:

```c
case TYPE_UBYTE: {
    unsigned char val = ((unsigned char *)col_data[c])[r];
    if (val == 255) fputs(null_str, fp);  // null sentinel
    else fprintf(fp, "%u", val);
    break;
}
case TYPE_SHORT: {
    short val = ((short *)col_data[c])[r];
    if (val == -32768) fputs(null_str, fp);
    else fprintf(fp, "%d", val);
    break;
}
```

### Implementation Priority

1. **`'ubyte`** — most useful, covers booleans and small categoricals.
   Main gap is the lack of sorting support, which is a simple addition
   (counting sort for 256-value range).
2. **`'short`** — nice to have, 2x savings over int for medium-range values.
3. **`'byte`** / **`'bool`** — low priority, covered by ubyte.

### Counting Sort for ubyte

Since ubyte has only 256 possible values, a counting sort is optimal:

```c
// O(n) counting sort for ubyte column, producing permutation
void ubyte_counting_sort(unsigned char *data, int *perm, int n, int desc) {
    int counts[256] = {0};
    int offsets[256];
    for (int i = 0; i < n; i++) counts[data[i]]++;
    // Prefix sum (ascending or descending order)
    if (!desc) {
        offsets[0] = 0;
        for (int i = 1; i < 256; i++) offsets[i] = offsets[i-1] + counts[i-1];
    } else {
        offsets[255] = 0;
        for (int i = 254; i >= 0; i--) offsets[i] = offsets[i+1] + counts[i+1];
    }
    for (int i = 0; i < n; i++) perm[offsets[data[i]]++] = i;
}
```

This is faster than comparison-based sort (O(n) vs O(n log n)) and
produces a stable permutation index compatible with DataTable's
`grade` / `sort-by` methods.

## Alternatives Considered

**Pure Lush (interpreted) export:** Would work for small tables but
`timestamp-to-string` at ~3M/sec means a 10M-row table takes 3+ seconds
just for timestamps.  String escaping in interpreted Lush would be even
slower.  C is the right call.

**mmap output:** Write to an mmap'd file.  Not worth the complexity — the
sequential `fwrite` pattern is already optimal for I/O, and we can't know
the output size in advance without a pre-scan.

**Parallel column formatting:** Format each column independently into
per-column string buffers, then interleave into rows.  Uses more memory
(all formatted strings buffered) but could use OpenMP.  Overkill for V1;
the single-threaded streaming approach should saturate I/O bandwidth for
typical workloads.
