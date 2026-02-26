# Async Communication: libuv vs pthreads vs multi-process

## Core constraint

Lush's interpreter (GC, symbol table, evaluation) is **not thread-safe**.
Any background thread must do pure C work only -- no Lush calls.

## The feed handler problem

1. Maintain persistent WebSocket connections to Coinbase (and later other exchanges)
2. Receive messages without blocking
3. Parse and store data (JSON -> column extraction pipeline)
4. **Publish** parsed data to downstream processes over IPC
5. Handle connection glitches: drops, reconnects, backoff, sequence gaps
6. Log feed health events (drops, latency spikes, reconnections)

## The bigger picture: a TorQ-like process architecture

The feed handler is not a standalone component.  It is one process in
a multi-process system inspired by Q/kdb+ TorQ:

```
                          ┌──────────────┐
                          │  Coinbase WS  │   (and eventually other exchanges)
                          └──────┬───────┘
                                 │ WebSocket
                          ┌──────▼───────┐
                          │ Feed Handler  │   libuv event loop
                          │  (Lush proc)  │   ws-recv, parse, publish
                          └──────┬───────┘
                                 │ wire async broadcast
               ┌─────────┬──────┼──────┬──────────┐
               ▼          ▼      ▼      ▼          ▼
         ┌──────────┐ ┌──────┐ ┌────┐ ┌──────┐ ┌────────┐
         │ Columnar  │ │ Log  │ │ RDB│ │ Pred │ │ Viz    │
         │ DB Writer │ │ Proc │ │    │ │ Model│ │ Server │
         │ (HDB)     │ │      │ │    │ │      │ │        │
         └──────────┘ └──────┘ └────┘ └──────┘ └────────┘
```

### TorQ analogy

| TorQ concept       | Lush equivalent                                  |
|--------------------|--------------------------------------------------|
| Feed handler       | Lush process: WS recv + JSON parse + publish     |
| Tickerplant        | Feed handler itself (no separate TP needed yet)  |
| RDB                | Lush process: accumulates today's DataTables      |
| HDB / WDB          | Lush process: writes ColumnarDB splayed to disk  |
| Gateway            | Future: routes queries across RDB + HDB          |
| Discovery service  | Future: process.csv or env-var based              |
| Log process        | Lush process: records feed health, drop events   |

In Q/kdb+ TorQ, the **feed handler** connects to the exchange and
pushes rows to the **tickerplant** via `.u.upd` (an async IPC call).
The tickerplant timestamps, logs, and broadcasts to all subscribers.

For us, the feed handler can **be** the tickerplant initially -- it
parses the data and broadcasts packed DataTable batches directly to
all connected downstream processes via `wire-send` / `broadcast`.
This avoids a hop.  If scale demands it later, a separate tickerplant
process can be interposed.

### Key insight: the feed handler is a process, not a thread

The feed handler **must** be an independent process because:

1. **Crash isolation** -- a feed handler bug or Coinbase protocol
   change should not kill an analysis session or a running model.
2. **Multiple consumers** -- the same feed data fans out to the DB
   writer, the log process, the prediction model, the visualizer.
   Each is a separate Lush process connected via wire protocol.
3. **Independent lifecycle** -- the feed handler can be restarted,
   upgraded, or pointed at a different exchange without touching
   the downstream processes.
4. **Operational clarity** -- each process has its own stderr, can be
   monitored independently, and maps to one clear responsibility.

This rules out the pthreads approach as the primary architecture.
The pthreads model solves "non-blocking recv in a single process"
but does not solve "publish to N consumers" or "crash isolation."

---

## Approach 1: pthreads + ring buffer

Worker thread (C only)          Main Lush thread
---------------------          -----------------
recv() from socket              REPL / user code
yyjson parse                    periodically poll queue
write into ring buffer -------> drain ring buffer
  (mutex or lock-free)          copy into DataTable

**Pros:**
- Lowest latency -- in-process, shared memory
- Worker does all C-level work (recv, parse, extract columns) without touching interpreter
- Already have the infrastructure: yyjson pool parse, column extractors, all zero-alloc
- A single `pthread_mutex` + `pthread_cond` suffices for producer-consumer queue

**Cons:**
- Worker must NEVER call into Lush -- no error(), no printf through Lush, no Lush heap alloc
- Error reporting limited to status codes in shared memory
- Segfault in worker takes down whole process
- Must write careful C: ring buffer, overflow policy, shutdown coordination
- **Does not solve the multi-process publish problem** -- all consumers
  would need to live in the same process, which defeats isolation
- The interpreter-lock concern doesn't even arise if the feed handler
  is a separate process (it owns its own interpreter)

**Where pthreads still fits:**
Within a single feed handler process, pthreads could offload the raw
`recv()` loop to a background thread while the main thread runs the
libuv event loop or the wire server.  But this is a micro-optimization
inside one process, not the architecture.  The WebSocket recv + parse
is fast enough to run in the libuv event loop directly (yyjson parses
at 1.7+ GB/s; a Coinbase l2update is ~1KB; we have ~100 msgs/sec at
peak -- that's ~100us of parse work per second).

**DHC bridge sketch (retained for reference):**

```c
// C side: lock-free SPSC ring buffer
typedef struct {
    char buf[RING_SIZE];
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    // pre-parsed column data at each slot
} feed_ring_t;

// Worker thread: pure C, never touches Lush
void *feed_worker(void *arg) {
    while (running) {
        n = recv(fd, msg, ...);
        // parse with yyjson (stack pool, zero-alloc)
        // write parsed columns into ring slot
        atomic_store(&ring->write_pos, ...);
    }
}
```

```lisp
;; Lush side: poll from interpreted code
(de feed-poll (ring side-codes prices quantities timestamps n-max)
    ;; DHC-compiled: reads from ring, writes into column arrays
    ;; returns number of rows drained
    ...)
```

---

## Approach 2: libuv event loop (recommended for feed handler)

### Architecture: libuv-driven feed handler process

```
┌─────────────────────────────────────────────────────────┐
│  Feed Handler Process (single-threaded, libuv loop)     │
│                                                         │
│  ┌─────────────┐   ┌──────────────┐   ┌─────────────┐  │
│  │ uv_tcp /    │   │ uv_timer     │   │ uv_tcp      │  │
│  │ uv_stream   │   │ reconnect    │   │ wire server  │  │
│  │ (WebSocket) │   │ backoff      │   │ (broadcast)  │  │
│  │             │   │ health check │   │              │  │
│  └──────┬──────┘   └──────┬───────┘   └──────┬──────┘  │
│         │                 │                   │         │
│         ▼                 ▼                   ▼         │
│  ┌──────────────────────────────────────────────────┐   │
│  │              uv_run(loop, UV_RUN_DEFAULT)         │   │
│  │                                                   │   │
│  │  on_ws_data:                                      │   │
│  │    yyjson parse (stack pool, zero-alloc)          │   │
│  │    extract columns via lush_json_parse_l2update() │   │
│  │    batch into wire-packed DataTable               │   │
│  │    broadcast to all connected wire clients        │   │
│  │                                                   │   │
│  │  on_timer:                                        │   │
│  │    check connection health, attempt reconnect     │   │
│  │    log feed stats (msg count, latency, gaps)      │   │
│  │                                                   │   │
│  │  on_wire_connect:                                 │   │
│  │    accept downstream process connection           │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

The feed handler is a **dedicated Lush process** whose main loop is
`uv_run()`.  There is no REPL -- it runs headless.  All I/O (WebSocket
recv, wire protocol send, timers) is managed by a single libuv event loop.

### Why libuv is the right tool here

1. **Connection lifecycle management.**  Coinbase WebSocket connections
   drop.  Handling reconnection with exponential backoff, DNS re-resolution,
   and health-check timers is exactly what libuv's timer + TCP primitives
   are built for.  With raw pthreads you'd hand-roll all of this; with
   libuv it's `uv_timer_start(&reconnect_timer, on_reconnect, delay, 0)`.

2. **Unified event loop for inbound and outbound I/O.**  The feed handler
   needs to both **receive** from Coinbase and **send** to downstream
   wire clients.  libuv's loop multiplexes both directions.  The wire
   server's `select()` loop in wire.lsh would be replaced by (or
   integrated with) `uv_poll` or `uv_tcp` handles for the wire listener
   and client fds.

3. **Multiple exchange connections.**  When the system grows to handle
   Binance, Kraken, etc., each exchange is just another `uv_tcp` handle
   in the same event loop.  No additional threads, no fd-set management.

4. **No interpreter-lock concern.**  Since the feed handler is its own
   process, it owns its Lush interpreter exclusively.  There is no
   "background thread must not call Lush" constraint.  The libuv
   callbacks can freely call Lush functions (json-parse, wire-pack,
   wire-send) because the interpreter is idle during `uv_run()`.

   This is the key realization: the reason pthreads seemed attractive
   was the single-process model where the interpreter needed to remain
   responsive for a REPL.  In a dedicated feed handler process, there
   is no REPL.  The process exists to run the event loop.

5. **Battle-tested.**  Node.js, neovim, Julia, and many production
   systems rely on libuv for exactly this kind of I/O multiplexing
   with timers.  ~20 .c files to vendor.

### libcurl coexistence

The existing curl package uses libcurl for WebSocket connections.
There are two options:

**Option A: libuv owns the socket, replace libcurl for WS.**
Write a thin WebSocket client on top of libuv's `uv_tcp_t`.  This
means implementing the WS handshake and frame protocol (~200 lines of
C) but gives libuv full control of the socket lifecycle.  Clean, no
impedance mismatch.

**Option B: libcurl multi-interface + libuv.**
Use `curl_multi_socket_action()` which is designed to integrate with
external event loops.  libcurl tells us which fds to watch; we register
them as `uv_poll_t` handles.  This is the documented way to use libcurl
with libuv.  More complex but preserves libcurl's HTTP upgrade, TLS,
and WS frame handling.

**Recommendation:** Start with Option A.  The Coinbase WS protocol is
well-defined.  A minimal WS client on top of libuv avoids the libcurl
multi-interface complexity.  If we later need libcurl features (proxy
support, complex auth), we can switch to Option B.

### Feed handler event loop sketch

```c
// C level: libuv callbacks (compiled via DHC or as a .c file)

void on_ws_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread < 0) {
        // Connection lost -- schedule reconnect timer
        uv_timer_start(&reconnect_timer, on_reconnect, backoff_ms, 0);
        return;
    }
    // Accumulate WebSocket frames, handle fragmentation
    // When a complete message arrives:
    //   yyjson_doc *doc = yyjson_read_opts(msg, len, 0, &pool_alc, NULL);
    //   int nrows = lush_json_parse_l2update(doc, sides, prices, qtys, ...);
    //   if (nrows > 0) enqueue_batch(nrows);
}

void on_reconnect(uv_timer_t *timer) {
    // Attempt reconnection
    // On success: resubscribe to channels
    // On failure: double backoff, reschedule timer
}

void on_batch_ready(uv_async_t *async) {
    // Drain parsed batches
    // wire-pack into DataTable
    // broadcast to all connected wire clients
}
```

```lisp
;; Lush level: feed handler main

(libload "wire/wire")
(libload "json/json")
(libload "feed/feed-uv")  ;; libuv bindings + feed logic

(de feed-handler-main ()
    ;; Parse command-line args: --exchanges, --products, --port
    (let ((port (or (wire-parse-port) 6000))
          (products '("BTC-USD" "ETH-USD")) )

      ;; Initialize libuv loop + WebSocket connection + wire server
      (feed-init port products)

      ;; Run the event loop (blocks forever, or until signal)
      (feed-run)

      ;; Cleanup
      (feed-shutdown) ) )
```

### Glitch handling with libuv

Connection glitches are first-class concerns.  libuv makes each of
these straightforward:

| Glitch                    | libuv mechanism              |
|---------------------------|------------------------------|
| WebSocket disconnect      | `on_read` returns `UV_EOF`; start reconnect timer |
| Reconnect backoff         | `uv_timer_start` with exponential delay    |
| Heartbeat / keepalive     | `uv_timer_start` periodic; detect stale conn |
| Sequence gap detection    | Track sequence numbers in callback; log gaps |
| DNS change after failover | libuv `uv_getaddrinfo` async DNS resolution  |
| Graceful shutdown         | `uv_signal_t` for SIGTERM; drain + close     |
| Health logging            | `uv_timer_t` periodic: emit stats to log proc |

Each of these would be hand-rolled boilerplate with raw pthreads
or raw `select()`.

---

## Approach 3: Multi-process with wire protocol (the deployment model)

This is not an alternative to libuv -- it is the **deployment model**
that libuv enables.  The feed handler is one process; everything else
is another process.

Feed Handler (Lush)             Downstream Processes (Lush)
-------------------             ---------------------------
libuv event loop                wire-connect to feed handler
ws-recv, json-parse             on-async callback receives batches
wire broadcast ──────────────>  wire-unpack-datatable -> local DataTable
                                process, store, visualize, trade

### What the wire protocol already provides

Looking at the existing wire package:

- **WireServer** with `select()`-based event loop, non-blocking
  incremental reads, heartbeat, idle timeout, per-client state
- **`broadcast`** method: serialize once, send async to all clients
- **`wire-pack-datatable` / `wire-unpack-datatable`**: column-oriented
  DataTable serialization via bwrite/bread
- **`wire-pack-datatable-compressed`**: LZ4 + byte-transpose for
  efficient numeric column transfer
- **WirePool** with auto-reconnect for downstream clients

This infrastructure is already well-suited for the TorQ-like pub/sub
pattern.  The feed handler calls `(==> server broadcast packed-batch)`
and every connected downstream process receives it.

### What needs to change for libuv integration

The WireServer currently uses its own `select()` loop in `serve()`.
For the feed handler, the wire server's I/O needs to be driven by the
libuv event loop instead.  Two options:

**Option A: Thin libuv wrapper around wire protocol.**
Register the wire listen-fd and client-fds as `uv_poll_t` handles in
the libuv loop.  When libuv signals readability, call the existing
`_try-read-client` / `_handle-message` logic.  The wire protocol bytes
and framing stay the same; only the I/O multiplexing changes from
`select()` to libuv.

**Option B: Full libuv TCP for wire.**
Use `uv_tcp_t` + `uv_listen` + `uv_accept` for the wire server.
Rewrite the header/payload state machine using `uv_read_start` callbacks.
Cleaner integration but more rewrite.

**Recommendation:** Option A -- it reuses the proven wire protocol code
and the `_try-read-client` state machine.  The main change is replacing
the `while running ... _wire-select ...` loop with `uv_poll_t` handles
in the libuv loop.

### Publishing model: push, not subscribe

In Q/kdb+ TorQ, the tickerplant pushes data to subscribers.
Subscribers register via `.u.sub` and the tickerplant pushes via `.u.upd`.

For the initial Lush feed handler, we can simplify further:

**Broadcast to all connected clients.**  Any process that connects to
the feed handler's wire port automatically receives all data.  No
subscription negotiation needed.  The feed handler doesn't need to
know what its consumers do with the data.

This matches the "feed handler just publishes" design you described.
If we later need per-table or per-product subscriptions, that's an
enhancement to the wire protocol (add a subscribe message type), not
a change to the overall architecture.

### IPC cost discussion

The wire protocol adds serialization + socket overhead vs. shared memory:

- `wire-pack-datatable`: bwrite serializes column idx1 arrays.  For
  a batch of 100 l2update rows (5 columns: int + 2*double + long + int),
  that's ~3.2KB of raw column data.
- Socket write + read: one `write()` syscall for header+payload,
  one `read()` on the receiver.  Localhost TCP loopback is ~1us.
- Total per-batch overhead: ~5-10us for serialize + syscall + deserialize.
  At 10 batches/sec that's 50-100us/sec -- negligible.

The hot path (yyjson parse + column extraction) is already ~1us per
message.  The wire overhead is comparable.  Zero-copy optimizations
(mmap-backed columns, Unix domain sockets with `sendmsg`/`SCM_RIGHTS`)
are available later if profiling shows the need, but this is premature
optimization territory.

---

## Revised assessment

**For the feed handler:** libuv in a dedicated process is the right choice.

1. The feed handler is an **independent process**, not a background
   thread.  This eliminates the interpreter-lock concern that motivated
   pthreads.  The process owns its interpreter; the libuv event loop
   and Lush callbacks cooperate naturally.

2. **Connection lifecycle** (reconnect, backoff, health checks, multi-
   exchange) is the hard problem, not raw recv speed.  libuv provides
   timers, async DNS, signal handling -- exactly the primitives needed
   for robust feed handling.  With pthreads you'd reimplement these.

3. The **publish side** (wire broadcast to downstream processes) is
   already implemented.  libuv integrates with it by driving the wire
   server's fds from the same event loop.

4. **pthreads is not wasted if we start with libuv**, because there's
   nothing to throw away.  Conversely, starting with pthreads and then
   discovering we need reconnect timers, multi-exchange multiplexing,
   and multi-process IPC would mean building most of what libuv
   provides for free.

5. The parse hot path (`lush_json_parse_l2update`, yyjson, column
   extraction) is fast enough to run synchronously in the libuv
   callback.  ~100us/sec of parse work doesn't need a separate thread.

**Pragmatic path (revised):**

1. **Vendor libuv** (~20 .c files) into `packages/libuv/`.  Build via
   lushmake.  Expose core handles: `uv_loop`, `uv_tcp`, `uv_timer`,
   `uv_poll`, `uv_signal`, `uv_async`.

2. **Build feed handler process** using libuv:
   - WebSocket client (thin, on `uv_tcp_t`, or via libcurl multi)
   - JSON parse + column extraction (existing `json-c.c` functions)
   - Wire server integrated into libuv loop (Option A: `uv_poll` on wire fds)
   - Reconnect timers, health logging, graceful shutdown

3. **Build downstream process templates:**
   - DB writer: connects to feed handler, receives batches, appends to
     ColumnarDB splayed tables.
   - Log process: connects to feed handler, records feed health events.
   - Interactive process: connects to feed handler, accumulates DataTable
     for REPL queries (this one uses the REPL).

4. **Process management:** Initially just shell scripts.  Later,
   a process.csv + launcher (TorQ-style) if the process count grows.

---

## Open questions

- **WebSocket client strategy:** Write a minimal WS client on libuv,
  or use libcurl's multi-interface?  Minimal is cleaner but means
  implementing WS frame parsing + TLS (via OpenSSL, which libcurl
  already links).

- **Wire protocol on libuv:** Option A (uv_poll on existing wire fds)
  or Option B (full uv_tcp rewrite)?  Option A is less work and
  reuses the tested state machine.

- **Batching strategy:** How many messages to accumulate before
  broadcasting a batch?  Time-based (every 100ms), count-based
  (every 50 rows), or size-based (every 4KB)?  TorQ's segmented
  tickerplant supports all three.

- **Log format for feed health:** Append to a DataTable (queryable)
  or a plain text log?  DataTable is more useful but heavier.
