# libuv Package & Coinbase Pipeline -- Design Notes

## Architecture

### Core Constraint

Lush's interpreter (GC, symbol table, evaluation) is not thread-safe. Any
background thread must do pure C work only -- no Lush calls. This rules out
pthreads as the primary architecture for async I/O.

The solution: each component is an **independent Lush process** that owns its
interpreter exclusively. A libuv event loop drives all I/O within each process.
libuv callbacks can freely call Lush functions because the interpreter is idle
during `uv_run()`. No interpreter-lock concern arises.

### Why libuv (not pthreads)

1. **Connection lifecycle management.** Reconnection with exponential backoff,
   DNS re-resolution, and health-check timers map directly to libuv's timer +
   TCP primitives. With raw pthreads this would all be hand-rolled.

2. **Unified event loop.** The feed handler receives from Coinbase and sends to
   downstream wire clients. libuv multiplexes both directions in a single loop.

3. **Multiple exchange connections.** Each exchange is another `uv_tcp` handle
   in the same event loop. No additional threads or fd-set management.

4. **Parse hot path is fast enough inline.** yyjson parses at 1.7+ GB/s; a
   Coinbase message is ~1KB at ~100 msgs/sec peak -- roughly 100us/sec of parse
   work. No need for a separate thread.

5. **Battle-tested.** Node.js, neovim, Julia all use libuv for this pattern.

### TorQ-Inspired Process Topology

The architecture follows Q/kdb+ TorQ conventions: independent processes
connected by async IPC, each with a single clear responsibility.

```
Coinbase WSS --> Feed Handler (19960) --wire broadcast--> RDB (19961)
                                                            |
                                      +---------------------+
                                      |                     |
                                 Analytics (19963)     HDB Writer (19965)
                                 (VWAP, MA, etc.)           |
                                      |                     v
                                      |           /datafast1/experiment/coinbasedata/
                                      |             YYYY.MM.DD/ticker/
                                      |
                                 Gateway (19964) <---- HDB Reader (19962)
                                      |
                                 REPL queries
```

The feed handler **is** the tickerplant -- it parses data and broadcasts packed
DataTable batches directly to all connected downstream processes via
`wire-send` / `broadcast`. No separate TP process needed at current scale.

### Port Assignments

| Process      | Port  | Purpose                        |
|-------------|-------|--------------------------------|
| Feed Handler | 19960 | Wire broadcast of parsed ticks |
| RDB          | 19961 | Query server for in-memory data|
| HDB Reader   | 19962 | Query server for historical data|
| Analytics    | 19963 | VWAP, MA, spread, volatility   |
| Gateway      | 19964 | Unified query entry point      |
| HDB Writer   | 19965 | Status/flush commands          |

---

## API Summary

### Process Descriptions

**Feed Handler (port 19960)** -- `feed-handler.lsh`, entry: `scripts/coinbase-feed.lsh`
- Connects to `wss://ws-feed.exchange.coinbase.com`
- Subscribes to `ticker`, `level2`, and `heartbeat` channels for BTC-USD and ETH-USD
- Hot-path C JSON parser extracts: price, bid, ask, side, volume (last_size), time, product
- Broadcasts parsed ticks to all connected wire clients
- Auto-reconnects on WebSocket disconnection (5s delay)

**RDB (port 19961)** -- `rdb.lsh`, entry: `scripts/coinbase-rdb.lsh`
- Connects to feed handler as wire client
- Accumulates ticker and L2 data into DataTables
- Ticker schema: product, price, bid, ask, side, volume, time, local_time
- `local_time` stamped via `(timestamp-now)` at receive time (us precision)
- Serves queries: count, last-N, snapshot, where, rows-from

**HDB Writer (port 19965)** -- `hdb-writer.lsh`, entry: `scripts/coinbase-hdb-writer.lsh`
- Connects to RDB, periodically pulls new rows via high-water mark
- Flushes ticker and L2 data to date-partitioned columnar files on disk
- Flush interval: 60 seconds
- Data directory: `/datafast1/experiment/coinbasedata/`

**HDB Reader (port 19962)** -- `hdb-reader.lsh`, entry: `scripts/coinbase-hdb-reader.lsh`
- Reads date-partitioned data from `/datafast1/experiment/coinbasedata/`
- Lazy-loads partitions into memory on first query
- Supports range queries across multiple dates

**Analytics (port 19963)** -- `analytics.lsh`, entry: `scripts/coinbase-analytics.lsh`
- Connects to RDB, polls for new ticker rows every 1 second
- Computes per-product analytics over a sliding window (20 ticks):
  moving average (O(1) running sum), bid-ask spread, rolling volatility,
  min/max price, TWAP, VWAP
- Tracks `sum_pv` and `sum_vol` per product for VWAP

**Gateway (port 19964)** -- `gateway.lsh`, entry: `scripts/coinbase-gateway.lsh`
- Connects to all 4 backend services
- Smart query routing: auto-detects target from query pattern
- Explicit routing: `("rdb" sub-query)`, `("ana" sub-query)`, etc.
- Permissions support (localhost trusted by default)

### Ticker Schema

| Column     | Type   | Source                               |
|-----------|--------|--------------------------------------|
| product   | string | Coinbase `product_id`                |
| price     | real   | Coinbase `price`                     |
| bid       | real   | Coinbase `best_bid`                  |
| ask       | real   | Coinbase `best_ask`                  |
| side      | string | Coinbase `side`                      |
| volume    | real   | Coinbase `last_size` (trade volume)  |
| time      | stamp  | Coinbase `time` (broker timestamp)   |
| local_time| stamp  | `(timestamp-now)` at receive (us)    |

### Monitor API (`coinbase-monitor.lsh`)

```lisp
(libload "libuv/coinbase-monitor")
(coinbase-status)                   ;; check all services
(coinbase-print-status)             ;; formatted status display
(coinbase-vwap "BTC-USD")           ;; live VWAP
(coinbase-analytics "BTC-USD")      ;; alist: mavg, spread, vwap, ...
(coinbase-last-trade "BTC-USD")     ;; alist: price, volume, time, local_time
(coinbase-last-n "ticker" 10)       ;; last N rows as DataTable
(coinbase-rdb-stats)                ;; RDB row counts
(coinbase-hdb-count)                ;; HDB date partitions and counts
```

---

## Implementation Details

### libuv Integration

libuv is vendored (~20 .c files) into `packages/libuv/`. Built via lushmake.
Exposes core handles: `uv_loop`, `uv_tcp`, `uv_timer`, `uv_poll`, `uv_signal`,
`uv_async`.

### WebSocket Client

A thin WebSocket client built on top of libuv's `uv_tcp_t` (`ws-client.lsh`).
Implements the WS handshake and frame protocol directly rather than using
libcurl's multi-interface. This gives libuv full control of the socket lifecycle
and avoids the complexity of the curl_multi_socket_action integration.

### Wire Protocol Integration

The existing wire protocol's `select()` loop is replaced by `uv_poll_t` handles
in the libuv event loop. The wire listen-fd and client-fds are registered as
libuv poll handles. When libuv signals readability, the existing
`_try-read-client` / `_handle-message` logic runs unchanged. The wire protocol
bytes and framing stay the same; only the I/O multiplexing layer changed.

Publishing model: **broadcast to all connected clients**. Any process that
connects to the feed handler's wire port automatically receives all data. No
subscription negotiation needed.

### IPC Cost

- `wire-pack-datatable` serializes column idx1 arrays via bwrite
- For a 100-row batch (5 columns: int + 2*double + long + int): ~3.2KB raw
- Localhost TCP loopback: ~1us per syscall
- Total per-batch overhead: ~5-10us (serialize + syscall + deserialize)
- At 10 batches/sec: 50-100us/sec -- negligible

### Glitch Handling

| Glitch                    | libuv Mechanism                            |
|---------------------------|--------------------------------------------|
| WebSocket disconnect      | `on_read` returns `UV_EOF`; start reconnect timer |
| Reconnect backoff         | `uv_timer_start` with exponential delay    |
| Heartbeat / keepalive     | `uv_timer_start` periodic; detect stale conn |
| Sequence gap detection    | Track sequence numbers in callback; log gaps |
| DNS change after failover | `uv_getaddrinfo` async DNS resolution      |
| Graceful shutdown         | `uv_signal_t` for SIGTERM; drain + close   |
| Health logging            | `uv_timer_t` periodic: emit stats          |

### C Layer Changes

- `packages/json/json-c.c`: `lush_json_parse_ticker()` has `double *volumes`
  parameter; extracts `last_size` field from Coinbase ticker JSON
- Feed handler passes volumes to `json-parse-ticker` and includes `"volume"` in
  ticker broadcast alist
- Analytics tracks `sum-pv` / `sum-vol` per product (running sums for VWAP)

### HDB Disk Layout

```
/datafast1/experiment/coinbasedata/
  .ctrl/
    feed-handler.pid, rdb.pid, hdb-writer.pid,
    hdb-reader.pid, analytics.pid, gateway.pid
  logs/
    feed-handler.log, rdb.log, hdb-writer.log,
    hdb-reader.log, analytics.log, gateway.log
  YYYY.MM.DD/
    ticker/
      meta.lsh
      col-0.col (product codes) + col-0-codes.col + col-0-pool-str.bin
      col-1.col (price), col-2.col (bid), col-3.col (ask)
      col-4.col (side codes), col-5.col (volume)
      col-6.col (time), col-7.col (local_time)
    l2/
      meta.lsh, col-0.col ... col-4.col
```

### Starting and Stopping

```bash
# Start (dependency order: FH -> RDB -> HDB Writer, HDB Reader -> Analytics -> Gateway)
bash packages/libuv/scripts/coinbase-start.sh

# Stop (SIGTERM in reverse order; FH and RDB have signal handlers for graceful shutdown)
bash packages/libuv/scripts/coinbase-stop.sh
```

### Files

| File                           | Purpose                        |
|--------------------------------|--------------------------------|
| `libuv.lsh`                   | libuv Lush bindings            |
| `libuv-c.c` / `libuv-c.h`    | C bridge                       |
| `libuv-config.lsh`            | Build configuration            |
| `ws-client.lsh`               | WebSocket client on libuv      |
| `feed-handler.lsh`            | Feed handler logic             |
| `rdb.lsh`                     | RDB process logic              |
| `analytics.lsh`               | Analytics engine               |
| `hdb-writer.lsh`              | HDB flush logic                |
| `hdb-reader.lsh`              | HDB query logic                |
| `gateway.lsh`                 | Query router                   |
| `permissions.lsh`             | Gateway permissions            |
| `consumer.lsh`                | Wire consumer helpers          |
| `coinbase-monitor.lsh`        | REPL convenience functions     |
| `gw-cli.lsh`                  | Gateway CLI client             |
| `scripts/coinbase-start.sh`   | Launch all processes           |
| `scripts/coinbase-stop.sh`    | Stop all processes             |
| `scripts/coinbase-feed.lsh`   | Feed handler entry point       |
| `scripts/coinbase-rdb.lsh`    | RDB entry point                |
| `scripts/coinbase-hdb-writer.lsh` | HDB writer entry point     |
| `scripts/coinbase-hdb-reader.lsh` | HDB reader entry point     |
| `scripts/coinbase-analytics.lsh`  | Analytics entry point       |
| `scripts/coinbase-gateway.lsh`    | Gateway entry point         |

---

## Known Issues / Limitations

- **Single-exchange only.** Currently hardcoded for Coinbase (BTC-USD, ETH-USD).
  Adding exchanges (Binance, Kraken) requires additional `uv_tcp` handles and
  exchange-specific JSON parsers, but the architecture supports this -- each
  exchange is just another handle in the same event loop.

- **No subscription filtering.** All connected wire clients receive all data.
  Per-table or per-product subscriptions would require a subscribe message type
  in the wire protocol.

- **No separate tickerplant.** The feed handler also acts as tickerplant. If
  scale demands it, a separate TP process can be interposed between the feed
  handler and downstream consumers.

- **Shell-script process management.** Start/stop is via bash scripts. A
  process.csv + launcher (TorQ-style) would be needed if the process count
  grows significantly.

- **No zero-copy IPC.** Wire protocol serializes via bwrite over TCP loopback.
  Zero-copy options (mmap-backed columns, Unix domain sockets with
  `sendmsg`/`SCM_RIGHTS`) are available if profiling shows the need, but
  current overhead (~50-100us/sec) is negligible.

- **Analytics VWAP accumulation.** `sum_pv` and `sum_vol` accumulate
  indefinitely per product. For very long-running sessions, numerical precision
  could degrade. A periodic reset or windowed approach should be considered.

- **Batching strategy.** No formal batching policy. Messages are broadcast as
  they arrive. Time-based (every 100ms), count-based (every 50 rows), or
  size-based (every 4KB) batching could reduce syscall overhead at higher
  message rates.
