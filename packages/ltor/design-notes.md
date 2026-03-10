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

## Transport Layer

LTOR depends on the `zmq` package (`packages/zmq/`) for all transport:
- `zmq.lsh` — ZMQ socket API (ctx, socket, bind, connect, send, recv, poll)
- `zmq-serde.lsh` — bwrite/bread serialization over ZMQ frames

All LTOR process files `(libload "zmq/zmq-serde")` to access the
transport functions.

## Business Logic Separation

The `coinbase-*.lsh` files contain transport-agnostic business logic:
- Table schemas and data insertion
- Query dispatch and analytics computation
- Date-partition management

The `ltor-*.lsh` files contain the process orchestration:
- ZMQ socket setup and teardown
- Event loop (poll, dispatch)
- Heartbeat logging and monitoring

## Data Directory

```
/datafast1/experiment/coinbasedata-zmq/
  YYYY.MM.DD/
    ticker/   (meta.lsh, col-*.col)
    l2/       (meta.lsh, col-*.col)
  .ctrl/      (PID files)
  logs/       (per-process log files)
```

## Production Hardening

- **30-second heartbeat logging** on all 6 processes (ticker count, query count, uptime)
- **RDB table trimming** at 100K rows (keeps newest 50K) to prevent OOM
- **Nil-safe shutdown** on all 6 processes (`when` guards on zmq-close)
- **HDB Writer midnight rollover**: uses data timestamps, not wall-clock;
  splits batches spanning midnight
- **FH stale connection detection**: if no data for 60s, forces reconnect
- **stdout line buffering**: `stdbuf -oL` on all process launches
