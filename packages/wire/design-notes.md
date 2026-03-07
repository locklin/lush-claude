# Wire Package: Design Notes

IPC package for Lush, enabling multi-process communication over TCP sockets.
Inspired by Q/kdb+ IPC, TorQ's architecture, and Kerf1's wire.c.

---

## Architecture

### Goals

- **Lush-to-Lush IPC**: Server exposes tables and evaluator to clients via
  s-expression evaluation. Client sends an s-expression, server evaluates it,
  result comes back.
- **Cross-language access**: R, Python, or any language with a TCP socket can
  query a Lush server using the text (sexp) encoding.
- **Foundation for TorQ-like architecture**: Gateway agents, real-time
  databases, tickerplants all need IPC.

### Not implemented (potential future work)

- Authentication/login (challenge-response like Q's `.z.pw`)
- TLS/encryption
- Multi-table joins across processes (gateway scatter-gather)
- Pub/sub / async event streaming (tickerplant pattern)
- Discovery service, handler chain, process types

### Why a separate package?

- httpd is for serving web UIs and REST APIs (HTTP is stateless, text-heavy).
- RemoteLush is single-client, single-server with no protocol headers,
  no custom handlers, no cross-language support.
- Wire provides persistent connections, binary data transfer, bidirectional
  communication, and minimal framing overhead (16 bytes vs HTTP headers).

Wire reuses RemoteLush's `bwrite`/`bread` pattern for binary encoding but
adds multiplexed connections, protocol headers, and custom handler hooks.

### File Layout

| File | Description |
|------|-------------|
| `wire.lsh` (~1400 lines) | Main module: inline C helpers, protocol, server, client, WirePool |
| `wire-serialize.lsh` (~280 lines) | DataTable pack/unpack (binary + sexp + compressed) |
| `wire_helpers.c` (~248 lines) | DX functions: memstream, recv-nonblock, time-seconds |
| `wire_helpers.h` | Header for wire_helpers.c |
| `tests/run-all.lsh` (~724 lines) | Test suite stages 1-22, loads run-phase2.lsh |
| `tests/run-phase2.lsh` (~485 lines) | Test suite stages 23-35 |

Note: LZ4 source (`lz4.c`/`lz4.h`, BSD-2-Clause, Yann Collet) is bundled for
compression support.

---

## API Summary

### Wire Protocol (16-byte header)

```
Offset  Size  Field         Values
------  ----  -----         ------
0       4     Magic         "LWIR" (0x4C574952)
4       1     Version       1
5       1     MsgType       0=async, 1=sync-request, 2=response, 3=error, 4=heartbeat
6       1     Encoding      0=sexp (text), 1=bwrite (binary), 2=compressed
7       1     Reserved      0
8       4     MsgID         uint32 little-endian (correlation ID; currently always 0)
12      4     PayloadLen    uint32 little-endian
```

MsgID is reserved for future pipelining/out-of-order responses; Phase 1 uses 0.

**Message types:**

| Type | Name | Direction | Semantics |
|------|------|-----------|-----------|
| 0 | async | client->server | Fire-and-forget; server evaluates, discards result |
| 1 | sync | client->server | Server evaluates, sends response (type 2) back |
| 2 | response | server->client | Result of a sync request (carries MsgID of request) |
| 3 | error | server->client | Error during evaluation; payload is error message |
| 4 | heartbeat | either | Keepalive ping |

**Encodings:**

| Code | Name | Use case |
|------|------|----------|
| 0 | sexp | Cross-language: any client sends `"(+ 1 2)"` as UTF-8 text |
| 1 | bwrite | Lush-to-Lush: preserves all types via `bwrite`/`bread` |
| 2 | compressed | LZ4-compressed binary with byte-transpose preprocessing |

### Server

```lisp
;; Create server (reads --port/-p from argv, or LUSH_WIRE_PORT env, or default 5555)
(setq server (wire-server-new))

;; Or with explicit port
(setq server (wire-server-new 5555))

;; Customize handlers (Q-style .z.* hooks)
(setq :server:on-sync   (lambda (client-id expr) ...))  ;; sync query handler
(setq :server:on-async  (lambda (client-id expr) ...))  ;; async message handler
(setq :server:on-connect    (lambda (client-id) ...))
(setq :server:on-disconnect (lambda (client-id) ...))
(setq :server:on-error      (lambda (client-id err) ...))

;; Start blocking event loop
(==> server serve)

;; Or single iteration (for testing)
(==> server serve-once)

;; Broadcast async message to all connected clients
(==> server broadcast payload)

;; Convenience: create + serve in one call
(wire-server-start)
(wire-server-start 5555)
```

**WireServer slots:** `port`, `listen-sock`, `clients` (htable: fd-key ->
client-entry), `running`, `handlers`, `on-connect`, `on-disconnect`, `on-sync`,
`on-async`, `on-error`, `next-client-id`, `localhost-only`,
`idle-timeout-secs`, `keepalive-interval-secs`.

**Default handlers:** evaluate s-expression in server environment.

### Client

```lisp
(libload "wire/wire")
(setq conn (wire-connect "localhost" 5555))
(wire-call conn "(+ 1 2)")                      ;; => 3 (sync RPC)
(wire-call conn "(==> db tables)")               ;; => ("trades" "quotes")
(wire-send conn "(setq x 42)")                   ;; async fire-and-forget
(wire-close conn)
```

`wire-call` auto-selects encoding: strings use sexp, objects use binary.

### Connection Pool

```lisp
(setq pool (new WirePool))
(==> pool add "rdb" "localhost" 5556)
(==> pool add "hdb" "localhost" 5557)
(==> pool get "rdb")            ;; returns connection (auto-reconnects on failure)
(==> pool call "rdb" "(+ 1 2)") ;; RPC through pool
(==> pool send "rdb" "(setq x 42)")
(==> pool close-all)
```

### DataTable Serialization

```lisp
;; Binary (Lush-to-Lush)
(wire-pack-datatable dt)         ;; => list suitable for bwrite
(wire-unpack-datatable packed)   ;; => DataTable

;; Compressed (LZ4 + byte-transpose)
(wire-pack-datatable-compressed dt)
(wire-unpack-datatable-compressed packed)

;; S-expression (cross-language)
(wire-datatable-to-sexp dt)      ;; => string
```

Handles all column types: real, int, float, stamp, string.

### argv Parsing

```lisp
(wire-parse-port)  ;; scans argv for --port/-p, falls back to LUSH_WIRE_PORT env
```

Works with dynamic scoping: `argv` from `startup` is visible in loaded scripts,
so a gateway-agent wrapper script's `--port` flag propagates correctly.

### Cross-Language Examples

**Python:**
```python
import socket, struct
sock = socket.create_connection(("localhost", 5555))
payload = b'(+ 1 2)'
header = b'LWIR' + bytes([1, 1, 0, 0]) + struct.pack('<II', 0, len(payload))
sock.sendall(header + payload)
resp_hdr = sock.recv(16)
resp_len = struct.unpack('<I', resp_hdr[12:16])[0]
resp = sock.recv(resp_len)  # => b'3'
```

**R:**
```r
con <- socketConnection("localhost", 5555, blocking=TRUE, open="r+b")
# Build 16-byte header + UTF-8 payload, send, read response header + payload
```

---

## Implementation Details

### Phase 1 (Core IPC) -- Done

C socket helpers (bind, accept, select, read/write exact, header parse/build),
WireServer class with `select()`-based event loop, client API, sexp and bwrite
encoding, DataTable serialization, argv parsing, default eval handlers, custom
handler hooks. 122 tests across 22 stages.

### Phase 2 (Robustness and Performance) -- Done

Memstream serialization (eliminated temp files), non-blocking incremental reads,
connection timeout/keepalive, LZ4 compression with byte-transpose, broadcast,
WirePool with auto-reconnect. 193 tests across 35 stages.

### Key deviations from the original plan

1. **No separate wire-c.c**: All C socket functions are inline in `wire.lsh`
   via `dhc-make` with `#{ ... #}` blocks, following the `columnardb-io.lsh`
   pattern. Avoids a separate compilation step.

2. **Raw file descriptors throughout**: The plan suggested wrapping C fds as
   Lush FILE* handles for `socketselect` compatibility. Instead, all socket I/O
   uses compiled C functions on raw fds. `_wire-select` calls `select()`
   directly on raw fds, avoiding the complexity of bridging C fds and Lush file
   handles.

3. **bwrite/bread via memstream (Phase 2)**: Phase 1 used temp files as
   intermediary because `bwrite`/`bread` require Lush FILE* handles. Phase 2
   replaced this with DX-registered `open_memstream`/`fmemopen` wrappers in
   `wire_helpers.c` that create proper Lush file objects for in-memory I/O.

4. **DX functions for memstream**: Used `init_wire_helpers()` DX registration
   (not dhc) because memstream requires `new_extern(&file_W_class, f)` which
   is only available from C-level DX code.

5. **Separate dhc-make name for LZ4**: The LZ4 `dhc-make-with-libs` block uses
   a unique name (`"wire_lz4"`) to avoid overwriting the first dhc-make output
   (`"wire"`).

6. **Combined header+payload send**: Non-blocking reads exposed a TCP
   fragmentation issue -- separate `send()` calls for header and payload could
   leave the payload undelivered when the receiver does a non-blocking `recv()`
   immediately after reading the header. Fixed by combining into a single
   `send()` call.

7. **Test file split**: Phase 2 tests (stages 23-35) are in a separate file
   (`run-phase2.lsh`) loaded from `run-all.lsh` to avoid Lush reader issues
   with large single files.

8. **Localhost-only binding**: Implemented Option A from the plan --
   `_wire-bind-localhost` binds to `INADDR_LOOPBACK` at the C level so the
   kernel rejects non-local connections at the TCP level (not post-accept).

### Lush quirks encountered

- `cadddr`/`cddddr` not built-in: defined at the top of wire.lsh.
- No early `return` from functions: used nested `if` instead.
- `ubyte-matrix 0` invalid: all buffer allocations use `(max 1 n)`.
- No `each-htable`: used `(htable-keys ht)` to iterate htable entries.
- No `boundp`: just check `(consp argv)` directly (dynamic scoping guarantees
  `argv` exists from `startup`).

### Comparison with prior art

| Aspect | Kerf1 | Q/kdb+ | Lush Wire |
|--------|-------|--------|-----------|
| Header | 16 bytes | 8 bytes | 16 bytes (magic + encoding + MsgID) |
| Payload sizing | Power-of-2 (wastes up to 50%) | Exact | Exact |
| Serialization | Custom K0 tree | Custom | bwrite (native) + sexp (text) |
| Event loop | `select()` in C | Internal | `select()` via compiled C |
| Compression | WKDM/gzip/LZ4 | None | LZ4 + byte-transpose |
| Fork-on-connect | Supported | No | No (single-threaded multiplexing) |
| Connection pool | Fixed array[FD_SETSIZE] | Internal | Dynamic htable |

---

## Known Issues / Limitations

- **MsgID unused**: Always 0. Pipelining and out-of-order responses are not
  implemented; the protocol field exists for forward compatibility.

- **No authentication**: Localhost-only binding is the only access control.
  No handshake, no credentials, no per-connection user tracking.

- **No TLS**: All communication is plaintext.

- **Single-threaded**: Server processes one message at a time within the event
  loop. Long-running eval blocks all other clients.

- **Payload size limit**: `PayloadLen` is uint32 (max ~4 GB per message).

- **No pub/sub**: Broadcast is server-push-to-all-clients; there is no
  topic-based subscription mechanism.

- **No gateway scatter-gather**: The hooks are in place (`on-sync` callback)
  to build a gateway pattern, but no built-in scatter-gather implementation.

- **Memstream DX functions**: The `wire_helpers.c` DX functions use
  `open_memstream`/`fmemopen` which may not be available on all platforms
  (POSIX 2008). Linux is fine; macOS may need a fallback.

### Test results

```
Wire:       193 passed, 0 failed (35 stages)
ColumnarDB: 603 passed, 0 failed
DataTable:  868 passed, 0 failed
```
