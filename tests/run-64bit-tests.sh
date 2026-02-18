#!/bin/sh
# Run the 64-bit diagnostic tests for Lush.
# Warnings are printed to stderr; test output to stdout.
#
# Usage:
#   ./tests/run-64bit-tests.sh
#
# The script should be run from the lush root directory.

LUSH_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LUSH="$LUSH_DIR/bin/lush"

if [ ! -x "$LUSH" ]; then
    echo "ERROR: lush binary not found at $LUSH"
    echo "Run 'make' first to build lush."
    exit 1
fi

echo "Running 64-bit diagnostic tests..."
echo "Stderr output (warnings) will appear inline."
echo "-------------------------------------------"

echo "(load \"$LUSH_DIR/tests/test-64bit-warnings.lsh\") (exit 0)" | "$LUSH" 2>&1

echo "-------------------------------------------"
echo "Done."
