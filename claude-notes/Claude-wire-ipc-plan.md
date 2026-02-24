# Wire Package: Interprocess Communication for Lush

Design plan for `packages/wire/`, an IPC package enabling Lush instances to
communicate over TCP sockets — query tables, execute expressions, and exchange
data.  Inspired by Q/kdb+ IPC, TorQ's architecture, and Kerf1's wire.c.

---

## 1. Motivation and Goals

The columnardb + DataTable packages provide single-process columnar storage.
The next step is multi-process:

- **Lush-to-Lush IPC**: A Lush server exposes its tables and evaluator to
  Lush clients.  A client sends an s-expression, the server evaluates it,
  the result comes back.
- **Cross-language access**: R, Python, or any language that can open a TCP
  socket and exchange simple framed messages can query a Lush server.
- **Foundation for TorQ-like architecture**: Gateway agents, real-time
  databases, tickerplants — all need IPC.

### Non-goals for Phase 1

- Authentication/login (future: challenge-response like Q's `.z.pw`)
- TLS/encryption (future)
- Compression (future: LZ4 or zstd, like Kerf's wire.c)
- Multi-table joins across processes (future: gateway scatter-gather)
- Pub/sub or async event streaming (future: tickerplant pattern)

---

## 2. Prior Art Analysis

### 2.1 Q/kdb+ IPC

Q's IPC is the gold standard for data-oriented IPC:

- **Connection**: `hopen `:host:port:user:pass`` returns an integer handle (fd).
- **Wire format**: 8-byte header (endianness, msg-type, reserved, length) +
  self-describing serialized payload.
- **Three message types**: async (0), sync-request (1), response (2).
- **Execution**: Server evaluates a string expression or applies a function.
- **Callbacks**: `.z.pg` (sync handler), `.z.ps` (async handler), `.z.pw`
  (auth), `.z.po` (open), `.z.pc` (close).
- **Deferred sync**: Send async, block for result — avoids server blocking.

Key takeaway: the protocol is minimal (3 message types, one header format)
and the power comes from the callback hooks.

### 2.2 TorQ Architecture

TorQ layers enterprise patterns on top of Q IPC:

- **Process types**: Tickerplant, RDB, HDB, Gateway, Discovery, Monitor.
- **Gateway pattern**: Scatter query to multiple backends, gather results,
  apply join function, return aggregated result.
- **Discovery service**: Dynamic process registry with subscribe/notify.
- **Handler chain**: Composable interceptors for logging, access control,
  tracking.
- **Attribute-based routing**: Processes advertise capabilities (date ranges,
  tables), gateway routes queries accordingly.

Key takeaway: the gateway scatter-gather pattern and handler chain are the
most reusable patterns.  We should design hooks that make these possible
later.

### 2.3 Kerf1 wire.c

Kerf's wire protocol (from `kerf1/src/wire.c` and `inet.c`):

- **Message header (MESSAGE0)**: 16 bytes — endianness, zip_type,
  execution_type, response_type, display_type, logging_type, payload_size.
- **Payload**: Serialized K0 object tree.  Every object has a 16-byte header
  (type, ref count, size).  Sizes are rounded to powers of 2.
- **Execution types**: none, string_eval, string_call, apply, json.
- **Response types**: no_ack (async), full (sync), abbreviated, tmpfilename.
- **Event loop**: `select()`-based, single-threaded.  CONNECTION_POOL array
  indexed by fd.  Incremental reads (header phase, then payload phase).
- **Compression**: Optional per-message.  WKDM, gzip, LZ4.  Byte-transpose
  pre-processing for better compression of columnar numeric data.
- **Fork-on-connect**: Optional process-per-connection isolation.

Key takeaway: the incremental read (header-then-payload) and the
`select()`-based event loop are directly applicable.  The power-of-2 padding
is wasteful and unnecessary for us.  The separation of execution_type and
response_type is a clean design.

### 2.4 Existing Lush Infrastructure

What we already have:

| Primitive | Location | What it does |
|-----------|----------|--------------|
| `socketopen` | `src/unix.c:1711` | TCP connect to host:port, returns `(fout . fin)` |
| `socketaccept` | `src/unix.c:1831` | Bind+listen (1-arg) or accept (3-arg), returns file handles |
| `socketselect` | `src/unix.c:1991` | `select()` wrapper for multiplexed I/O, returns ready handles |
| `bwrite` / `bread` | `src/binary.c` | Serialize/deserialize arbitrary Lush objects (lists, numbers, strings, matrices, objects) |
| `read8` / `write8` | `lsh/libc/stdio.lsh` | Byte-level I/O on file handles |
| `write-string` | built-in | Bulk string write (much faster than byte-by-byte) |
| `RemoteLush` | `lsh/libstd/remote.lsh` | Existing master-slave RPC using `bwrite`/`bread` |
| `httpd.lsh` | `packages/httpd/` | HTTP server with `socketaccept` + `socketselect` patterns |
| `json-encode` | `packages/httpd/httpd-json.lsh` | Lush values to JSON strings |

The `RemoteLush` class is the closest existing code to what we need.  It uses
`bwrite`/`bread` for serialization and `socketopen`/`socketselect` for I/O.
However, it is a master-slave model (one client per server) and does not
support multiplexed connections or a protocol header.

---

## 3. Package Layout

```
packages/wire/
  wire.lsh                  ;; Main module: server, client, protocol
  wire-c.c                  ;; C helpers: localhost binding, peer address check
  wire-c.h                  ;; Header for C helpers
  wire-serialize.lsh        ;; DataTable serialization (columnar wire format)
  tests/
    run-all.lsh             ;; Test suite
    test-wire.lsh           ;; Protocol, server, client tests
```

### Why a separate package (not under httpd or curl)

The user specified this explicitly: "it should live under its own directory
in packages; call it packages/wire".  The httpd package is for serving web
UIs, curl is for data ingestion from external APIs.  Wire is for
inter-process data exchange — a fundamentally different concern.

---

## 4. Wire Protocol Design

### 4.1 Message Header (16 bytes)

Inspired by Kerf's MESSAGE0 but simplified and without power-of-2 padding:

```
Offset  Size  Field            Values
------  ----  -----            ------
0       4     Magic            "LWIR" (0x4C574952) — identifies a wire message
4       1     Version          1 (protocol version)
5       1     MsgType          0=async, 1=sync-request, 2=response, 3=error
6       1     Encoding         0=sexp (text), 1=bwrite (binary), 2=json
7       1     Reserved         0 (future: compression flags)
8       4     MsgID            uint32 little-endian — correlation ID
12      4     PayloadLen       uint32 little-endian — payload byte count
```

Total: 16 bytes, same as Kerf.  The magic bytes allow us to detect
non-wire connections (e.g., plain HTTP requests hitting the wrong port)
and reject them cleanly.

**Why MsgID (unlike Kerf/Q)**:  Kerf and Q match requests to responses by
temporal ordering on the socket (one outstanding request at a time).  Adding
a MsgID costs 4 bytes per message but enables:
- Pipelined requests (send multiple before reading responses)
- Out-of-order responses (server can process faster queries first)
- Better error reporting ("error for query #42")
- Async callbacks (server can push events tagged with an ID)

For Phase 1, we will use MsgID = 0 (sequential) and not implement
pipelining, but the field is there for future use.

### 4.2 Message Types

| Type | Name | Direction | Semantics |
|------|------|-----------|-----------|
| 0 | async | client→server | Fire-and-forget.  Server evaluates, discards result. |
| 1 | sync | client→server | Server evaluates, sends response (type 2) back. |
| 2 | response | server→client | Result of a sync request.  Carries MsgID of request. |
| 3 | error | server→client | Error during evaluation.  Payload is error message string. |

### 4.3 Encoding Formats

| Code | Name | Payload format | Use case |
|------|------|----------------|----------|
| 0 | sexp | UTF-8 text S-expression | Cross-language: any client can send `"(+ 1 2)"` |
| 1 | bwrite | Lush binary (`bwrite`/`bread` format) | Lush-to-Lush: fast, preserves all types including matrices |
| 2 | json | JSON string | Cross-language: R/Python clients send JSON-RPC-like requests |

**Sexp encoding** is the default and the simplest.  The client sends a string
like `(==> (==> db table "trades") num-rows)` and the server evaluates it.
The result is printed as an s-expression string and sent back.  Any language
can do this with a TCP socket.

**Binary encoding** uses Lush's built-in `bwrite`/`bread`.  This preserves
exact types (double, int, matrix, list, string) without parsing overhead.
Ideal for Lush-to-Lush communication and bulk data transfer.

**JSON encoding** is for structured RPC from other languages:
```json
{"fn": "table-query", "args": ["trades", "select * from trades where price > 100"]}
```
The server dispatches based on `fn` and returns JSON-encoded results.

### 4.4 DataTable Wire Format

For transferring DataTable objects between processes, we need a columnar
wire format.  This is handled by `wire-serialize.lsh`.

**Approach A (Phase 1): Text/S-expression**

Serialize a DataTable as an s-expression:
```lisp
((names ("price" "qty" "symbol"))
 (types (real int string))
 (nrows 3)
 (data (
   (1.5 2.5 3.5)             ;; price column
   (100 200 300)              ;; qty column
   ("AAPL" "GOOG" "MSFT")))) ;; symbol column
```

Simple, human-readable, works from any language.  Slow for large tables.

**Approach B (Phase 1): Binary via bwrite**

Use `bwrite` to serialize the column list directly.  The server wraps the
DataTable metadata and column data into a list:

```lisp
(list "datatable" names types nrows columns)
```

The client receives this via `bread` and reconstructs the DataTable using
`datatable-from-columns`.  This is fast and preserves exact numeric types.
However, it only works between Lush instances.

**Approach C (Future): Columnar binary format**

A custom binary format inspired by Kerf's wire.c:
- Schema header (column names, types, row count)
- Each column as a contiguous block of native-endian bytes
- String columns as codes + pool (matching columnardb's on-disk format)
- Optional per-column LZ4 compression with byte-transpose pre-processing

This would enable zero-copy reception (receive directly into mmap'd storage)
and efficient cross-language reading.  Phase 2+.

### 4.5 Handshake (Future)

Phase 1: no handshake.  The first message on a new connection is a regular
wire message.  The server checks the magic bytes; if they don't match, it
closes the connection.

Future: after TCP connect, the client sends a capabilities + credentials
message:

```
"user:password\x01\x00"   (like Q: username:password + version byte + null)
```

The server responds with a single byte (agreed version) or closes the
connection on auth failure.  The `.wire.pw` callback handles custom auth.

---

## 5. Server Design

### 5.1 WireServer Class

```lisp
(defclass WireServer object
  port                    ;; int: listening port
  listen-sock             ;; file handle: listening socket
  clients                 ;; htable: fd-key -> client-entry
  running                 ;; boolean: event loop control
  handlers                ;; htable: handler name -> function

  ;; Callbacks (Q-style .z.* hooks)
  on-connect              ;; (lambda (client-id) ...) — called on new connection
  on-disconnect           ;; (lambda (client-id) ...) — called on close
  on-sync                 ;; (lambda (client-id expr) ...) — sync query handler
  on-async                ;; (lambda (client-id expr) ...) — async message handler
  on-error                ;; (lambda (client-id err) ...) — error hook

  ;; State
  next-client-id          ;; int: monotonic client ID counter
  localhost-only          ;; boolean: restrict to 127.0.0.1 connections
)
```

Client entries track per-connection state:
```lisp
;; client-entry: (id fin fout read-state)
;; read-state: () or partial header/payload buffer
```

### 5.2 Event Loop

The server uses `socketselect` for multiplexed I/O, following the httpd.lsh
pattern but supporting multiple concurrent connections:

```lisp
(defmethod WireServer serve ()
  ;; Bind listening socket
  (setq listen-sock (socketaccept port))
  (setq running t)
  (while running
    ;; Build select set: listening socket + all client read fds
    (let ((ready (apply socketselect (cons timeout (cons listen-sock client-fds)))))
      ;; Handle ready fds
      (each ((fd ready))
        (cond
         ((= fd listen-sock)
          (==> this _accept-client))
         (t
          (==> this _read-client fd)) ) ) ) ) )
```

**Critical design note**: Lush's `socketaccept` with the 1-argument form
returns a listening socket handle.  Subsequent 3-argument calls with that
handle accept individual connections.  The `socketselect` function tells us
when the listening socket has a pending connection or when a client socket
has data to read.

### 5.3 Localhost-Only Restriction

The current `socketaccept` implementation in `src/unix.c` binds to all
interfaces (`AI_PASSIVE` with `NULL` host).  For localhost-only, we have
two options:

**Option A: C helper function (preferred)**

Add a small C file `wire-c.c` with a function that creates a listening
socket bound specifically to `127.0.0.1`:

```c
int wire_bind_localhost(int port) {
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1
    addr.sin_port = htons(port);
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 16);  // backlog of 16
    return sock;
}
```

Also provide `wire_accept(int listen_fd)` which calls `accept()` and
returns the new fd, and `wire_peer_is_local(int fd)` which uses
`getpeername()` to verify the peer is `127.0.0.1` or `::1`.

These are compiled via `dhc-make` and called from Lush.  The returned
fd is wrapped in Lush file handles using `fd-open` or a similar mechanism.

**Option B: Post-accept check (simpler, fallback)**

Use the standard `socketaccept` but immediately check the peer address after
accepting.  If the peer is not localhost, close the connection.  This is
less efficient (the TCP handshake completes before we reject) but requires
no C code.

**Decision**: Implement Option A for correctness (the kernel rejects
non-local connections at the TCP level).  Fall back to Option B if the C
compilation step proves problematic.

### 5.4 Message Reading (Incremental)

Following Kerf's two-phase incremental read pattern:

```
Phase 1: Read 16-byte header
  - If < 16 bytes available, buffer partial header, return to select loop
  - When complete, parse magic, version, type, encoding, msgid, payload_len
  - Validate magic ("LWIR"), reject bad connections
  - Allocate payload buffer of payload_len bytes

Phase 2: Read payload
  - If < payload_len bytes available, buffer partial payload, return to select
  - When complete, dispatch based on msg_type and encoding
```

For Phase 1 implementation, we simplify: since Lush's `read8` and
`socketselect` work at the FILE* level with buffering, we can do blocking
reads within a single select-ready callback.  A full non-blocking incremental
read would require C-level buffering (Phase 2 optimization).

### 5.5 Default Handlers

The server provides sensible defaults for the callback hooks:

```lisp
;; Default sync handler: evaluate s-expression in server's environment
(de _wire-default-on-sync (client-id expr)
  (eval (read-string expr)) )

;; Default async handler: evaluate and discard result
(de _wire-default-on-async (client-id expr)
  (eval (read-string expr))
  () )
```

Users customize these to implement:
- Table-specific query handlers (like Q's `.z.pg`)
- Access control (check client-id against whitelist)
- Logging and monitoring
- Gateway scatter-gather dispatch

### 5.6 Binding to the listening socket from Lush FILE* handles

The `socketaccept` C code returns Lush FILE* handles (file_R_class /
file_W_class).  The `socketselect` function works with these handles.
However, to support multiple concurrent connections, we need to keep the
listening socket open and accept multiple times.  The existing 3-argument
form of `socketaccept` does this:

```lisp
(setq listen-sock (socketaccept port))          ;; returns listening fd
(socketaccept listen-sock 'fin 'fout)           ;; accepts one connection
;; listen-sock remains open for more connections
```

This is exactly the pattern httpd.lsh uses in its accept loop.

---

## 6. Client Design

### 6.1 wire-connect Function

```lisp
(de wire-connect (host port)
  ;; Returns a wire-connection handle (htable with fin, fout, msgid counter)
  ;; Uses socketopen to establish TCP connection
  ...)
```

### 6.2 wire-call (Synchronous RPC)

```lisp
(de wire-call (conn expr)
  ;; Send sync request, block until response
  ;; Returns the result value
  ...)
```

Equivalent to Q's `h "expr"` (positive handle = sync).

### 6.3 wire-send (Async Fire-and-Forget)

```lisp
(de wire-send (conn expr)
  ;; Send async message, don't wait for response
  ...)
```

Equivalent to Q's `(neg h) "expr"` (negative handle = async).

### 6.4 wire-close

```lisp
(de wire-close (conn)
  ;; Close the connection
  ...)
```

### 6.5 Example Client Usage

```lisp
(libload "wire/wire")
(setq conn (wire-connect "localhost" 5555))
(wire-call conn "(+ 1 2)")                      ;; => 3
(wire-call conn "(==> db tables)")               ;; => ("trades" "quotes")
(wire-call conn "(==> (==> db table \"trades\") num-rows)")  ;; => 50000
(wire-close conn)
```

---

## 7. Server Startup and argv Handling

### 7.1 argv in Lush

Lush uses dynamic scoping for interpreted code.  The `startup` function in
`sys/stdenv.lsh` receives argv as a parameter:

```lisp
(de startup argv
  ...
  (when (consp argv)
    (libload (car argv))     ;; loads the script
    (setq argv ()) )
  ...)
```

Because Lush uses dynamic scoping, the `argv` binding from `startup` is
visible inside the loaded script and inside any files that script loads.
This was verified experimentally:

```
$ lush outer.lsh --port 5555
outer argv = ("outer.lsh" "--port" "5555")
inner argv = ("outer.lsh" "--port" "5555")    ;; loaded by outer via (load ...)
```

This means: the wire package can parse `argv` directly, and it will work
correctly whether the user runs `lush wire-server.lsh --port 5555` directly
or loads wire.lsh from within a `gateway-agent.lsh` wrapper script.

### 7.2 Port Configuration

The wire server determines its port from (in priority order):

1. Explicit argument to `wire-server-start`: `(wire-server-start 5555)`
2. `argv` parsing: `--port 5555` or `-p 5555`
3. Environment variable: `LUSH_WIRE_PORT`
4. Default: 5555

### 7.3 argv Parser

```lisp
(de wire-parse-argv ()
  ;; Scan argv for --port/-p, return port number or ()
  (let ((port ())
        (args (if (and (boundp 'argv) argv) argv ())) )
    (while (consp args)
      (cond
       ((or (= (car args) "--port") (= (car args) "-p"))
        (when (consp (cdr args))
          (setq port (val (cadr args)))
          (setq args (cddr args)) ) )
       (t (setq args (cdr args))) ) )
    port ) )
```

### 7.4 Example: Standalone Server Script

`examples/wire-server-standalone.lsh`:
```lisp
#!/usr/bin/env lush
(libload "columnardb/columnardb")
(libload "wire/wire")

;; Load a database
(setq db (new Database "/data/mydb"))

;; Start wire server (reads --port from argv, or uses default)
(wire-server-start)
```

Run as: `lush examples/wire-server-standalone.lsh --port 5555`

### 7.5 Example: Gateway Agent Script

`examples/gateway-agent.lsh`:
```lisp
#!/usr/bin/env lush
(libload "wire/wire")

;; Gateway that fans out queries to multiple backends
(setq backends
  (list (wire-connect "localhost" 5556)
        (wire-connect "localhost" 5557) ))

;; Custom sync handler: scatter to backends, gather results
(de gateway-query (client-id expr)
  (let ((results ()))
    (each ((b backends))
      (setq results (nconc1 results (wire-call b expr))) )
    results ) )

;; Start gateway server with custom handler
(let ((server (wire-server-new)))
  (setq :server:on-sync gateway-query)
  (==> server serve) )
```

Run as: `lush examples/gateway-agent.lsh --port 5555`

The inner `wire-server-new` reads `--port` from `argv`, which still contains
`("examples/gateway-agent.lsh" "--port" "5555")` because of dynamic scoping.

---

## 8. C Helper Module (wire-c.c)

### 8.1 Functions

```c
// Bind a TCP listening socket to 127.0.0.1:port
// Returns fd on success, -1 on error
int wire_bind_localhost(int port);

// Bind a TCP listening socket to 0.0.0.0:port (all interfaces)
// Returns fd on success, -1 on error
int wire_bind_any(int port);

// Accept a connection on a listening socket
// Returns new fd, sets peer_ip (as uint32 in network order)
// Returns -1 on error
int wire_accept(int listen_fd, int *peer_ip_out);

// Check if a peer address is localhost (127.0.0.1 or ::1)
int wire_peer_is_local(int fd);

// Read exactly n bytes from fd (blocking)
// Returns number of bytes read, or -1 on error/EOF
int wire_read_exact(int fd, unsigned char *buf, int n);

// Write exactly n bytes to fd (blocking, handles partial writes)
// Returns 0 on success, -1 on error
int wire_write_all(int fd, unsigned char *buf, int n);

// Get the 16-byte wire header from a buffer
// Validates magic, returns 0 on success, -1 on bad header
int wire_parse_header(unsigned char *buf, int *msg_type,
                      int *encoding, int *msg_id, int *payload_len);

// Build a 16-byte wire header into a buffer
void wire_build_header(unsigned char *buf, int msg_type,
                       int encoding, int msg_id, int payload_len);
```

### 8.2 DHC Wrappers

The C functions are wrapped via `dhc-make` for calling from Lush:

```lisp
(de _wire-bind-localhost (port) ...)    ;; compiled, returns fd as int
(de _wire-bind-any (port) ...)          ;; compiled, returns fd as int
(de _wire-accept (listen-fd) ...)       ;; compiled, returns new fd
(de _wire-peer-is-local (fd) ...)       ;; compiled, returns 0 or 1
(de _wire-read-exact (fd buf n) ...)    ;; compiled, returns count
(de _wire-write-all (fd buf n) ...)     ;; compiled, returns 0 or -1
```

### 8.3 Integration with Lush FILE* handles

A key challenge: `socketaccept` and `socketselect` work with Lush FILE*
handles (via `fdopen`), but our C functions return raw fds.  We need to
bridge this.

The approach: use `fd-open` from `libc/files.lsh` to wrap the raw fd
returned by our C functions into a Lush file descriptor.  Then use
`socketselect` with these handles in the event loop.  For actual reading
and writing, the C-level `wire_read_exact`/`wire_write_all` operate on the
raw fd directly (bypassing FILE* buffering, which is actually desirable for
a message-oriented protocol).

Alternatively, use `socketaccept` for the accept loop (it already handles
the fd→FILE* conversion) and only use the C helpers for localhost binding
and peer checking.  This is the simpler path for Phase 1.

**Phase 1 approach**:
- Use `_wire-bind-localhost` to create the listening socket (C level)
- Wrap the fd as a Lush FILE* handle for use with `socketselect`
- Use `socketaccept` with the wrapped handle for accepting connections
  (the 3-arg form accepts on an existing listening handle)
- After accept, use `_wire-peer-is-local` as a secondary check
- Read/write the wire header via compiled C functions operating on idx1
  buffers, then use `read8`/`write8` or `bwrite`/`bread` for payloads

---

## 9. DataTable Serialization

### 9.1 Phase 1: S-expression Encoding

For sexp encoding (cross-language compatible):

```lisp
(de wire-serialize-datatable (dt)
  ;; Returns a string: s-expression representation of the DataTable
  (let ((names (==> dt col-names))
        (types (==> dt col-types))
        (nrows (==> dt num-rows))
        (ncols (==> dt num-cols)) )
    ;; Format: ((names (...)) (types (...)) (nrows N) (data (...)))
    ... ) )

(de wire-deserialize-datatable (sexp)
  ;; Parse s-expression, rebuild DataTable via datatable-from-columns
  ... )
```

### 9.2 Phase 1: Binary Encoding (Lush-to-Lush)

For bwrite encoding, the DataTable is wrapped:

```lisp
(de wire-pack-datatable (dt)
  ;; Returns a list suitable for bwrite:
  (list 'datatable
        (==> dt col-names)
        (==> dt col-types)
        (==> dt num-rows)
        ;; Column data: list of idx1 or string lists
        (let ((cols ()))
          (for (i 0 (1- (==> dt num-cols)))
            (let ((col (==> dt get-column i))
                  (type (==> dt col-type i)) )
              (if (= type 'string)
                ;; String columns: send as list of strings
                (let ((strings ()))
                  (for (r 0 (1- (==> dt num-rows)))
                    (setq strings (nconc1 strings (==> dt get r i))) )
                  (setq cols (nconc1 cols strings)) )
                ;; Numeric columns: send the idx1 directly (bwrite handles it)
                (let ((data (select-type type
                              (double-matrix (==> dt num-rows))
                              ... )))
                  (for (r 0 (1- (==> dt num-rows)))
                    (data r (col r)) )
                  (setq cols (nconc1 cols data)) ) ) ) )
          cols ) ) )
```

`bwrite` natively serializes idx (matrix) objects, so numeric columns
transfer efficiently as contiguous binary data.

### 9.3 Phase 2+: Custom Columnar Binary Format

A future wire format that matches columnardb's on-disk layout:

```
Schema (variable length):
  uint16  ncols
  uint32  nrows
  For each column:
    uint8   type (0=real, 1=int, 2=float, 3=stamp, 4=string)
    uint16  name_len
    char[]  name (UTF-8, not null-terminated)

Column data (contiguous blocks):
  For each numeric column:
    uint8   compression (0=none, 1=lz4)
    uint32  compressed_len (or uncompressed_len if no compression)
    byte[]  data (native endian, column-major)
  For each string column:
    uint32  pool_size
    char[]  pool (null-terminated strings)
    uint32  codes_len
    int32[] codes
```

This enables:
- Direct memory mapping of received data
- Per-column LZ4 compression with byte-transpose pre-processing
- Cross-language reading (any language that understands the format)
- Streaming reception (process columns as they arrive)

---

## 10. Cross-Language Access

### 10.1 From R

R can connect via a socket and send text:

```r
library(socketConnection)  # or use {connections}

con <- socketConnection("localhost", 5555, blocking = TRUE, open = "r+b")

# Send wire header + sexp payload
send_wire_msg <- function(con, expr) {
  payload <- charToRaw(expr)
  header <- raw(16)
  header[1:4] <- charToRaw("LWIR")   # magic
  header[5]   <- as.raw(1)            # version
  header[6]   <- as.raw(1)            # sync
  header[7]   <- as.raw(0)            # sexp encoding
  header[13:16] <- writeBin(length(payload), raw(), size=4, endian="little")
  writeBin(c(header, payload), con)
  # Read response header + payload
  resp_hdr <- readBin(con, raw(), 16)
  resp_len <- readBin(resp_hdr[13:16], integer(), size=4, endian="little")
  resp_body <- readChar(con, resp_len)
  resp_body
}

send_wire_msg(con, "(==> (==> db table \"trades\") num-rows)")
# => "50000"
close(con)
```

For DataTable results, the R client would parse the s-expression
representation (column-major lists of values) into an R data.frame.

### 10.2 From Python

```python
import socket, struct

def wire_call(host, port, expr):
    sock = socket.create_connection((host, port))
    payload = expr.encode('utf-8')
    header = b'LWIR'                         # magic
    header += bytes([1, 1, 0, 0])            # version, sync, sexp, reserved
    header += struct.pack('<I', 0)           # msg_id
    header += struct.pack('<I', len(payload)) # payload_len
    sock.sendall(header + payload)
    resp_hdr = sock.recv(16)
    resp_len = struct.unpack('<I', resp_hdr[12:16])[0]
    resp = b''
    while len(resp) < resp_len:
        resp += sock.recv(resp_len - len(resp))
    sock.close()
    return resp.decode('utf-8')

wire_call('localhost', 5555, '(+ 1 2)')
# => '3'
```

---

## 11. Tests

### 11.1 Test Structure

`packages/wire/tests/run-all.lsh` loads the test framework and runs tests.
`packages/wire/tests/test-wire.lsh` contains the actual test cases.

### 11.2 Test Cases

1. **wire-parse-argv**: Parse port from simulated argv
2. **wire-parse-argv from embedded script**: Set argv to simulate
   gateway-agent calling wire-server, verify port is correctly parsed
3. **Header encode/decode**: Build a header, parse it back, verify fields
4. **Server start/stop**: Start server on a port, verify it's listening,
   stop it
5. **Client connect/disconnect**: Connect to server, verify connection,
   disconnect
6. **Sync call (sexp)**: Client sends `"(+ 1 2)"`, verify response is `"3"`
7. **Sync call (eval expression)**: Client sends `(list 1 2 3)`, verify
8. **Async send**: Client sends async message, verify server processes it
   (set a global variable, check from client)
9. **Multiple clients**: Two clients connected simultaneously, both get
   correct responses
10. **Localhost-only**: Verify server binds to 127.0.0.1 (test by connecting
    from localhost)
11. **Bad magic rejection**: Send garbage bytes, verify server closes
    connection
12. **Custom handler**: Set custom on-sync handler, verify it's called
13. **DataTable round-trip (sexp)**: Serialize a DataTable, send via wire,
    receive and reconstruct, verify data integrity
14. **DataTable round-trip (binary)**: Same but with bwrite encoding
15. **Error handling**: Send expression that causes an error, verify error
    response

### 11.3 Test Infrastructure

Tests need to run server and client in the same Lush process.  Approach:
start the server in a child process (via `filteropen` or `bground`) and
connect as a client from the test process.  Or: start the server, run one
iteration of the event loop, connect, exchange messages, stop.

The simplest approach for Phase 1: use the existing `sys` function to start
a Lush server process in the background, wait for it to be ready, run client
tests, then kill the server.

---

## 12. Implementation Phases

### Phase 1: Core IPC (this implementation)

**Files to create:**

| File | Description |
|------|-------------|
| `packages/wire/wire.lsh` | Main module: protocol, server, client |
| `packages/wire/wire-c.c` | C helpers: localhost bind, accept, peer check, header parse/build, read/write exact |
| `packages/wire/wire-c.h` | C header |
| `packages/wire/wire-serialize.lsh` | DataTable serialization (sexp + binary) |
| `packages/wire/tests/run-all.lsh` | Test runner |
| `packages/wire/tests/test-wire.lsh` | Test cases |

**Capabilities:**

- Start a wire server on a port (from argv or explicit)
- Accept connections (localhost-only)
- Sync and async message exchange
- S-expression and bwrite encoding
- Default eval handler + custom handler hooks
- Client API: connect, call, send, close
- DataTable serialization (sexp and binary round-trip)
- Server embeddable in other scripts (gateway-agent pattern)
- Basic error handling (eval errors → error response)

### Phase 2: Robustness and Performance

- Non-blocking incremental reads (handle partial messages across select calls)
- Connection timeout and keep-alive
- Per-column LZ4 compression for DataTable wire format
- Byte-transpose pre-processing (from Kerf's zip.c) for better compression
- `wire-broadcast`: send one message to multiple connections (like Q's `-25!`)
- Connection pool management (reconnect on failure)

### Phase 3: Security

- Handshake with credentials (user:password)
- `.wire.pw` callback for custom authentication
- Per-connection user tracking (like Q's `.z.u`)
- Handler-level access control (whitelist of allowed functions)

### Phase 4: TorQ-Like Infrastructure

- Discovery service: process registration and lookup
- Gateway scatter-gather: fan out queries, aggregate results
- Pub/sub: tickerplant pattern for real-time data distribution
- Handler chain: composable interceptors for logging, monitoring, access
- Process types and hierarchical configuration

---

## 13. Key Design Decisions and Rationale

### Why not extend httpd.lsh?

HTTP is request-response, stateless, and text-heavy.  Wire IPC needs:
- Persistent connections (no per-request overhead)
- Binary data transfer (matrices, columnar data)
- Bidirectional communication (server can push events)
- Minimal framing overhead (16 bytes vs. HTTP headers)

The httpd package is valuable for web UIs and REST APIs.  Wire is for
high-throughput inter-process data exchange.

### Why not extend RemoteLush?

RemoteLush is single-client, single-server.  It lacks:
- Multiple concurrent connections
- Protocol headers (no message type, no length framing)
- Custom handlers (hardcoded eval-and-return)
- Cross-language support

However, RemoteLush's use of `bwrite`/`bread` is exactly right for
Lush-to-Lush binary encoding.  We reuse that pattern.

### Why a 16-byte header (not 8 like Q)?

Q's 8-byte header packs endianness, msg-type, 2 reserved bytes, and a
4-byte length — supporting messages up to ~2GB.  Our 16-byte header adds:
- Magic bytes (4 bytes): allows detecting non-wire connections
- Encoding field (1 byte): supports sexp, binary, and JSON
- MsgID (4 bytes): enables future pipelining and correlation

The extra 8 bytes per message are negligible compared to payload sizes.

### Why sexp as default encoding (not binary)?

S-expressions are:
- Human-readable and debuggable
- Parseable from any language (trivial to implement)
- Already Lush's native syntax
- Sufficient for most queries and small results

Binary (bwrite) is used when performance matters (large DataTable transfers
between Lush instances).  The encoding field in the header lets the client
choose.

### Why localhost-only by default?

Security baseline: until we implement authentication, restricting to
localhost prevents unauthorized network access.  This is the right default
for a development/single-machine deployment.  A flag (or future auth) can
open it to other hosts.

---

## 14. Comparison with Kerf1 Decisions

| Aspect | Kerf1 | Lush Wire | Rationale |
|--------|-------|-----------|-----------|
| Header size | 16 bytes | 16 bytes | Same |
| Payload sizing | Power-of-2 (wastes up to 50%) | Exact length | No wasted bandwidth |
| Message types | 5 execution × 4 response | 4 msg types | Simpler; execution vs response distinction handled by encoding |
| Serialization | Custom K0 tree format | bwrite (native) + sexp (text) | Reuse existing infrastructure |
| Event loop | `select()` in C | `socketselect` in Lush | Same underlying mechanism |
| Compression | WKDM/gzip/LZ4 + byte-transpose | Future Phase 2 | Keep Phase 1 simple |
| Auth | Field exists but unused | Future Phase 3 | Same pragmatic approach |
| Fork-on-connect | Supported | Not planned | Lush is single-threaded; multiplexing is simpler |
| Connection pool | Fixed array[FD_SETSIZE] | htable by fd | Dynamic, no fd limit |

---

## 15. Verification Plan

```bash
cd /workspaces/claude-sandbox/lush-claude
# Clean compiled C
rm -rf packages/wire/C packages/columnardb/C packages/datatable/C packages/timedate/C

# Run wire tests
TMPDIR=/tmp/claude bin/lush packages/wire/tests/run-all.lsh

# Verify columnardb still passes
TMPDIR=/tmp/claude bin/lush packages/columnardb/tests/run-all.lsh

# Verify datatable still passes
TMPDIR=/tmp/claude bin/lush packages/datatable/tests/run-all.lsh
```

Manual integration test:
```bash
# Terminal 1: start server
TMPDIR=/tmp/claude bin/lush packages/wire/examples/server.lsh --port 5555

# Terminal 2: connect client
TMPDIR=/tmp/claude bin/lush -e '
  (libload "wire/wire")
  (setq c (wire-connect "localhost" 5555))
  (printf "result: %l\n" (wire-call c "(+ 1 2)"))
  (wire-close c)
'
```
