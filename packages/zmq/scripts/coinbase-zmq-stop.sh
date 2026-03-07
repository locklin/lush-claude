#!/bin/bash
# coinbase-zmq-stop.sh -- Stop all ZMQ Coinbase pipeline processes
#
# Sends SIGTERM to all processes tracked in the control directory,
# then verifies they've stopped.
#
# Usage:
#   bash packages/zmq/scripts/coinbase-zmq-stop.sh

DATA_DIR=/datafast1/experiment/coinbasedata-zmq
CTRL_DIR=$DATA_DIR/.ctrl

if [ ! -d "$CTRL_DIR" ]; then
    echo "No control directory found at $CTRL_DIR"
    echo "ZMQ pipeline may not be running."
    exit 0
fi

echo "=== ZMQ Coinbase Pipeline Shutdown ==="
echo ""

# Stop in reverse order: gateway first, feed handler last
PROCESSES="gateway analytics hdb-reader hdb-writer rdb feed-handler"

for proc in $PROCESSES; do
    PID_FILE="$CTRL_DIR/$proc.pid"
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo -n "Stopping $proc (PID=$PID)... "
            kill "$PID"
            echo "SIGTERM sent"
        else
            echo "$proc (PID=$PID) already stopped"
        fi
        rm -f "$PID_FILE"
    else
        echo "$proc: no PID file"
    fi
done

# Wait for processes to exit
echo ""
echo -n "Waiting for processes to exit..."
sleep 2

# Check for stragglers and SIGKILL if needed
for proc in $PROCESSES; do
    PID_FILE="$CTRL_DIR/$proc.pid"
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "WARNING: $proc (PID=$PID) still running, sending SIGKILL"
            kill -9 "$PID"
        fi
    fi
done

echo " done"
echo ""
echo "=== All ZMQ processes stopped ==="
