#!/usr/bin/env bash
#
# install-xgboost-locally.sh — Download and build xgboost 3.2.0
#
# Creates dist/ with lib/libxgboost.so and include/xgboost/c_api.h
# needed by the Lush xgboost package. Run once before (libload "xgboost/xgboost").
#
# Usage:  cd packages/xgboost && ./install-xgboost-locally.sh

set -euo pipefail

XGB_VERSION="3.2.0"
XGB_TARBALL="xgboost-src-${XGB_VERSION}.tar.gz"
XGB_URL="https://github.com/dmlc/xgboost/releases/download/v${XGB_VERSION}/xgboost-src-${XGB_VERSION}.tar.gz"
XGB_SHA256="16a31dfbc0c54544c9c36ab5f696fa7b646c125f161c52c814d757a58241a404"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/xgboost-src-${XGB_VERSION}"
DIST_DIR="${SCRIPT_DIR}/dist"

# ---- helpers ----

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

check_deps() {
    local missing=()
    for cmd in gcc make cmake; do
        command -v "$cmd" >/dev/null || missing+=("$cmd")
    done
    if (( ${#missing[@]} )); then
        die "Missing required tools: ${missing[*]}"
    fi
}

verify_checksum() {
    local file="$1" expected="$2"
    local actual
    if command -v sha256sum >/dev/null; then
        actual=$(sha256sum "$file" | awk '{print $1}')
    elif command -v shasum >/dev/null; then
        actual=$(shasum -a 256 "$file" | awk '{print $1}')
    else
        info "Warning: no sha256 tool found, skipping checksum verification"
        return 0
    fi
    if [ "$actual" != "$expected" ]; then
        die "Checksum mismatch for ${file}\n  expected: ${expected}\n  got:      ${actual}"
    fi
}

# ---- main ----

if [ -f "${DIST_DIR}/lib/libxgboost.so" ] && [ -f "${DIST_DIR}/include/xgboost/c_api.h" ]; then
    info "dist/ already exists and looks complete. To rebuild, remove it first:"
    echo "    rm -rf ${DIST_DIR}"
    exit 0
fi

check_deps

cd "$SCRIPT_DIR"

# Download
if [ ! -f "$XGB_TARBALL" ]; then
    info "Downloading xgboost ${XGB_VERSION}..."
    curl -L -o "$XGB_TARBALL" "$XGB_URL"
else
    info "Using existing ${XGB_TARBALL}"
fi

info "Verifying checksum..."
verify_checksum "$XGB_TARBALL" "$XGB_SHA256"

# Extract
if [ ! -d "$SRC_DIR" ]; then
    info "Extracting..."
    tar xzf "$XGB_TARBALL"
fi

# Build with cmake
info "Building xgboost ${XGB_VERSION} with cmake..."
cd "$SRC_DIR"
mkdir -p build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$DIST_DIR" -DBUILD_STATIC_LIB=OFF
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
info "Compiling (${NPROC} jobs)..."
make -j"$NPROC"

info "Installing to dist/..."
make install

cd "$SCRIPT_DIR"

# Verify
if [ -f "${DIST_DIR}/lib/libxgboost.so" ]; then
    info "Success! xgboost ${XGB_VERSION} installed."
    ls -la "${DIST_DIR}/lib/libxgboost.so"
else
    die "Build completed but libxgboost.so not found"
fi

# Cleanup hint
echo ""
info "You can now remove the source files to save space:"
echo "    rm -rf ${SRC_DIR} ${XGB_TARBALL}"
