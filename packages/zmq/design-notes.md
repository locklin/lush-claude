# ZeroMQ Package for Lush — Design Notes

*Moved from `claude-notes/zeromq-design-notes.md` into the package.*

## Implementation Status

**Implemented: Option A (Full ZMQ)** — a separate ZMQ-based pipeline
(`packages/zmq/`) on ports 19970-19976, running alongside the libuv
pipeline on ports 19960-19965.

### Package Structure

```
packages/zmq/
  zmq-c.h              # Minimal ZMQ API declarations (~90 lines)
  zmq-c.c              # C bridge: handle table + wrapper functions
  zmq-config.lsh        # lushmake build: compile zmq-c.c, link -lzmq
  zmq.lsh              # Main loader: DHC wrappers + high-level API
  zmq-serde.lsh         # bwrite/bread serialization over ZMQ frames
  zmq-feed.lsh          # Feed handler: libuv WS + ZMQ PUB + ROUTER
  zmq-rdb.lsh           # RDB: SUB + ROUTER
  zmq-analytics.lsh     # Analytics: SUB + ROUTER, direct FH subscription
  zmq-hdb-writer.lsh    # HDB Writer: SUB + ROUTER, direct FH subscription
  zmq-hdb-reader.lsh    # HDB Reader: ROUTER only
  zmq-gateway.lsh       # Gateway: ROUTER frontend + DEALER backends
  zmq-monitor.lsh       # REPL convenience functions
  design-notes.md        # This file
  scripts/
    coinbase-zmq-start.sh
    coinbase-zmq-stop.sh
    coinbase-zmq-{feed,rdb,analytics,hdb-writer,hdb-reader,gateway}.lsh
  tests/
    test-zmq.lsh          # Binding unit tests
    test-zmq-pipeline.lsh # Integration tests
```

### Port Assignments

| Process | Port | ZMQ Socket | Purpose |
|---------|------|------------|---------|
| Feed Handler PUB | 19970 | PUB | Broadcast ticks |
| RDB | 19971 | ROUTER | Serve queries |
| HDB Reader | 19972 | ROUTER | Serve historical queries |
| Analytics | 19973 | ROUTER | Serve analytics queries |
| Gateway | 19974 | ROUTER | Client frontend |
| HDB Writer | 19975 | ROUTER | Status queries |
| Feed Handler Control | 19976 | ROUTER | FH stats/control |

### Data Flow

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

### Key Architectural Improvements over libuv Pipeline

1. **PUB/SUB fan-out**: FH broadcasts via ZMQ PUB.  HDB Writer and
   Analytics subscribe **directly to FH** (TorQ WDB pattern),
   eliminating RDB as intermediary.
2. **zmq_poll**: Unified event loop replaces WireServer's select() +
   register-fd hack.
3. **Topic filtering**: Subscribers receive only what they want
   (e.g., Analytics subscribes to "ticker" only).
4. **Auto-reconnect**: ZMQ handles reconnection natively.

### Data Directory

ZMQ pipeline data is stored separately:
- libuv: `/datafast1/experiment/coinbasedata/`
- ZMQ: `/datafast1/experiment/coinbasedata-zmq/`

---

## Original Design Notes

*(Preserved below for reference — the original analysis that motivated
the ZMQ implementation.)*

# ZeroMQ as an Alternative to Wire + libuv

## Motivation

The current Coinbase pipeline uses two layers for inter-process communication:

1. **Wire protocol** (`packages/wire/wire.lsh`) — custom binary framing
   (4-byte header + bwrite/bread payload), select()-based server, incremental
   non-blocking reads, heartbeat, compression (LZ4 + byte-transpose)
2. **libuv** (`packages/libuv/libuv.lsh`) — async event loop for WebSocket,
   timers, signal handling

The `register-fd` addition (registering the feed handler's fd with the
RDB's WireServer so both FH broadcasts and client queries go through one
`select()`) mirrors kdb+'s approach.  But this is fundamentally what
ZeroMQ was designed to eliminate — ZeroMQ provides message-level
abstractions over sockets with built-in patterns for pub/sub, request/reply,
load-balanced routing, and reconnection, all multiplexed through `zmq_poll`.

## ZeroMQ Patterns and Their Pipeline Equivalents

### PUB/SUB — Feed Handler Broadcast

The feed handler broadcasts parsed ticks to all downstream processes.
Today this is WireServer.broadcast() with manual connection tracking.

**ZMQ equivalent:** The feed handler binds a `PUB` socket.  Downstream
processes (RDB, HDB Writer, Analytics) connect with `SUB` sockets and
set topic filters.

**Advantages over wire broadcast:**
- No connection tracking needed (PUB socket handles it)
- Topic-based filtering — subscribers only receive what they want
- `ZMQ_SNDHWM` drops messages to slow subscribers rather than blocking
- Automatic reconnection on subscriber side
- No need for `_add-client`, `_remove-client`, fd tracking

### ROUTER/DEALER — Query Request/Reply

Backend processes bind a `ROUTER` socket.  The Gateway (or REPL)
connects with a `DEALER` socket.  The `ROUTER` socket automatically
tracks client identity, so responses go back to the right requester.

### zmq_poll — Unified Event Loop

The key win.  One `zmq_poll` multiplexes ZMQ sockets and raw fds.

## What ZMQ Eliminates

| Current component | ZMQ replacement |
|-------------------|-----------------|
| WireServer class (select loop, fd tracking) | zmq_poll + ROUTER |
| Wire protocol header (4-byte framing) | ZMQ message framing |
| `register-fd` (external fd in select loop) | zmq_poll mixed items |
| Manual reconnection logic | ZMQ auto-reconnect |
| Broadcast iteration over client list | PUB socket fan-out |

## What ZMQ Does NOT Replace

| Component | Why it stays |
|-----------|-------------|
| bwrite/bread serialization | ZMQ is transport, not serialization |
| WebSocket client (Coinbase) | ZMQ doesn't speak WebSocket |
| JSON parsing (yyjson hot path) | Application logic |
| Query routing logic (Gateway) | Application logic |
| DataTable schema and storage | Application logic |

## Performance Considerations

- **Latency**: ~5-15µs per message on localhost (JSON parse dominates)
- **Throughput**: Millions of msgs/sec on localhost (feed is ~100 msgs/sec)
- **Memory**: ~1MB per socket (1000 × ~1KB messages at default HWM)
- **Thread safety**: ZMQ sockets are NOT thread-safe (fine for single-threaded Lush)
- **Reconnection**: Automatic with configurable interval

## Comparison Summary

| Aspect | Wire + libuv | ZeroMQ |
|--------|-------------|--------|
| Event loop | libuv (FH) + select (others) | zmq_poll (all) |
| Pub/sub | Manual broadcast + fd tracking | PUB/SUB socket |
| Request/reply | on-sync callback | ROUTER/DEALER |
| Reconnection | Manual (WirePool) | Automatic |
| Framing | Custom 4-byte header | ZMQ handles it |
| Topic filtering | None | ZMQ_SUBSCRIBE prefix match |
| Slow subscriber | Blocks publisher | HWM drop policy |
| Compression | LZ4 + byte-transpose | Must apply manually |
| Serialization | bwrite/bread | bwrite/bread (unchanged) |
