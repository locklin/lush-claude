# Async Communication: libuv vs pthreads vs multi-process

## Core constraint

Lush's interpreter (GC, symbol table, evaluation) is **not thread-safe**.
Any background thread must do pure C work only -- no Lush calls.

## The feed handler problem

1. Maintain persistent WebSocket connections
2. Receive messages without blocking the REPL
3. Parse and store data (JSON -> DataTable pipeline)
4. Let the user query data while the feed runs

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

**DHC bridge sketch:**

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

## Approach 2: libuv

### 2A. libuv in a background thread

libuv thread                    Main Lush thread
------------                    ----------------
uv_run(loop, UV_RUN_DEFAULT)    REPL / user code
  uv_tcp/uv_udp callbacks       poll shared queue
  uv_timer for reconnect         drain into DataTable
  uv_async for shutdown
  parse + enqueue ------------>

- libuv handles reconnection, backoff timers, multiple connections, DNS -- all in one loop
- `uv_async_send()` for thread-safe wakeup from Lush (subscribe/unsubscribe)
- Same constraint: no Lush calls from callbacks

### 2B. libuv polled from Lush (single-threaded)

```lisp
(de feed-tick ()
    ;; DHC: calls uv_run(loop, UV_RUN_NOWAIT)
    ;; processes ready I/O, fires callbacks
    ;; callbacks parse JSON, append to column arrays
    ...)
```

- No mutex, no races -- everything single-threaded
- But Lush blocks during computation, messages queue in kernel socket buffer
- Works if (feed-tick) called frequently or hooked into Lush idle

**Pros of libuv generally:**
- Battle-tested (Node.js, neovim, Julia)
- Cross-platform (epoll on Linux, kqueue on macOS)
- Built-in reconnect/timer primitives -- useful for feed that survives disconnects
- Thread pool for blocking work (uv_queue_work)
- Single vendored dep (~20 .c files)

**Cons:**
- Heavier dep than raw pthreads (still smaller than libcurl)
- Event loop model adds conceptual complexity vs simple blocking thread
- Background thread: same "no Lush from callbacks" constraint
- Single-threaded poll: REPL blocks unless disciplined about yielding

---

## Approach 3: Multi-process with wire protocol

Feed process (Lush)             Main process (Lush)
-------------------             -------------------
ws-connect, ws-recv             wire-listen on unix socket
json-parse-l2update             wire-recv -> DataTable
wire-send batched rows -------> query, aggregate, display

**Pros:**
- No thread safety concerns -- each process has own interpreter
- Crash isolation: feed dying doesn't kill analysis session
- Multiple feed processes (per exchange, per product group)
- Wire protocol already handles serialization
- Could add a (lush-fork) primitive

**Cons:**
- IPC overhead: data crosses Unix socket (memcpy + syscall per batch)
- More moving parts: manage child processes, stderr, restart on crash
- Can't share DataTable memory directly (could add mmap-backed columns later)

---

## Assessment

**For the feed handler:** pthreads + ring buffer is the best fit.

1. Hot path (recv -> yyjson parse -> extract columns) is already pure C.
   `lush_json_parse_l2update()` writes directly into column arrays with
   no Lush interaction. Worker thread calling that is straightforward.
2. Handoff surface is tiny: ring buffer of "N new rows in these columns."
3. libuv's value-add (multiplexing, timers, DNS) matters less with 1-3
   WebSocket connections that libcurl already manages. Adding libuv on
   top of libcurl is awkward.
4. Multi-process is safest but adds operational complexity.

**For async wire protocol:** libuv or raw epoll makes more sense than
pthreads, because the problem is I/O multiplexing (watching multiple
file descriptors) not background computation.

**Pragmatic path:**

1. Start with **pthreads** for feed handler -- single C file: worker
   thread + SPSC ring buffer, DHC wrappers for feed-start/stop/poll
2. Add **non-blocking wire I/O** later using raw epoll_wait wrapped
   in DHC (simpler than vendoring libuv, Linux-only anyway)
3. If need grows to many connections with reconnect/timer logic,
   **then** vendor libuv and port the epoll code to it
