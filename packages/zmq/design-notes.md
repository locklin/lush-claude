# ZeroMQ Package for Lush — Design Notes

## Overview

The `zmq` package provides ZMQ transport bindings for Lush: C bridge to
libzmq, DHC wrappers for the socket API, and a serialization layer
(bwrite/bread over ZMQ frames).

The database management pipeline (feed handler, RDB, analytics, HDB,
gateway, monitor) has moved to `packages/ltor/`.

## Package Structure

```
packages/zmq/
  zmq-c.h              # Minimal ZMQ API declarations (~90 lines)
  zmq-c.c              # C bridge: handle table + wrapper functions
  zmq-config.lsh       # lushmake build: compile zmq-c.c, link -lzmq
  zmq.lsh              # Main loader: DHC wrappers + high-level API
  zmq-serde.lsh        # bwrite/bread serialization over ZMQ frames
  design-notes.md      # This file
  tests/
    test-zmq.lsh         # Binding unit tests (40 tests)
    test-zmq-pipeline.lsh # PUB/SUB + ROUTER/DEALER transport tests (20 tests)
```

## C Bridge (`zmq-c.h`, `zmq-c.c`)

Minimal C bridge (~90 lines header) exposing:
- Context management: `zmq_ctx_new`, `zmq_ctx_term`
- Socket lifecycle: `zmq_socket`, `zmq_close`, `zmq_bind`, `zmq_connect`
- Send/recv: `zmq_send`, `zmq_recv`, `zmq_msg_*` family
- Polling: `zmq_poll` with a static pollitem array
- Socket options: linger, HWM, subscribe, identity

Uses a handle table (like libuv) to map integer handles to void pointers.

## Socket API (`zmq.lsh`)

DHC wrappers expose the full ZMQ socket API to Lush:
- `(zmq-ctx-new)` → context handle
- `(zmq-socket ctx type)` → socket handle
- `(zmq-bind sock addr)`, `(zmq-connect sock addr)`
- `(zmq-send-bytes sock buf len flags)`, `(zmq-recv-bytes sock buf len flags)`
- `(zmq-poll nitems timeout)`, `(zmq-poll-ready idx)`
- Socket types: `*zmq-pub*`, `*zmq-sub*`, `*zmq-router*`, `*zmq-dealer*`, etc.

## Serialization (`zmq-serde.lsh`)

Serialization layer on top of ZMQ frames using Lush bwrite/bread:
- `(zmq-pub-send sock topic data)` — multi-part PUB message
- `(zmq-sub-recv sock)` → `(topic payload)` pair
- `(zmq-query-send sock expr)` / `(zmq-query-recv sock)` — DEALER round-trip
- `(zmq-router-recv-query sock)` / `(zmq-router-send-reply sock result)`
- `(zmq-call sock expr)` — convenience wrapper (send + recv)
- `(wire-pack-datatable dt)` / `(wire-unpack-datatable packed)` — DataTable packing

## Tests

- **test-zmq.lsh** (40 tests): Unit tests for ZMQ bindings — context/socket
  lifecycle, PUB/SUB messaging, ROUTER/DEALER request/reply, poll, options.
- **test-zmq-pipeline.lsh** (20 tests): Integration tests for transport
  patterns — PUB/SUB fan-out, ROUTER/DEALER round-trip, multi-topic
  filtering, serialization round-trip.

## ZMQ vs Wire Protocol

| Aspect | Wire | ZMQ |
|--------|------|-----|
| Event loop | select() | zmq_poll |
| Pub/sub | Manual broadcast + fd tracking | PUB/SUB socket |
| Request/reply | on-sync callback | ROUTER/DEALER |
| Reconnection | Manual (WirePool) | Automatic |
| Framing | Custom 4-byte header | ZMQ handles it |
| Topic filtering | None | ZMQ_SUBSCRIBE prefix match |
| Serialization | bwrite/bread | bwrite/bread (unchanged) |

## Zero-Copy Send

For large messages (serialized DataTables, bulk analytics results), the default
`zmq_send()` copies data from the Lush buffer into ZMQ's internal message
buffer. Zero-copy send avoids this by using `zmq_msg_init_data()`, which
creates a message that references the caller's buffer directly.

### Buffer Lifetime

After `zmq_msg_send()` returns, ZMQ may still hold a raw pointer into the Lush
ubyte-matrix. If Lush's GC collects the matrix, the pointer becomes dangling.

**Solution**: The buffer is pinned in a Lush global variable `*zmq-zc-pinned*`
to prevent GC. The C free callback (`zc_free_callback`) clears a static
`zc_in_flight` flag when ZMQ is done with the data.

### Concurrency

Only one zero-copy send can be in-flight at a time (single-threaded Lush).
If `zmq-zc-busy` returns true, the high-level API functions automatically fall
back to regular (copy) send. This is transparent to the caller.

### When to Use

- **Use ZC**: Payload > ~4KB (serialized DataTables, large query results)
- **Use regular send**: Small messages (topics, control frames, short replies)

The serde layer provides `zmq-pub-send-zc` and `zmq-router-send-reply-zc`
which handle this automatically: topic/identity frames use regular copy (small),
payload frames use zero-copy.

### PUB/SUB Compatibility

`zmq_msg_send()` works identically across all socket types. For PUB with
multiple subscribers, ZMQ handles internal reference-counting transparently.
The free callback fires when the original buffer is no longer needed by any
subscriber path.

## Performance

- **Latency**: ~5-15µs per message on localhost
- **Throughput**: Millions of msgs/sec on localhost
- **Memory**: ~1MB per socket at default HWM
- **Thread safety**: ZMQ sockets are NOT thread-safe (fine for single-threaded Lush)
