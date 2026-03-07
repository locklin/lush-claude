# Coinbase Live Data Pipeline — Architecture Document

## Overview

A TorQ-inspired multi-process data pipeline that captures live BTC-USD and ETH-USD tick data from the Coinbase WebSocket feed, stores it in an in-memory RDB and persistent HDB, computes live VWAP analytics, and provides query access through a unified gateway.

All components are written in Lush, using the libuv event loop for async I/O and wire IPC for inter-process communication.

## Data Flow

```
Coinbase WSS ──→ Feed Handler (19960) ──wire broadcast──→ RDB (19961)
                                                            │
                                   ┌────────────────────────┤
                                   │                        │
                              Analytics (19963)        HDB Writer (19965)
                              (VWAP, MA, etc.)              │
                                   │                        ▼
                                   │              /datafast1/experiment/coinbasedata/
                                   │                YYYY.MM.DD/ticker/
                                   │
                              Gateway (19964) ◄──── HDB Reader (19962)
                                   │
                              REPL queries
```

## Port Assignments

| Process      | Port  | Purpose                              |
|-------------|-------|--------------------------------------|
| Feed Handler | 19960 | Wire broadcast of parsed ticks       |
| RDB          | 19961 | Query server for in-memory data      |
| HDB Reader   | 19962 | Query server for historical data     |
| Analytics    | 19963 | VWAP, MA, spread, volatility         |
| Gateway      | 19964 | Unified query entry point            |
| HDB Writer   | 19965 | Status/flush commands                |

## Process Descriptions

### Feed Handler (port 19960)
- Connects to `wss://ws-feed.exchange.coinbase.com`
- Subscribes to `ticker`, `level2`, and `heartbeat` channels for BTC-USD and ETH-USD
- Hot-path C JSON parser extracts: price, bid, ask, side, volume (last_size), time, product
- Broadcasts parsed ticks to all connected wire clients
- Auto-reconnects on WebSocket disconnection (5s delay)
- Entry point: `packages/libuv/scripts/coinbase-feed.lsh`

### RDB (port 19961)
- Connects to feed handler as wire client
- Accumulates ticker and L2 data into DataTables
- Ticker schema: product, price, bid, ask, side, volume, time, local_time
- `local_time` stamped via `(timestamp-now)` at receive time (µs precision)
- Serves queries: count, last-N, snapshot, where, rows-from
- Entry point: `packages/libuv/scripts/coinbase-rdb.lsh`

### HDB Writer (port 19965)
- Connects to RDB, periodically pulls new rows via high-water mark
- Flushes ticker and L2 data to date-partitioned columnar files on disk
- Flush interval: 60 seconds
- Data directory: `/datafast1/experiment/coinbasedata/`
- Entry point: `packages/libuv/scripts/coinbase-hdb-writer.lsh`

### HDB Reader (port 19962)
- Reads date-partitioned data from `/datafast1/experiment/coinbasedata/`
- Lazy-loads partitions into memory on first query
- Supports range queries across multiple dates
- Entry point: `packages/libuv/scripts/coinbase-hdb-reader.lsh`

### Analytics (port 19963)
- Connects to RDB, polls for new ticker rows every 1 second
- Computes per-product analytics over a sliding window (20 ticks):
  - Moving average (O(1) via running sum)
  - Bid-ask spread
  - Rolling volatility (running sum of squares)
  - Min/max price
  - TWAP (time-weighted average price)
  - VWAP (volume-weighted average price)
- Entry point: `packages/libuv/scripts/coinbase-analytics.lsh`

### Gateway (port 19964)
- Connects to all 4 backend services
- Smart query routing: auto-detects target from query pattern
- Explicit routing: `("rdb" sub-query)`, `("ana" sub-query)`, etc.
- Permissions support (localhost trusted by default)
- Entry point: `packages/libuv/scripts/coinbase-gateway.lsh`

## RDB Ticker Schema

| Column     | Type   | Source                                    |
|-----------|--------|-------------------------------------------|
| product   | string | Coinbase `product_id`                     |
| price     | real   | Coinbase `price`                          |
| bid       | real   | Coinbase `best_bid`                       |
| ask       | real   | Coinbase `best_ask`                       |
| side      | string | Coinbase `side`                           |
| volume    | real   | Coinbase `last_size` (trade volume)       |
| time      | stamp  | Coinbase `time` (broker timestamp, µs)    |
| local_time| stamp  | `(timestamp-now)` at receive (µs)         |

## HDB Disk Layout

```
/datafast1/experiment/coinbasedata/
  .ctrl/
    feed-handler.pid
    rdb.pid
    hdb-writer.pid
    hdb-reader.pid
    analytics.pid
    gateway.pid
  logs/
    feed-handler.log
    rdb.log
    hdb-writer.log
    hdb-reader.log
    analytics.log
    gateway.log
  2026.03.06/
    ticker/
      meta.lsh
      col-0.col (product codes)
      col-0-codes.col
      col-0-pool-str.bin
      col-1.col (price)
      col-2.col (bid)
      col-3.col (ask)
      col-4.col (side codes)
      col-5.col (volume)
      col-6.col (time)
      col-7.col (local_time)
    l2/
      meta.lsh
      col-0.col ... col-4.col
```

## Starting and Stopping

### Start
```bash
cd /path/to/lush-claude
bash packages/libuv/scripts/coinbase-start.sh
```

Processes start in dependency order with brief delays:
1. Feed Handler → 2. RDB → 3. HDB Writer, HDB Reader → 4. Analytics → 5. Gateway

### Stop
```bash
bash packages/libuv/scripts/coinbase-stop.sh
```

Sends SIGTERM in reverse order. Feed handler and RDB have signal handlers for graceful shutdown.

## Monitoring from REPL

```lisp
(libload "libuv/coinbase-monitor")

;; Check all services
(coinbase-status)
(coinbase-print-status)

;; Live VWAP
(coinbase-vwap "BTC-USD")       ;; → 67314.50

;; All analytics for a product
(coinbase-analytics "BTC-USD")  ;; → alist with mavg, spread, vwap, ...

;; Last trade details
(coinbase-last-trade "BTC-USD") ;; → alist with price, volume, time, local_time

;; Last N ticker rows as DataTable
(let ((dt (coinbase-last-n "ticker" 10)))
  (==> dt print-table))

;; RDB statistics
(coinbase-rdb-stats)

;; HDB date partitions and counts
(coinbase-hdb-count)
```

## Key Changes Made

### C Layer (`packages/json/json-c.c`)
- `lush_json_parse_ticker()` gains `double *volumes` parameter
- Extracts `last_size` field from Coinbase ticker JSON (string → strtod)

### Feed Handler (`packages/libuv/feed-handler.lsh`)
- Added `*fh-tk-volumes*` buffer
- Passes volumes to `json-parse-ticker`
- Includes `"volume"` in ticker broadcast alist

### RDB (`packages/libuv/rdb.lsh`)
- Added `"volume"` (real) and `"local_time"` (stamp) columns to ticker table
- `rdb-insert-ticker` extracts volume and stamps `(timestamp-now)`

### Analytics (`packages/libuv/analytics.lsh`)
- Tracks `sum-pv` and `sum-vol` per product (running sums for VWAP)
- `ana-update-product` accepts `volume` parameter
- Added `_ana-vwap` function: `sum_pv / sum_vol`
- Added `"vwap"` column to analytics table
- Added `"vwap"` and `"vwap-all"` query types

### New Files
- `packages/libuv/scripts/coinbase-feed.lsh` — Feed handler entry point
- `packages/libuv/scripts/coinbase-rdb.lsh` — RDB entry point
- `packages/libuv/scripts/coinbase-hdb-writer.lsh` — HDB writer entry point
- `packages/libuv/scripts/coinbase-hdb-reader.lsh` — HDB reader entry point
- `packages/libuv/scripts/coinbase-analytics.lsh` — Analytics entry point
- `packages/libuv/scripts/coinbase-gateway.lsh` — Gateway entry point
- `packages/libuv/scripts/coinbase-start.sh` — Launch all processes
- `packages/libuv/scripts/coinbase-stop.sh` — Stop all processes
- `packages/libuv/coinbase-monitor.lsh` — REPL convenience functions
