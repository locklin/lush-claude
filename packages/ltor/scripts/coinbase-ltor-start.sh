#!/bin/bash
# coinbase-ltor-start.sh -- Launch all LTOR Coinbase pipeline processes
#
# Starts 6 processes as background jobs, writes PIDs to control directory.
# Must be run from the lush-claude root directory.
#
# In sandboxed environments (Claude Code), all processes must run within
# the same bash session to share a network namespace.  The script uses
# heredoc-based Lush invocations instead of nohup + script files.
#
# Usage:
#   cd /path/to/lush-claude
#   bash packages/ltor/scripts/coinbase-ltor-start.sh
#
# To stop:
#   bash packages/ltor/scripts/coinbase-ltor-stop.sh
#
# To run in sandbox (background, keeps parent alive):
#   Use run_in_background with this script, then the `wait` at the end
#   keeps the parent process alive so child processes survive.

set -e

# lush-pkg stores built libraries under $HOME/.lush/local;
# in sandboxed environments /home/scott is read-only, so point HOME
# to the writable /tmp/claude tree where lush-pkg previously built libuv/curl.
export HOME=${LUSH_HOME:-/tmp/claude}

LUSH=./src/lush
DATA_DIR=/datafast1/experiment/coinbasedata-zmq
CTRL_DIR=$DATA_DIR/.ctrl
LOG_DIR=$DATA_DIR/logs

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
        echo "ERROR: LTOR pipeline already running (feed-handler PID=$OLD_PID)"
        echo "Run coinbase-ltor-stop.sh first."
        exit 1
    fi
fi

echo "=== LTOR Coinbase Pipeline Launcher ==="
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

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

# 1. Feed Handler (must start first — others subscribe to it)
echo -n "Starting feed handler... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/feed-handler.log"
stdbuf -oL $LUSH <<'FH_EOF' >> "$LOG_DIR/feed-handler.log" 2>&1 &
(libload "ltor/ltor-feed")
(ltor-fh-start '("BTC-USD" "ETH-USD") '("ticker" "heartbeat") 19970 19976)
FH_EOF
echo $! > "$CTRL_DIR/feed-handler.pid"
echo "PID=$! (pub=19970 ctrl=19976)"

# Wait for feed handler to bind
sleep 3

# 2. RDB (subscribes to feed handler)
echo -n "Starting RDB... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/rdb.log"
stdbuf -oL $LUSH <<'RDB_EOF' >> "$LOG_DIR/rdb.log" 2>&1 &
(libload "ltor/ltor-rdb")
(ltor-rdb-start 19970 19971)
RDB_EOF
echo $! > "$CTRL_DIR/rdb.pid"
echo "PID=$! (port 19971)"

# 3. HDB Writer (subscribes directly to feed handler)
echo -n "Starting HDB writer... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/hdb-writer.log"
stdbuf -oL $LUSH <<'HDBW_EOF' >> "$LOG_DIR/hdb-writer.log" 2>&1 &
(libload "ltor/ltor-hdb-writer")
(ltor-hdbw-start 19970 19975 "/datafast1/experiment/coinbasedata-zmq" 60)
HDBW_EOF
echo $! > "$CTRL_DIR/hdb-writer.pid"
echo "PID=$! (port 19975)"

# 4. HDB Reader (reads from disk, no upstream dependency)
echo -n "Starting HDB reader... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/hdb-reader.log"
stdbuf -oL $LUSH <<'HDBR_EOF' >> "$LOG_DIR/hdb-reader.log" 2>&1 &
(libload "ltor/ltor-hdb-reader")
(ltor-hdbr-start 19972 "/datafast1/experiment/coinbasedata-zmq")
HDBR_EOF
echo $! > "$CTRL_DIR/hdb-reader.pid"
echo "PID=$! (port 19972)"

# 5. Analytics (subscribes directly to feed handler)
echo -n "Starting analytics... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/analytics.log"
stdbuf -oL $LUSH <<'ANA_EOF' >> "$LOG_DIR/analytics.log" 2>&1 &
(libload "ltor/ltor-analytics")
(ltor-ana-start 19970 19973 20)
ANA_EOF
echo $! > "$CTRL_DIR/analytics.pid"
echo "PID=$! (port 19973)"

# Wait for backends to be ready
sleep 3

# 6. Gateway (connects to all backends)
echo -n "Starting gateway... "
echo "=== started $TIMESTAMP ===" >> "$LOG_DIR/gateway.log"
stdbuf -oL $LUSH <<'GW_EOF' >> "$LOG_DIR/gateway.log" 2>&1 &
(libload "ltor/ltor-gateway")
(ltor-gw-start 19974 19971 19972 19973 19975)
GW_EOF
echo $! > "$CTRL_DIR/gateway.pid"
echo "PID=$! (port 19974)"

echo ""
echo "=== All LTOR processes launched ==="
echo ""
echo "Logs: $LOG_DIR/"
echo "PIDs: $CTRL_DIR/"
echo ""
echo "Monitor from Lush REPL:"
echo "  (libload \"ltor/ltor-monitor\")"
echo "  (ltor-coinbase-status)"
echo "  (ltor-coinbase-vwap \"BTC-USD\")"
echo ""
echo "Stop all:"
echo "  bash packages/ltor/scripts/coinbase-ltor-stop.sh"
