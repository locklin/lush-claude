#!/bin/bash
# coinbase-zmq-start.sh -- Launch all ZMQ Coinbase pipeline processes
#
# Starts 6 processes with nohup, writes PIDs to control directory.
# Must be run from the lush-claude root directory.
#
# This is the ZMQ pipeline on ports 19970-19976 — runs independently
# of the libuv pipeline on ports 19960-19965.
#
# Usage:
#   cd /path/to/lush-claude
#   bash packages/zmq/scripts/coinbase-zmq-start.sh
#
# To stop:
#   bash packages/zmq/scripts/coinbase-zmq-stop.sh

set -e

LUSH=./src/lush
DATA_DIR=/datafast1/experiment/coinbasedata-zmq
CTRL_DIR=$DATA_DIR/.ctrl
LOG_DIR=$DATA_DIR/logs
SCRIPT_DIR=packages/zmq/scripts

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
        echo "ERROR: ZMQ pipeline already running (feed-handler PID=$OLD_PID)"
        echo "Run coinbase-zmq-stop.sh first."
        exit 1
    fi
fi

echo "=== ZMQ Coinbase Pipeline Launcher ==="
echo "Data directory: $DATA_DIR"
echo ""

# Port assignments
echo "Port assignments:"
echo "  Feed Handler PUB:    19970"
echo "  RDB:                 19971"
echo "  HDB Reader:          19972"
echo "  Analytics:           19973"
echo "  Gateway:             19974"
echo "  HDB Writer:          19975"
echo "  Feed Handler CTRL:   19976"
echo ""

# 1. Feed Handler (must start first — others subscribe to it)
echo -n "Starting feed handler... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-feed.lsh" > "$LOG_DIR/feed-handler.log" 2>&1 &
echo $! > "$CTRL_DIR/feed-handler.pid"
echo "PID=$! (pub=19970 ctrl=19976)"

# Wait for feed handler to bind
sleep 2

# 2. RDB (subscribes to feed handler)
echo -n "Starting RDB... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-rdb.lsh" > "$LOG_DIR/rdb.log" 2>&1 &
echo $! > "$CTRL_DIR/rdb.pid"
echo "PID=$! (port 19971)"

# 3. HDB Writer (subscribes directly to feed handler)
echo -n "Starting HDB writer... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-hdb-writer.lsh" > "$LOG_DIR/hdb-writer.log" 2>&1 &
echo $! > "$CTRL_DIR/hdb-writer.pid"
echo "PID=$! (port 19975)"

# 4. HDB Reader (reads from disk, no upstream dependency)
echo -n "Starting HDB reader... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-hdb-reader.lsh" > "$LOG_DIR/hdb-reader.log" 2>&1 &
echo $! > "$CTRL_DIR/hdb-reader.pid"
echo "PID=$! (port 19972)"

# 5. Analytics (subscribes directly to feed handler)
echo -n "Starting analytics... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-analytics.lsh" > "$LOG_DIR/analytics.log" 2>&1 &
echo $! > "$CTRL_DIR/analytics.pid"
echo "PID=$! (port 19973)"

# Wait for backends to be ready
sleep 2

# 6. Gateway (connects to all backends)
echo -n "Starting gateway... "
nohup $LUSH "$SCRIPT_DIR/coinbase-zmq-gateway.lsh" > "$LOG_DIR/gateway.log" 2>&1 &
echo $! > "$CTRL_DIR/gateway.pid"
echo "PID=$! (port 19974)"

echo ""
echo "=== All ZMQ processes launched ==="
echo ""
echo "Logs: $LOG_DIR/"
echo "PIDs: $CTRL_DIR/"
echo ""
echo "Monitor from Lush REPL:"
echo "  (libload \"zmq/zmq-monitor\")"
echo "  (zmq-coinbase-status)"
echo "  (zmq-coinbase-vwap \"BTC-USD\")"
echo ""
echo "Stop all:"
echo "  bash packages/zmq/scripts/coinbase-zmq-stop.sh"
