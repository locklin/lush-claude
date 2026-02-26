#!/usr/bin/env bash
#
# vendor-libuv.sh — Download libuv v1.49.2 and extract Linux-relevant source files
#
# Populates include/ and src/ with the ~49 files needed to build libuv
# as a shared library on Linux.  Run once before (libload "libuv/libuv").
#
# Usage:  cd packages/libuv && ./vendor-libuv.sh

set -euo pipefail

LIBUV_VERSION="1.49.2"
LIBUV_TARBALL="libuv-v${LIBUV_VERSION}.tar.gz"
LIBUV_URL="https://dist.libuv.org/dist/v${LIBUV_VERSION}/${LIBUV_TARBALL}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXTRACT_DIR="${SCRIPT_DIR}/libuv-v${LIBUV_VERSION}"

# ---- helpers ----

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

check_deps() {
    local missing=()
    for cmd in gcc make curl tar; do
        command -v "$cmd" >/dev/null || missing+=("$cmd")
    done
    if (( ${#missing[@]} )); then
        die "Missing required tools: ${missing[*]}"
    fi
}

# ---- file lists ----

# Public headers
INCLUDE_FILES=(
    "include/uv.h"
)

INCLUDE_UV_FILES=(
    "include/uv/errno.h"
    "include/uv/linux.h"
    "include/uv/posix.h"
    "include/uv/threadpool.h"
    "include/uv/tree.h"
    "include/uv/unix.h"
    "include/uv/version.h"
)

# Core source files (platform-independent)
SRC_FILES=(
    "src/fs-poll.c"
    "src/heap-inl.h"
    "src/idna.c"
    "src/idna.h"
    "src/inet.c"
    "src/queue.h"
    "src/random.c"
    "src/strscpy.c"
    "src/strscpy.h"
    "src/strtok.c"
    "src/strtok.h"
    "src/thread-common.c"
    "src/threadpool.c"
    "src/timer.c"
    "src/uv-common.c"
    "src/uv-common.h"
    "src/uv-data-getter-setters.c"
    "src/version.c"
)

# Unix/Linux source files
# Note: posix-poll.c, posix-hrtime.c, sysinfo-loadavg.c, and
# sysinfo-memory.c are NOT needed on Linux (linux.c provides these).
# random-devurandom.c IS needed (fallback called from random.c).
SRC_UNIX_FILES=(
    "src/unix/async.c"
    "src/unix/core.c"
    "src/unix/dl.c"
    "src/unix/fs.c"
    "src/unix/getaddrinfo.c"
    "src/unix/getnameinfo.c"
    "src/unix/internal.h"
    "src/unix/linux.c"
    "src/unix/loop-watcher.c"
    "src/unix/loop.c"
    "src/unix/pipe.c"
    "src/unix/poll.c"
    "src/unix/process.c"
    "src/unix/procfs-exepath.c"
    "src/unix/proctitle.c"
    "src/unix/random-devurandom.c"
    "src/unix/random-getrandom.c"
    "src/unix/random-sysctl-linux.c"
    "src/unix/signal.c"
    "src/unix/stream.c"
    "src/unix/tcp.c"
    "src/unix/thread.c"
    "src/unix/tty.c"
    "src/unix/udp.c"
)

# ---- main ----

if [ -f "${SCRIPT_DIR}/include/uv.h" ] && [ -f "${SCRIPT_DIR}/src/unix/linux.c" ]; then
    info "Source files already present. To re-vendor, remove include/ and src/ first:"
    echo "    rm -rf ${SCRIPT_DIR}/include ${SCRIPT_DIR}/src"
    exit 0
fi

check_deps

cd "$SCRIPT_DIR"

# Download
if [ ! -f "$LIBUV_TARBALL" ]; then
    info "Downloading libuv v${LIBUV_VERSION}..."
    curl -L -o "$LIBUV_TARBALL" "$LIBUV_URL"
else
    info "Using existing ${LIBUV_TARBALL}"
fi

# Extract full tarball to temp location
if [ ! -d "$EXTRACT_DIR" ]; then
    info "Extracting..."
    tar xzf "$LIBUV_TARBALL"
fi

# Verify extraction produced expected directory
if [ ! -f "${EXTRACT_DIR}/include/uv.h" ]; then
    die "Extraction failed: ${EXTRACT_DIR}/include/uv.h not found"
fi

# Create target directories
mkdir -p "${SCRIPT_DIR}/include/uv"
mkdir -p "${SCRIPT_DIR}/src/unix"

# Copy files
info "Copying public headers..."
for f in "${INCLUDE_FILES[@]}"; do
    cp "${EXTRACT_DIR}/${f}" "${SCRIPT_DIR}/${f}"
done
for f in "${INCLUDE_UV_FILES[@]}"; do
    cp "${EXTRACT_DIR}/${f}" "${SCRIPT_DIR}/${f}"
done

info "Copying core source files..."
for f in "${SRC_FILES[@]}"; do
    cp "${EXTRACT_DIR}/${f}" "${SCRIPT_DIR}/${f}"
done

info "Copying unix/linux source files..."
for f in "${SRC_UNIX_FILES[@]}"; do
    cp "${EXTRACT_DIR}/${f}" "${SCRIPT_DIR}/${f}"
done

# Count files
TOTAL=$(( ${#INCLUDE_FILES[@]} + ${#INCLUDE_UV_FILES[@]} + ${#SRC_FILES[@]} + ${#SRC_UNIX_FILES[@]} ))
info "Vendored ${TOTAL} files from libuv v${LIBUV_VERSION}"

# Verify key files
for check in "include/uv.h" "src/uv-common.c" "src/unix/linux.c" "src/unix/core.c"; do
    if [ ! -f "${SCRIPT_DIR}/${check}" ]; then
        die "Verification failed: ${check} not found"
    fi
done

info "Success! libuv v${LIBUV_VERSION} source vendored."

# Cleanup hint
echo ""
info "You can remove the extracted source to save space:"
echo "    rm -rf ${EXTRACT_DIR} ${LIBUV_TARBALL}"
