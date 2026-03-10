# R Client for Historical Data Queries — Research Notes

## Decision: Wire Protocol (not ZMQ)

Wire is the right transport for an R client. Both protocols use the same
underlying serialization (bwrite/bread), but wire is far simpler:

- **16-byte fixed header** (magic "LWIR" + version + msgtype + encoding + length)
  vs ZMQ which requires linking libzmq
- **No external dependencies** — R's native `socketConnection()` is sufficient
- **Built-in text mode** (encoding=0 → sexp format) — ZMQ only supports binary
- **Synchronous RPC** matches R's blocking evaluation model
- **~200-300 lines of R** vs ~500+ for ZMQ + binding

Wire was explicitly modeled on kdb+ IPC (wire.lsh header says "inspired by
Q/kdb+ IPC, TorQ, and Kerf1's wire.c").

## The kdb+/R Pattern (rkdb on CRAN)

rkdb is the gold standard for this kind of connector. Entire API is 3 functions:

```r
h <- open_connection("localhost", 5000)
result <- execute(h, "select avg price by sym from trade")  # → data.frame
close_connection(h)
```

Key design choices confirmed across all successful R database connectors
(rkdb, mongolite, DBI, elastic, redux):

1. **Always return data.frame** for tabular results
2. **Type conversion in C** via .Call() for performance (rkdb does a switch
   on q type codes in C, builds R SEXP objects directly)
3. **Minimal API** — rkdb has 3 functions, no OOP classes needed
4. **Thin wrapper** — pass through the database's query language as a string,
   don't try to wrap it in R methods
5. **Explicit connect/disconnect** lifecycle

## Type Mapping: Lush DataTable → R data.frame

| Lush type | Wire representation | R type |
|-----------|-------------------|--------|
| real | double (8 bytes) | numeric |
| int | int (4 bytes) | integer |
| float | float (4 bytes) | numeric |
| stamp | long (8 bytes) | numeric or POSIXct |
| string | list of strings | character |

Stamp columns are microsecond epoch timestamps — should convert to POSIXct
in R for usability. The `bit64` package could preserve full precision if needed.

## Wire Protocol Details for R Implementation

### Header (16 bytes)

```
Bytes 0-3:   Magic "LWIR" (0x4C574952)
Byte 4:      Version (1)
Byte 5:      MsgType (0=async, 1=sync, 2=response, 3=error, 4=heartbeat)
Byte 6:      Encoding (0=sexp/text, 1=binary/bwrite, 2=compressed/LZ4)
Byte 7:      Reserved (0)
Bytes 8-11:  MsgID (uint32 little-endian)
Bytes 12-15: PayloadLen (uint32 little-endian)
```

### Query Flow

1. Client opens TCP socket
2. Sends: 16-byte header (msgtype=1/sync, encoding=0/sexp) + UTF-8 query string
3. Reads: 16-byte response header → extract payload length
4. Reads: payload bytes → parse sexp text into data.frame

### S-expression Response Format

From `wire-datatable-to-sexp` in wire-serialize.lsh:

```
((names ("product" "price" "bid" "ask" "side" "volume" "time" "local_time"))
 (types (string real real real string real stamp stamp))
 (nrows 1234)
 (data (("BTC-USD" "ETH-USD" ...)
        (50000.5 51000.2 ...)
        (49999.0 50999.0 ...)
        ...)))
```

Schema is explicit (names, types, nrows). Data is columnar. Straightforward
to parse into a data.frame in R.

**Caveat**: sexp mode uses `%g` format for doubles (~6 significant digits).
For full precision, binary mode (encoding=1) preserves exact IEEE 754 doubles
but requires parsing Lush's bwrite format.

### Available Queries (from coinbase-hdb-reader.lsh)

```
"dates"                                    → list of date strings
"tables"                                   → list of table names
("count" date tbl)                         → integer
("snapshot" date tbl)                      → packed DataTable
("last" date tbl N)                        → last N rows
("select" date tbl col-list)               → column projection
("where" date tbl op col val)              → filtered rows
("range" tbl date-from date-to)            → multi-date concatenation
("range-where" tbl from to op col val)     → filtered range
"stats"                                    → alist of server stats
```

## Two Implementation Options

### Option A: Pure R + sexp encoding (start here)

- Parse sexp text response in R using string manipulation
- No compiled code, works everywhere, easy to debug
- Performance adequate for <100K rows
- ~200-300 lines of R

### Option B: R + C via .Call() (later, if needed)

- Parse binary bwrite format directly into R SEXPs in C
- Much faster for large datasets (millions of rows)
- Requires compiling C code (like rkdb does)
- Could reuse Lush's wire_helpers.c for header parsing
- Would need a bwrite→SEXP type dispatch function (similar to rkdb's
  `from_any_kobject`)

## Server-Side Requirement

The HDB Reader business logic is now transport-agnostic in
`packages/zmq/coinbase-hdb-reader.lsh`. To serve wire clients, need one of:

1. **Standalone wire HDB server** — new .lsh file that creates a WireServer,
   sets on-sync callback to `hdbr-handle-query`, enters serve loop.
   Simplest option, ~30 lines of Lush.

2. **Add wire listener to existing ZMQ HDB Reader** — zmq-hdb-reader.lsh
   would also bind a WireServer port. More complex (two event loops).

Option 1 is cleaner — a dedicated `wire-hdb-reader.lsh` process on its own
port, using the same `coinbase-hdb-reader.lsh` business logic.

## Reference Files

- Wire protocol: `packages/wire/wire.lsh` (header format, client/server API)
- Wire serialization: `packages/wire/wire-serialize.lsh` (pack/unpack/sexp)
- HDB query logic: `packages/zmq/coinbase-hdb-reader.lsh` (transport-agnostic)
- ZMQ serde: `packages/zmq/zmq-serde.lsh` (for comparison)
- rkdb source: https://github.com/KxSystems/rkdb (3-function API, C type dispatch)
- kdb+ IPC docs: https://code.kx.com/q/basics/ipc/ (8-byte header, self-describing)
