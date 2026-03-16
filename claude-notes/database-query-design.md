# Design Notes: Database as a Query Server

## Status: Design Document (2026-03-14, rev 2; updated 2026-03-16 — Phases 1, 3, 6 complete)

---

## 1. The Vision

The goal is a TorQ-like data platform in Lush: warehouse data, serve live data
to trading algorithms and charts, and make historical data available to
researchers — all through a gateway that routes queries to the right backend
and enforces access control.

The Database object exists to be at the **end of a wire**. Someone at the REPL
with direct access to DataTables can already use the lispy `db-select` macros
perfectly well. The Database becomes useful when it's the thing a remote client
talks to — an R session running a backtest, a dashboard pulling live prices, a
researcher who needs last month's trades but shouldn't see the live RDB.

This refocuses the entire design: the Database is not a local convenience
wrapper, it's the server-side query endpoint.

---

## 2. Architecture: How TorQ Does It, How LTOR Should Evolve

### 2.1 TorQ Reference Architecture

```
Feed Handlers → Tickerplant → RDB (in-memory today)
                            → WDB (periodic write-down)
                            → HDB (compressed historical, date-partitioned)

Clients → Gateway → routes to RDB / HDB / Analytics / etc.
                  → combines results (join across date boundaries)
                  → enforces permissions per user/group
                  → load-balances across redundant processes
```

Key TorQ patterns:
- **Gateway is the single entry point.** Clients never connect directly to backends.
- **Stored procedures, not ad-hoc eval.** The gateway exposes named query functions, not raw q execution. This is both safer and simpler to permission.
- **Permissioning at the gateway.** Each user has groups/roles. The gateway checks entitlements before dispatching. Individual backends trust the gateway.
- **Discovery service.** Processes self-register; the gateway discovers what's available dynamically.
- **Aggregation at the service level.** Don't pull large datasets through the gateway — filter and aggregate close to the data, return compact results.

### 2.2 Current LTOR Architecture (Updated 2026-03-16)

```
Feed Handler (PUB 19970) → RDB (ROUTER 19971)          [Database: ticker, l2]
                         → Analytics (ROUTER 19973)     [Database: analytics]
                         → HDB Writer (ROUTER 19975)    [Database: ticker, l2 (in-flight)]

HDB Reader (ROUTER 19972) ← reads disk                 [Database: per-date tables]

Gateway (ROUTER 19974) → DEALER to each backend
                       → smart-route by query command name
                       → SQL routing by FROM clause
                       → no permissioning
                       → no discovery (hardcoded ports)

Monitor (ltor-coinbase-sql) → Gateway → backend Database.query()
```

### 2.3 What's Missing vs TorQ

| TorQ Feature | LTOR Status | Priority |
|---|---|---|
| Gateway query routing | Exists: command-based + SQL FROM routing (2026-03-16) | Done |
| Permissioning | None | High for multi-user |
| General query language over wire | **Done** — SQL via Database.query() on all backends (2026-03-16) | Done |
| Discovery service | None (hardcoded ports in scripts) | Nice to have |
| Load balancing | None (single instance per backend) | Future |
| Cross-process joins (RDB + HDB) | Gateway could do this but doesn't | High value |
| Result aggregation at service level | Partial (HDB does multi-date concat) | Adequate |

---

## 3. The Database Object: Redefined Role

### 3.1 What It Is Now (Updated 2026-03-16)

A query endpoint: htable of name → DataTable, with `query` method accepting
SQL strings.  Used by all four LTOR data-holding backends (RDB, Analytics,
HDB Writer, HDB Reader) to serve SQL queries.  Each backend creates a
Database wrapper around its DataTables on demand (or maintains one
persistently, as HDB Writer does with `*hdbw-db*`).

### 3.2 What It Should Become

The **server-side query endpoint** — the object that sits behind a wire
connection and answers queries. Each LTOR process type wraps a Database:

```
RDB process:
  Database (volatile, in-memory)
    ├─ "ticker" → DataTable (live ticker data)
    └─ "l2"     → DataTable (live L2 data)
  Accepts queries over wire/ZMQ
  Handles: SELECT, WHERE, LAST, COUNT, SNAPSHOT

HDB process:
  Database (persisted, date-partitioned)
    ├─ "2026.03.13/ticker" → DataTable (lazy-loaded from disk)
    ├─ "2026.03.13/l2"     → DataTable
    └─ ...
  Accepts queries over wire/ZMQ
  Handles: SELECT, WHERE, RANGE queries across dates

Analytics process:
  Database (volatile, computed)
    └─ "analytics" → DataTable (running stats)
  Accepts queries + named functions (mavg, vwap, etc.)
```

The Gateway connects to multiple Database-serving processes and routes queries
based on user permissions and data location.

### 3.3 What Changes

The Database class needs:
1. A real `query` method (replacing the stub)
2. A `serve` method (bind to a wire port, answer queries)
3. Table-level metadata for permissioning (which tables are public, which restricted)
4. Schema introspection commands (`tables`, `meta`, `schema`)

It does NOT need:
- To replace the existing LTOR process code overnight
- To be the only way to serve data (existing bespoke handlers can coexist)
- An ORM or object-relational mapping layer

---

## 4. Query Language: Two Tiers

### 4.1 Tier 1: Lispy DSL (Local Use)

Already exists. `db-select` / `db-count` macros expand at compile time. Perfect
for interactive Lush sessions where you have direct DataTable access.

```lisp
(db-select ("price" "symbol")
  (from trades-dt)
  (where (and (= "symbol" "BTC-USD") (> "price" 50000)))
  (order-by "time" 'desc)
  (limit 100))
```

No changes needed. This is the in-process query interface.

### 4.2 Tier 2: Flat Query String (Wire Use)

A simple SQL-like string language for queries arriving over the wire from R,
Python, shell scripts, or any non-Lush client. This is the primary
justification for the state-machine parser.

```
SELECT price, symbol FROM trades WHERE price > 100 ORDER BY price DESC LIMIT 10

SELECT * FROM ticker WHERE symbol = 'BTC-USD' AND time > 1710000000

SELECT symbol, SUM(volume) FROM trades GROUP BY symbol

SELECT * FROM trades JOIN quotes ON symbol

SELECT COUNT(*) FROM ticker WHERE symbol IN ('BTC-USD', 'ETH-USD')
```

**Why flat strings over structured lists?**

The previous draft recommended structured lists first. But rethinking from the
wire-client perspective changes the calculus:

- **R clients** will call `execute(h, "SELECT ...")` — exactly the rkdb pattern
  from `r-client-research.md`. Constructing Lush S-expression lists from R is
  painful; constructing SQL strings is trivial.
- **Python clients** same story: `conn.query("SELECT ...")` is natural.
- **Shell/monitoring scripts**: `echo "SELECT count(*) FROM ticker" | lush-client`
- **Permissioning**: easier to parse, validate, and restrict a flat string than
  to inspect arbitrary nested list structures for banned operations.
- **Logging/auditing**: flat strings are human-readable in query logs.

The structured list format is still useful internally (Lush-to-Lush IPC), but
the flat string is what external clients will actually use.

### 4.3 SQL Subset Specification

The parser needs to handle a practical subset, not full SQL. Modeled on what
`db-select` already supports:

```
query       := select_stmt
select_stmt := SELECT columns FROM table_ref
               [JOIN table_ref ON col_name [LEFT|RIGHT|ASOF]]
               [WHERE predicate]
               [GROUP BY col_name agg_spec]
               [HAVING predicate]
               [ORDER BY col_name [ASC|DESC]]
               [LIMIT number]

columns     := * | col_list
col_list    := col_expr [, col_expr]*
col_expr    := col_name | expr AS alias

table_ref   := table_name                    -- resolved from Database catalog

predicate   := condition
             | predicate AND predicate
             | predicate OR predicate
             | ( predicate )

condition   := col_name op value
             | col_name BETWEEN value AND value
             | col_name IN ( value [, value]* )
             | col_name NOT IN ( value [, value]* )
             | col_name LIKE 'pattern'

op          := = | <> | != | < | > | <= | >=

value       := number | 'string' | "string"

expr        := col_name | number | expr arith_op expr | func( expr )
arith_op    := + | - | * | /
func        := ABS | SQRT | LOG | FLOOR | CEIL

agg_spec    := , agg_func( col_name )
agg_func    := COUNT | SUM | MEAN | AVG | MIN | MAX

count_stmt  := SELECT COUNT(*) FROM table_ref [WHERE predicate]
```

Extensions beyond standard SQL (matching `db-select` capabilities):
- `ASOF` join type (no SQL equivalent; used for time-series lookups)
- `MEAN` as alias for `AVG` (matching internal naming)
- Timestamp literals: `'2026.03.14'` or epoch integers

### 4.4 Parser Architecture

A two-stage pipeline:

```
SQL string → Tokenizer → token list → Parser → clause list → _query-execute
```

**Tokenizer**: Split on whitespace, respecting quoted strings. Recognize
keywords (SELECT, FROM, WHERE, etc.), operators, numbers, and identifiers.
Handle both `'single'` and `"double"` quoted strings. ~100 lines.

**Parser**: Recursive descent, one function per clause type. Each function
consumes tokens and appends to a clause alist. Error reporting includes the
token position. ~300 lines.

**Total**: ~400-500 lines of Lush, no C needed. The output is the same clause
alist that `_query-execute` already consumes.

---

## 5. Database Server: Wire Protocol

### 5.1 Transport: Wire (not ZMQ) for External Clients

The `r-client-research.md` already concluded: Wire is the right transport for
external clients. The reasons hold even more strongly now:

- **16-byte header + payload** — trivial to implement in R/Python/any language
- **No libzmq dependency** — R's native `socketConnection()` suffices
- **Text mode** (encoding=0) sends S-expression responses that are easy to parse
- **Binary mode** (encoding=1) for high-performance bulk transfers
- **Synchronous RPC** matches the query-response pattern
- **Already modeled on kdb+ IPC** — the rkdb pattern maps directly

ZMQ remains the internal transport between LTOR processes (pub/sub fan-out,
ROUTER/DEALER multiplexing). Wire is the external-facing protocol.

### 5.2 Server Implementation

The Database class gets a `serve` method that wraps a WireServer:

```lisp
(defmethod Database serve (port)
  (let ((srv (new WireServer port)))
    (setq :srv:on-sync
      (lambda (fd expr encoding)
        (==> this _handle-query fd expr encoding)))
    (==> srv start)
    (==> srv serve)))
```

The `_handle-query` method dispatches:

```lisp
(defmethod Database _handle-query (fd expr encoding)
  (cond
    ;; Introspection
    ((= expr "ping")     "pong")
    ((= expr "tables")   table-names)
    ((= expr "schema")   (_db-schema this))
    ;; String query → SQL parser → _query-execute
    ((stringp expr)      (_db-query-string this expr))
    ;; List query → direct → _query-execute
    ((listp expr)        (_db-query-list this expr))
    (t (list 'error "unknown query format"))))
```

Result serialization:
- DataTable results → `wire-pack-datatable` (binary) or `wire-datatable-to-sexp` (text)
- Scalar results (count, ping) → direct S-expression
- Errors → `(error "message")`

### 5.3 How an R Session Talks to It

```r
# R client (pure R, ~200 lines, per r-client-research.md)
h <- lush_connect("server", 5555)
trades <- lush_query(h, "SELECT * FROM ticker WHERE symbol = 'BTC-USD' LIMIT 100")
# trades is a data.frame with columns: product, price, bid, ask, side, volume, time

vwap <- lush_query(h, "SELECT symbol, SUM(price * volume) / SUM(volume) AS vwap
                        FROM ticker GROUP BY symbol")
lush_close(h)
```

This is the rkdb pattern: `open_connection` / `execute` / `close_connection`,
returning `data.frame`. The wire protocol makes this possible with zero
external dependencies on the R side.

---

## 6. Gateway Evolution

### 6.1 Current Gateway: Domain-Specific Router

The LTOR gateway hardcodes smart-routing rules for Coinbase commands:
- `"vwap"` → Analytics
- `"dates"` → HDB Reader
- `"snapshot"` → RDB (default)

This works for the Coinbase pipeline but doesn't generalize.

### 6.2 Evolved Gateway: Database-Aware Router

The gateway becomes a thin proxy that:
1. Accepts client connections (wire protocol for external, ZMQ for internal)
2. Parses the query to determine which backend(s) to hit
3. Checks user permissions
4. Dispatches to the appropriate Database-serving process
5. Combines results if the query spans multiple backends (RDB + HDB)
6. Returns the result to the client

**Query routing logic** (replacing the hardcoded smart-route):

```
FROM "ticker" + no date constraint → RDB (today's data)
FROM "ticker" + date range in past → HDB
FROM "ticker" + date range spanning today → HDB + RDB, concatenate results
Named functions (VWAP, MAVG) → Analytics
```

This is the TorQ pattern: the gateway inspects the query, determines which
process types have the data, dispatches, and joins results.

### 6.3 Cross-Process Joins

The highest-value gateway feature: a researcher queries
`SELECT * FROM ticker WHERE time > '2026.03.01'` and the gateway
automatically splits this into an HDB query (March 1-13) and an RDB query
(March 14 today), concatenates the results, and returns a single DataTable.

TorQ calls this the "join function." In our case, date-partitioned data
naturally splits across RDB (today) and HDB (history). The gateway needs:
1. Date range detection from the WHERE clause
2. Knowledge of which dates the HDB has vs what the RDB holds
3. Concatenation of wire-packed DataTables from both backends

### 6.4 Migration Path

The gateway doesn't need to be rewritten. Evolution:

1. **Now**: LTOR gateway continues working unchanged for existing Coinbase commands
2. **Add**: A `"query"` command that accepts SQL strings and routes them
3. **Add**: Wire protocol listener on the gateway (alongside ZMQ ROUTER)
4. **Add**: Permission checking before dispatch
5. **Eventually**: The bespoke commands (`"vwap"`, `"snapshot"`, etc.) become
   convenience aliases for specific SQL queries

---

## 7. Permissioning

### 7.1 TorQ Permission Model (Reference)

TorQ provides:
- User/password authentication (process-level)
- User groups and roles (function/table/variable access)
- Virtual tables (partial table access — e.g., only certain symbols)
- Gateway-level permissioning (check before dispatch, not at each backend)
- Public user support (anonymous read-only access)
- Admin role with full access

### 7.2 Lush Permissioning: Minimum Viable

For the initial implementation, a simple table-level access control:

```lisp
;; Permission table: user → list of allowed table names (or * for all)
(defmethod Database set-permissions (perms)
  ;; perms is an alist: (("researcher" ("ticker" "analytics"))
  ;;                      ("trader" *)
  ;;                      ("public" ("analytics")))
  (setq db-permissions perms))
```

The query handler checks permissions before executing:
```lisp
(defmethod Database _check-access (user table-name)
  (let ((allowed (cadr (assoc user db-permissions))))
    (or (= allowed '*)
        (member table-name allowed))))
```

Wire connections carry a username (set during handshake or from connection
metadata). If a query references a table the user can't access, return an
error before executing.

### 7.3 What This Enables

| User Type | Access | Use Case |
|---|---|---|
| `trader` | RDB (live), Analytics | Real-time prices, VWAP, spread |
| `researcher` | HDB (historical) | Backtesting, analysis |
| `admin` | Everything | System monitoring, debugging |
| `public` | Analytics summary only | Dashboard displays |
| `algo` | RDB + Analytics | Trading algorithm inputs |

---

## 8. Implementation Plan

### Phase 1: SQL Parser + Database.query() (~500 lines) — DONE

The SQL parser is the critical enabler. Without it, external clients can't
issue queries in a natural way.

1. **Tokenizer** (`datatable-sql.lsh`): ~100 lines — Done
2. **Parser** (`datatable-sql.lsh`): ~300 lines — Done
3. **Database.query()** (`columnardb-database.lsh`): ~50 lines — Done
4. **Tests**: ~150 lines — Done (1961 datatable tests pass)

### Phase 2: Database.serve() + Wire Query Server (~200 lines)

1. **Database.serve()**: WireServer wrapper with on-sync callback
2. **Query dispatch**: ping, tables, schema, SQL string, structured list
3. **Result packing**: wire-pack-datatable for DataTable results
4. **Standalone server script**: `db-server.lsh` (launch from command line)

### Phase 3: Gateway SQL Support (~100 lines) — DONE (2026-03-16)

1. ~~Add `"query"` command to LTOR gateway~~ — SQL strings flow through smart-route
2. Gateway parses SQL FROM clause to determine routing (`_ltor-gw-smart-route`)
3. Cross-process query: split date ranges across HDB + RDB, concatenate results — **not yet implemented**
4. Bespoke commands remain as fast-path aliases

### Phase 4: Permissioning (~150 lines)

1. Permission table on Database (user → table list)
2. Wire handshake carries username
3. Gateway checks permissions before dispatch
4. Denied queries return error without executing

### Phase 5: R Client (~250 lines of R)

1. `lush_connect(host, port)` → wire TCP socket
2. `lush_query(h, "SELECT ...")` → data.frame
3. `lush_close(h)` → disconnect
4. Type mapping: real→numeric, int→integer, stamp→POSIXct, string→character
5. Pure R, no compiled code (per r-client-research.md Option A)

### Phase 6: LTOR Backend Convergence — DONE (2026-03-16)

1. ~~Refactor RDB/HDB/Analytics to wrap their DataTables in Database objects~~ — Done:
   - `rdb-create-database` wraps ticker + l2 tables
   - `ana-create-database` wraps analytics table
   - `hdbw-create-database` wraps HDB Writer's in-flight tables (with `*hdbw-db*` auto-managed)
   - `hdbr-create-database` / `hdbr-create-range-database` wraps HDB Reader's loaded tables
2. ~~Replace bespoke query dispatch with `Database.query()`~~ — All four query handlers now accept SQL strings
3. Bespoke commands remain as fast-path aliases (coexist with SQL)
4. ~~All backends gain SQL query capability for free~~ — Done via Database wrapper
5. All business logic moved from `ltor-*.lsh` → `coinbase-*.lsh`; transport files are pure ZMQ
6. `ltor-coinbase-sql` added to monitor for REPL convenience

---

## 9. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| SQL parser bugs (wrong parse) | Medium | Medium | Extensive test suite, start with small subset |
| SQL parser incomplete (missing feature) | High initially | Low | Iterative: add features as needed |
| Performance: parsing overhead per query | Low | Low | Parse is O(query length), negligible vs data scan |
| Security: SQL injection | N/A | N/A | No eval; parsed into validated structure |
| LTOR backend refactor breaks things | Medium | High | Phase 6 is optional; existing code works |
| Wire server stability | Low | Medium | WireServer already tested (665 wire tests) |
| R client complexity | Low | Low | Pure R, no C; rkdb pattern is proven |

---

## 10. Open Questions

1. **Date-partitioned tables in Database.** Currently each date partition is a
   separate directory. Should the Database object understand date-partitioning
   natively (like kdb+ HDB), or should date ranges be handled at the gateway
   level? Leaning gateway — keeps Database simple.

2. **LIMIT pushdown.** For cross-process queries (RDB + HDB), should the
   gateway push LIMIT to each backend and then re-limit the merged result?
   Yes, for efficiency — but ORDER BY must also be pushed down for correctness.

3. **Aggregation pushdown.** For `GROUP BY` queries spanning RDB + HDB, the
   gateway can't just concatenate — it needs to merge partial aggregates.
   `SUM` and `COUNT` are mergeable; `MEAN` requires `SUM/COUNT`; `MIN`/`MAX`
   take the extremes. This is the hardest part of cross-process queries.

4. **Prepared statements / query caching.** Should the parser cache parsed
   queries by string hash? Probably not initially — parse time is negligible
   compared to data access. Revisit if profiling shows otherwise.

5. **Async queries.** TorQ's gateway supports async queries (client sends
   request, continues work, retrieves result later). WireServer supports async
   messages (msgtype=0). Worth adding but not in the first pass.

6. **Multiple result sets.** Should a query be able to return multiple tables
   (e.g., `SELECT * FROM trades; SELECT * FROM quotes`)? Not in v1 — one
   query, one result. Batch queries can come later.

---

## 11. File Map (Actual State as of 2026-03-16)

```
packages/datatable/
  datatable-sql.lsh          -- DONE: SQL tokenizer + parser
  datatable-query.lsh        -- DONE: _query-execute pipeline
  columnardb-database.lsh    -- DONE: Database class with query() method
  tests/test-sql.lsh         -- DONE: SQL parser tests
  tests/test-db-query.lsh    -- DONE: end-to-end Database.query tests

packages/ltor/
  coinbase-rdb.lsh           -- DONE: rdb-create-database, SQL in rdb-handle-query
  coinbase-analytics.lsh     -- DONE: ana-create-database, SQL in ana-handle-query
  coinbase-hdb-writer.lsh    -- DONE: hdbw-create-database, SQL in hdbw-handle-query
                                (all business logic absorbed from ltor-hdb-writer.lsh)
  coinbase-hdb-reader.lsh    -- DONE: hdbr-create-database, SQL in hdbr-handle-query
  ltor-rdb.lsh               -- DONE: pure ZMQ transport (calls rdb-*)
  ltor-analytics.lsh         -- DONE: pure ZMQ transport (calls ana-*)
  ltor-hdb-writer.lsh        -- DONE: pure ZMQ transport (calls hdbw-*)
  ltor-gateway.lsh           -- DONE: SQL routing via FROM clause in _ltor-gw-smart-route
  ltor-monitor.lsh           -- DONE: ltor-coinbase-sql / ltor-coinbase-sql-raw

(future)
  ltor-db-server.lsh         -- standalone Database wire server (Phase 2)
  packages/rlush/            -- R client package (Phase 5)
```
