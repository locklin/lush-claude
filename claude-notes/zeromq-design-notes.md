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

This document sketches what the pipeline would look like if wire + libuv
were replaced (partially) by ZeroMQ.

---

## ZeroMQ Patterns and Their Pipeline Equivalents

### PUB/SUB — Feed Handler Broadcast

The feed handler broadcasts parsed ticks to all downstream processes.
Today this is WireServer.broadcast() with manual connection tracking.

**ZMQ equivalent:** The feed handler binds a `PUB` socket.  Downstream
processes (RDB, HDB Writer, Analytics) connect with `SUB` sockets and
set topic filters.

```
Feed Handler                   Downstream
───────────                    ──────────
zmq_socket(PUB)                zmq_socket(SUB)
zmq_bind("tcp://*:19960")      zmq_connect("tcp://localhost:19960")
                                zmq_setsockopt(ZMQ_SUBSCRIBE, "ticker")
                                zmq_setsockopt(ZMQ_SUBSCRIBE, "l2")

zmq_send(pub, "ticker", 6,     zmq_recv(sub, ...)
         ZMQ_SNDMORE)           → topic frame: "ticker"
zmq_send(pub, data, len, 0)     → data frame: bwrite-packed alist
```

**Advantages over wire broadcast:**
- No connection tracking needed (PUB socket handles it)
- Topic-based filtering — subscribers only receive what they want
  (RDB subscribes to "ticker" + "l2", Analytics to "ticker" only)
- `ZMQ_SNDHWM` (send high water mark) drops messages to slow subscribers
  rather than blocking the publisher — critical for a hot feed
- Automatic reconnection on subscriber side (ZMQ_RECONNECT_IVL)
- No need for `_add-client`, `_remove-client`, fd tracking

**What we lose:**
- Wire's LZ4 + byte-transpose compression for numeric columns
  (ZMQ doesn't compress; we'd apply compression in the payload before send)
- Subscriber count awareness (PUB is fire-and-forget; use XPUB for
  subscription tracking if needed)

### ROUTER/DEALER — Query Request/Reply

The RDB, HDB Reader, and Analytics processes serve queries from the
Gateway and REPL.  Today this is WireServer with on-sync callback.

**ZMQ equivalent:** Backend processes bind a `ROUTER` socket.  The
Gateway (or REPL) connects with a `DEALER` socket.

```
RDB (query server)              Gateway (client)
──────────────                  ────────────────
zmq_socket(ROUTER)              zmq_socket(DEALER)
zmq_bind("tcp://*:19961")       zmq_connect("tcp://localhost:19961")

zmq_recv(router, ...)           zmq_send(dealer, query, ...)
  → identity frame
  → empty delimiter
  → query frame
process query
zmq_send(router, identity,      zmq_recv(dealer, ...)
         ZMQ_SNDMORE)             → result frame
zmq_send(router, "", 0,
         ZMQ_SNDMORE)
zmq_send(router, result, ...)
```

The `ROUTER` socket automatically tracks client identity, so responses
go back to the right requester — no manual fd-to-client mapping.

### ROUTER (Gateway) → Multiple DEALER (backends) — Smart Routing

The Gateway connects to 4 backends (RDB, HDB, Analytics, HDB Writer)
and routes queries based on content.

**ZMQ equivalent:** Gateway binds a frontend `ROUTER` (for REPL clients)
and connects 4 `DEALER` sockets (one per backend).  Query routing logic
remains in Lush — it inspects the query, picks the right DEALER, sends,
waits for reply, forwards back through the frontend ROUTER.

```
REPL ──DEALER──→ Gateway ──DEALER──→ RDB (ROUTER)
                 (ROUTER   ──DEALER──→ HDB (ROUTER)
                  front)   ──DEALER──→ Analytics (ROUTER)
                           ──DEALER──→ HDB Writer (ROUTER)
```

### zmq_poll — Unified Event Loop

**This is the key win.**  The `register-fd` pattern we added to WireServer
is ZMQ's native operating mode.  `zmq_poll` multiplexes across:

- ZMQ sockets (SUB for feed data, ROUTER for queries)
- Raw fds (libuv's WebSocket fd, stdin, timers via timerfd)

```c
zmq_pollitem_t items[] = {
    { sub_socket,  0, ZMQ_POLLIN, 0 },   // FH broadcast
    { router_socket, 0, ZMQ_POLLIN, 0 },  // client queries
    { NULL, timerfd, ZMQ_POLLIN, 0 },      // periodic flush
};
zmq_poll(items, 3, timeout_ms);
```

For the RDB, this replaces:
- WireServer's `select()` loop
- `register-fd` for the FH connection
- `on-async` / `on-sync` callback dispatch

Instead: one `zmq_poll`, check which items are ready, dispatch accordingly.

---

## Per-Process Socket Layout

### Feed Handler (19960)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| PUB    | bind `tcp://*:19960` | out | Broadcast ticks |
| ROUTER | bind `tcp://*:19966` | in/out | Control queries (status, stats) |

Still needs **libuv** for the Coinbase WebSocket connection (ZMQ does
not speak WebSocket).  The libuv `on_data` callback parses JSON and
publishes via `zmq_send` on the PUB socket.  `zmq_poll` can include
the libuv loop fd for integration.

### RDB (19961)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| SUB    | connect `tcp://localhost:19960` | in | Receive ticks |
| ROUTER | bind `tcp://*:19961` | in/out | Serve queries |

Main loop:
```c
while (running) {
    zmq_poll(items, 2, 100);
    if (items[0].revents & ZMQ_POLLIN)  // SUB ready
        receive_and_insert_tick();
    if (items[1].revents & ZMQ_POLLIN)  // ROUTER ready
        handle_query();
}
```

No libuv needed.  No WireServer needed.  No `register-fd` needed.

### HDB Writer (19965)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| SUB    | connect `tcp://localhost:19960` | in | Receive ticks directly from FH |
| ROUTER | bind `tcp://*:19965` | in/out | Status/control queries |

**Key change:** In the current architecture, HDB Writer connects to
the RDB and pulls data.  With ZMQ PUB/SUB, the HDB Writer subscribes
directly to the feed handler — same pattern as kdb+ TorQ where the WDB
subscribes to the TP, not through the RDB.  This eliminates the
RDB-as-intermediary bottleneck.

### HDB Reader (19962)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| ROUTER | bind `tcp://*:19962` | in/out | Serve historical queries |

No subscriptions needed — HDB Reader only serves queries on disk data.

### Analytics (19963)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| SUB    | connect `tcp://localhost:19960` | in | Receive ticks |
| ROUTER | bind `tcp://*:19963` | in/out | Serve analytics queries |

Currently polls the RDB every second.  With ZMQ SUB, analytics receives
ticks directly from the feed handler in real-time — lower latency,
no polling overhead.

### Gateway (19964)

| Socket | Type | Direction | Purpose |
|--------|------|-----------|---------|
| ROUTER | bind `tcp://*:19964` | in/out | Frontend (REPL clients) |
| DEALER | connect per-backend | out/in | Backend connections (4) |

Query routing logic stays the same; only the transport changes.

---

## What ZMQ Eliminates

| Current component | ZMQ replacement |
|-------------------|-----------------|
| WireServer class (select loop, fd tracking, incremental reads) | zmq_poll + ROUTER socket |
| Wire protocol header (4-byte length framing) | ZMQ message framing (automatic) |
| `_try-read-client` (incremental non-blocking reads) | ZMQ handles internally |
| `_add-client` / `_remove-client` (connection tracking) | ROUTER identity tracking |
| `register-fd` (external fd in select loop) | zmq_poll with mixed socket/fd items |
| Manual reconnection logic | ZMQ_RECONNECT_IVL (automatic) |
| WirePool (client-side reconnection) | ZMQ auto-reconnect on DEALER/SUB |
| libuv for RDB, HDB, Analytics, Gateway | zmq_poll replaces event loop |
| Broadcast iteration over client list | PUB socket (one zmq_send, ZMQ fans out) |

## What ZMQ Does NOT Replace

| Component | Why it stays |
|-----------|-------------|
| bwrite/bread serialization | ZMQ is transport, not serialization.  Payload still needs Lush-native encoding |
| LZ4 + byte-transpose compression | Apply to payload before zmq_send if needed |
| WebSocket client (Coinbase) | ZMQ doesn't speak WebSocket; feed handler still needs libuv for WSS |
| JSON parsing (yyjson hot path) | Application logic, transport-independent |
| Query routing logic (Gateway) | Application logic, just uses different send/recv calls |
| DataTable schema and storage | Application logic |

---

## Data Flow with ZMQ

```
Coinbase WSS ──libuv──→ Feed Handler
                         │
                         │ PUB tcp://*:19960
                         │ (topic: "ticker" or "l2")
              ┌──────────┼──────────┬──────────┐
              │ SUB      │ SUB      │ SUB      │
              ▼          ▼          ▼          │
           RDB        Analytics   HDB Writer   │
           ROUTER     ROUTER      ROUTER       │
           :19961     :19963      :19965       │
              │          │          │          │
              └────┬─────┘          │          │
                   │                ▼          │
              Gateway ◄─── HDB Reader          │
              ROUTER       ROUTER              │
              :19964       :19962              │
                   │                           │
              REPL (DEALER)                    │
```

Key difference from current architecture: **HDB Writer and Analytics
subscribe directly to the Feed Handler**, not through the RDB.  This
is the TorQ WDB pattern (WDB subscribes to TP directly) and eliminates
the RDB as a bottleneck.

---

## Implementation Path

### Option A: Full ZMQ (recommended if building from scratch)

1. **Vendor libzmq** (~50 .c files) or link to system libzmq
2. **Create `packages/zmq/zmq.lsh`** with DHC wrappers:
   - `zmq-ctx-new`, `zmq-ctx-destroy`
   - `zmq-socket`, `zmq-close`, `zmq-bind`, `zmq-connect`
   - `zmq-send`, `zmq-recv`, `zmq-send-more`
   - `zmq-setsockopt` (SUBSCRIBE, SNDHWM, RCVHWM, RECONNECT_IVL, LINGER)
   - `zmq-poll` (array of pollitems, timeout)
3. **Rewrite process entry points** to use ZMQ sockets instead of WireServer
4. **Keep libuv** only in the feed handler (for WebSocket)
5. **Keep bwrite/bread** for payload serialization (wrap in ZMQ message frames)

### Option B: ZMQ-style patterns on existing wire protocol (incremental)

Instead of adding a libzmq dependency, implement ZMQ's key patterns
on top of the existing wire protocol:

1. **Topic-filtered broadcast**: Add a topic field to wire async messages.
   Subscribers register topic filters.  WireServer.broadcast only sends
   to matching subscribers.
2. **High-water mark**: Add a per-client send queue with configurable
   HWM.  Drop messages (or disconnect) when the queue exceeds the limit.
3. **Direct subscription**: Let HDB Writer and Analytics connect
   directly to FH instead of through RDB.

This preserves the existing tested wire code and avoids a new C dependency,
while gaining the architectural benefits.

### Option C: Hybrid (pragmatic)

Use ZMQ for the **pub/sub fan-out** (FH → subscribers) where it adds
the most value.  Keep the existing wire protocol for **query request/reply**
(Gateway ↔ backends) where it works well and provides compression.

---

## ZMQ Message Format

For compatibility with existing bwrite/bread serialization:

```
Frame 0: topic string ("ticker", "l2", "heartbeat")
Frame 1: bwrite-serialized payload (same format as current wire protocol)
```

For queries:
```
Frame 0: bwrite-serialized query expression
Frame 1: (response) bwrite-serialized result
```

This means the serialization layer doesn't change — only the transport.
Migration is mechanical: replace `wire-send`/`wire-recv` calls with
`zmq-send`/`zmq-recv`, replace WireServer event loop with `zmq-poll`.

---

## Performance Considerations

- **Latency**: ZMQ adds ~5-15µs per message on localhost (comparable to
  current wire protocol overhead).  The hot path (JSON parse) dominates.
- **Throughput**: ZMQ PUB/SUB can handle millions of messages/sec on
  localhost.  Our feed is ~100 msgs/sec — not a bottleneck.
- **Memory**: Each ZMQ socket has internal buffers (default HWM = 1000
  messages).  For our message sizes (~1KB), that's ~1MB per socket.
- **Thread safety**: ZMQ sockets are NOT thread-safe (like kdb+ handles).
  Since each Lush process is single-threaded, this is fine.
- **Reconnection**: ZMQ auto-reconnects with configurable interval
  (default 100ms, max 0 = no backoff limit).  Current wire code has
  no auto-reconnect for clients; WirePool adds it but with manual retry.

## Comparison Summary

| Aspect | Wire + libuv (current) | ZeroMQ |
|--------|----------------------|--------|
| Event loop | libuv (FH) + select (others) | zmq_poll (all) |
| Pub/sub | Manual broadcast + fd tracking | PUB/SUB socket |
| Request/reply | on-sync callback | ROUTER/DEALER |
| Reconnection | Manual (WirePool) or libuv | Automatic (built-in) |
| Framing | Custom 4-byte header | ZMQ handles it |
| Topic filtering | None (all subscribers get everything) | ZMQ_SUBSCRIBE prefix match |
| Slow subscriber | Blocks publisher | HWM drop policy |
| Compression | LZ4 + byte-transpose (built-in) | Must apply manually |
| Serialization | bwrite/bread | bwrite/bread (unchanged) |
| Dependencies | libuv (vendored) | libzmq (~50 .c or system lib) |
| Maturity | Custom, ~1000 lines Lush + C | 15+ years, battle-tested |
| Lines of code | WireServer ~400 LOC | ZMQ bindings ~150 LOC |

---

## Recommendation

The current wire + libuv + `register-fd` approach works and all tests pass.
A full ZMQ migration is not urgent.  However, if the system grows to:

- Multiple exchanges (Binance, Kraken, etc.)
- More downstream consumers (logging, alerting, ML inference)
- Higher message rates
- Distributed deployment (processes on different machines)

Then ZMQ becomes compelling because it handles fan-out, reconnection,
and multi-host routing with less custom code.

**If starting fresh**, Option A (full ZMQ) would be cleaner.
**For incremental improvement**, Option B (ZMQ patterns on wire) gives
80% of the benefit with minimal disruption.  The most impactful single
change would be having HDB Writer and Analytics subscribe directly to
the Feed Handler (eliminating the RDB intermediary), which can be done
with or without ZMQ.
