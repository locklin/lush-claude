#!/bin/bash
# coinbase-start.sh -- Launch all Coinbase pipeline processes
#
# Starts 6 processes with nohup, writes PIDs to control directory.
# Must be run from the lush-claude root directory.
#
# Usage:
#   cd /path/to/lush-claude
#   bash packages/libuv/scripts/coinbase-start.sh
#
# To stop:
#   bash packages/libuv/scripts/coinbase-stop.sh

set -e

LUSH=./src/lush
DATA_DIR=/datafast/experiment/coinbasedata
CTRL_DIR=$DATA_DIR/.ctrl
LOG_DIR=$DATA_DIR/logs
SCRIPT_DIR=packages/libuv/scripts

# Verify we're in the right directory
if [ ! -x "$LUSH" ]; then
    echo "ERROR: $LUSH not found. Run from lush-claude root directory."
    exit 1
fi

# Create directories
mkdir -p "$CTRL_DIR" "$LOG_DIR" "$DATA_DIR"

# Check for existing processes
if [ -f "$CTRL_DIR/feed-handler.pid" ]; then
    OLD_PID=$(cat "$CTRL_DIR/feed-handler.pid")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "ERROR: Pipeline already running (feed-handler PID=$OLD_PID)"
        echo "Run coinbase-stop.sh first."
        exit 1
    fi
fi

echo "=== Coinbase Pipeline Launcher ==="
echo "Data directory: $DATA_DIR"
echo ""

# Port assignments
echo "Port assignments:"
echo "  Feed Handler:  19960"
echo "  RDB:           19961"
echo "  HDB Reader:    19962"
echo "  Analytics:     19963"
echo "  Gateway:       19964"
echo "  HDB Writer:    19965"
echo ""

# 1. Feed Handler (must start first — others connect to it)
echo -n "Starting feed handler... "
nohup $LUSH "$SCRIPT_DIR/coinbase-feed.lsh" > "$LOG_DIR/feed-handler.log" 2>&1 &
echo $! > "$CTRL_DIR/feed-handler.pid"
echo "PID=$! (port 19960)"

# Wait for feed handler to bind
sleep 2

# 2. RDB (connects to feed handler)
echo -n "Starting RDB... "
nohup $LUSH "$SCRIPT_DIR/coinbase-rdb.lsh" > "$LOG_DIR/rdb.log" 2>&1 &
echo $! > "$CTRL_DIR/rdb.pid"
echo "PID=$! (port 19961)"

# Wait for RDB to bind
sleep 2

# 3. HDB Writer (connects to RDB)
echo -n "Starting HDB writer... "
nohup $LUSH "$SCRIPT_DIR/coinbase-hdb-writer.lsh" > "$LOG_DIR/hdb-writer.log" 2>&1 &
echo $! > "$CTRL_DIR/hdb-writer.pid"
echo "PID=$! (port 19965)"

# 4. HDB Reader (reads from disk, no upstream dependency)
echo -n "Starting HDB reader... "
nohup $LUSH "$SCRIPT_DIR/coinbase-hdb-reader.lsh" > "$LOG_DIR/hdb-reader.log" 2>&1 &
echo $! > "$CTRL_DIR/hdb-reader.pid"
echo "PID=$! (port 19962)"

# 5. Analytics (connects to RDB)
echo -n "Starting analytics... "
nohup $LUSH "$SCRIPT_DIR/coinbase-analytics.lsh" > "$LOG_DIR/analytics.log" 2>&1 &
echo $! > "$CTRL_DIR/analytics.pid"
echo "PID=$! (port 19963)"

# Wait for backends to be ready
sleep 2

# 6. Gateway (connects to RDB, HDB reader, analytics, HDB writer)
echo -n "Starting gateway... "
nohup $LUSH "$SCRIPT_DIR/coinbase-gateway.lsh" > "$LOG_DIR/gateway.log" 2>&1 &
echo $! > "$CTRL_DIR/gateway.pid"
echo "PID=$! (port 19964)"

echo ""
echo "=== All processes launched ==="
echo ""
echo "Logs: $LOG_DIR/"
echo "PIDs: $CTRL_DIR/"
echo ""
echo "Monitor from Lush REPL:"
echo "  (libload \"libuv/coinbase-monitor\")"
echo "  (coinbase-status)"
echo "  (coinbase-vwap \"BTC-USD\")"
echo ""
echo "Stop all:"
echo "  bash packages/libuv/scripts/coinbase-stop.sh"
