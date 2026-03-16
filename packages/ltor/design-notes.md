# LTOR Package for Lush — Design Notes

*LTOR: Lush TorQ-like database management system.*

LTOR is a TorQ-inspired architecture for real-time data capture, analytics,
and historical storage.  It uses ZMQ (`packages/zmq/`) as the transport
layer and provides the pipeline orchestration: feed handler, RDB,
analytics, HDB writer/reader, gateway, and monitoring.

## Package Structure

```
packages/ltor/
  ltor-feed.lsh           # Feed handler: libuv WS + ZMQ PUB + ROUTER
  ltor-rdb.lsh            # RDB: SUB + ROUTER
  ltor-analytics.lsh      # Analytics: SUB + ROUTER, direct FH subscription
  ltor-hdb-writer.lsh     # HDB Writer: SUB + ROUTER, direct FH subscription
  ltor-hdb-reader.lsh     # HDB Reader: ROUTER only
  ltor-gateway.lsh        # Gateway: ROUTER frontend + DEALER backends
  ltor-monitor.lsh        # REPL convenience functions
  coinbase-rdb.lsh        # Coinbase RDB business logic (transport-agnostic)
  coinbase-analytics.lsh  # Coinbase analytics logic (transport-agnostic)
  coinbase-hdb-writer.lsh # Coinbase HDB writer helpers (transport-agnostic)
  coinbase-hdb-reader.lsh # Coinbase HDB reader logic (transport-agnostic)
  design-notes.md         # This file
  scripts/
    coinbase-ltor-start.sh
    coinbase-ltor-stop.sh
    coinbase-ltor-{feed,rdb,analytics,hdb-writer,hdb-reader,gateway}.lsh
    monitor-query.lsh
  tests/
    (pipeline tests remain in packages/zmq/tests/ as they test transport)
```

## Port Assignments

| Process | Port | ZMQ Socket | Purpose |
|---------|------|------------|---------|
| Feed Handler PUB | 19970 | PUB | Broadcast ticks |
| RDB | 19971 | ROUTER | Serve queries |
| HDB Reader | 19972 | ROUTER | Serve historical queries |
| Analytics | 19973 | ROUTER | Serve analytics queries |
| Gateway | 19974 | ROUTER | Client frontend |
| HDB Writer | 19975 | ROUTER | Status queries |
| Feed Handler Control | 19976 | ROUTER | FH stats/control |

## Data Flow

```
Coinbase WSS ──libuv──→ Feed Handler
                         │
                         │ PUB tcp://*:19970
                         │ (topic: "ticker" or "l2")
              ┌──────────┼──────────┬──────────┐
              │ SUB      │ SUB      │ SUB      │
              ▼          ▼          ▼          │
           RDB        Analytics   HDB Writer   │
           ROUTER     ROUTER      ROUTER       │
           :19971     :19973      :19975       │
              │          │                     │
              └────┬─────┘                     │
                   │                           │
              Gateway ◄──── HDB Reader         │
              ROUTER       ROUTER              │
              :19974       :19972              │
```

## Key Architectural Features

1. **PUB/SUB fan-out**: FH broadcasts via ZMQ PUB.  HDB Writer and
   Analytics subscribe **directly to FH** (TorQ WDB pattern),
   eliminating RDB as intermediary.
2. **zmq_poll**: Unified event loop replaces WireServer's select() +
   register-fd hack.
3. **Topic filtering**: Subscribers receive only what they want
   (e.g., Analytics subscribes to "ticker" only).
4. **Auto-reconnect**: ZMQ handles reconnection natively.
5. **Smart routing**: Gateway auto-detects backend from query pattern
   (e.g., "vwap" → Analytics, "dates" → HDB Reader).
6. **Database-centric**: Each data-holding process wraps its DataTables
   in a `Database` object, enabling SQL queries on live data.
7. **SQL throughout**: All backends (RDB, Analytics, HDB Writer, HDB Reader)
   accept SQL strings (SELECT ... FROM ...).  The gateway parses the FROM
   clause to route SQL queries to the correct backend.

## Transport Layer

LTOR depends on the `zmq` package (`packages/zmq/`) for all transport:
- `zmq.lsh` — ZMQ socket API (ctx, socket, bind, connect, send, recv, poll)
- `zmq-serde.lsh` — bwrite/bread serialization over ZMQ frames

All LTOR process files `(libload "zmq/zmq-serde")` to access the
transport functions.

## Business Logic Separation (DB-Centric Refactor, 2026-03-16)

The `coinbase-*.lsh` files contain **all** transport-agnostic business logic:
- Table schemas and data insertion
- Query dispatch (including SQL via Database wrapper)
- Date-partition management, flush logic, date math
- Database object creation for each data-holding process
- Process helper functions (called by transport layer)

The `ltor-*.lsh` files are **pure ZMQ transport** infrastructure:
- ZMQ socket setup and teardown
- Event loop (poll, dispatch to coinbase-* functions)
- Heartbeat logging and monitoring
- **Zero business logic**: no `add-column`, no `assoc "product"`, no flush logic

### Verification invariants

These invariants should hold after any change:
- `grep "add-column" ltor-*.lsh` → 0 matches
- `grep "assoc.*product" ltor-*.lsh` → 0 matches (no field extraction)
- `grep "zmq-" coinbase-*.lsh` → 0 matches in code (only `;;;` header comments)

### Per-file summary

| File | Prefix | Key functions |
|------|--------|---------------|
| `coinbase-rdb.lsh` | `rdb-` | `rdb-init-tables`, `rdb-insert-ticker`, `rdb-process-ticker`, `rdb-process-l2`, `rdb-trim-tables`, `rdb-create-database`, `rdb-handle-query` (with SQL) |
| `coinbase-analytics.lsh` | `ana-` | `ana-init-table`, `ana-update-product`, `ana-process-ticker`, `ana-create-database`, `ana-handle-query` (with SQL) |
| `coinbase-hdb-writer.lsh` | `hdbw-` | `hdbw-init-tables`, `hdbw-reset-state`, `hdbw-insert-ticker`, `hdbw-insert-l2`, `hdbw-flush-table`, `hdbw-flush`, `hdbw-check-flush`, `hdbw-create-database`, `hdbw-handle-query` (with SQL) |
| `coinbase-hdb-reader.lsh` | `hdbr-` | `hdbr-discover`, `hdbr-get-table`, `hdbr-create-database`, `hdbr-create-range-database`, `hdbr-handle-query` (with SQL) |
| `ltor-rdb.lsh` | `ltor-rdb-` | `ltor-rdb-start` (SUB+ROUTER, calls rdb-*) |
| `ltor-analytics.lsh` | `ltor-ana-` | `ltor-ana-start` (SUB+ROUTER, calls ana-*) |
| `ltor-hdb-writer.lsh` | `ltor-hdbw-` | `ltor-hdbw-start` (SUB+ROUTER, calls hdbw-*) |
| `ltor-hdb-reader.lsh` | `ltor-hdbr-` | `ltor-hdbr-start` (ROUTER, calls hdbr-*) |
| `ltor-feed.lsh` | `ltor-fh-` | `ltor-fh-start` (PUB+CTRL, broadcast dispatch) |
| `ltor-gateway.lsh` | `ltor-gw-` | `ltor-gw-start` (ROUTER+DEALERs, SQL smart-routing) |
| `ltor-monitor.lsh` | `ltor-coinbase-` | `ltor-coinbase-sql`, `ltor-coinbase-status`, etc. |

## Data Directory

```
/datafast1/experiment/coinbasedata-zmq/
  YYYY.MM.DD/
    ticker/   (meta.lsh, col-*.col)
    l2/       (meta.lsh, col-*.col)
  .ctrl/      (PID files)
  logs/       (per-process log files)
```

## SQL Query Flow

End-to-end SQL query from REPL to backend and back:

```
User REPL
  (ltor-coinbase-sql "SELECT * FROM ticker WHERE product = 'BTC-USD' LIMIT 10")
      │
      ▼
  ltor-monitor.lsh: _ltor-monitor-query(sql-string)
      │  ZMQ DEALER → ROUTER
      ▼
  ltor-gateway.lsh: _ltor-gw-smart-route("SELECT...")
      → regex-seek "FROM TICKER" → routes to "rdb"
      │  ZMQ DEALER → ROUTER
      ▼
  ltor-rdb.lsh: rdb-handle-query(sql-string)
      → detects "SELECT" prefix
      → rdb-create-database()
        → (new Database ())
        → attach "ticker" *rdb-ticker-table*
        → attach "l2" *rdb-l2-table*
      → (==> db query sql-string)
        → sql-parse → clause alist → _query-execute
      → wire-pack-datatable(result)
      │
      ▼  (back through gateway, monitor)
  → wire-unpack-datatable → DataTable returned to user
```

Gateway SQL routing logic (in `_ltor-gw-smart-route`):
- `FROM TICKER` or `FROM L2` → "rdb" backend
- `FROM ANALYTICS` → "ana" backend
- Default → "rdb" backend

## Production Hardening

- **30-second heartbeat logging** on all 6 processes (ticker count, query count, uptime)
- **RDB table trimming** at 100K rows (keeps newest 50K) to prevent OOM
- **Nil-safe shutdown** on all 6 processes (`when` guards on zmq-close)
- **HDB Writer midnight rollover**: uses data timestamps, not wall-clock;
  splits batches spanning midnight
- **FH stale connection detection**: if no data for 60s, forces reconnect
- **stdout line buffering**: `stdbuf -oL` on all process launches
