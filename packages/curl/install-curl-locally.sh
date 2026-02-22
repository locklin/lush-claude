#!/usr/bin/env bash
#
# install-curl-locally.sh — Download and build curl 8.12.1 with WebSocket support
#
# Creates dist/ with lib/libcurl.{a,so} and include/curl/ headers needed
# by the Lush curl package. Run once before (libload "curl/curl").
#
# Usage:  cd packages/curl && ./install-curl-locally.sh

set -euo pipefail

CURL_VERSION="8.12.1"
CURL_TARBALL="curl-${CURL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/${CURL_TARBALL}"
CURL_SHA256="7b40ea64947e0b440716a4d7f0b7aa56230a5341c8377d7b609649d4aea8dbcf"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/curl-${CURL_VERSION}"
DIST_DIR="${SCRIPT_DIR}/dist"

# ---- helpers ----

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

check_deps() {
    local missing=()
    for cmd in gcc make curl; do
        command -v "$cmd" >/dev/null || missing+=("$cmd")
    done
    if (( ${#missing[@]} )); then
        die "Missing required tools: ${missing[*]}"
    fi
    # OpenSSL dev headers needed for TLS
    if ! pkg-config --exists openssl 2>/dev/null; then
        if [ ! -f /usr/include/openssl/ssl.h ]; then
            die "OpenSSL development headers not found (install libssl-dev or openssl-devel)"
        fi
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

if [ -f "${DIST_DIR}/lib/libcurl.so" ] && [ -f "${DIST_DIR}/include/curl/curl.h" ]; then
    info "dist/ already exists and looks complete. To rebuild, remove it first:"
    echo "    rm -rf ${DIST_DIR}"
    exit 0
fi

check_deps

cd "$SCRIPT_DIR"

# Download
if [ ! -f "$CURL_TARBALL" ]; then
    info "Downloading curl ${CURL_VERSION}..."
    curl -L -o "$CURL_TARBALL" "$CURL_URL"
else
    info "Using existing ${CURL_TARBALL}"
fi

info "Verifying checksum..."
verify_checksum "$CURL_TARBALL" "$CURL_SHA256"

# Extract
if [ ! -d "$SRC_DIR" ]; then
    info "Extracting..."
    tar xzf "$CURL_TARBALL"
fi

# Configure
info "Configuring curl ${CURL_VERSION} (WebSocket + OpenSSL, minimal protocols)..."
cd "$SRC_DIR"
./configure --prefix="$DIST_DIR" \
    --enable-websockets --with-openssl \
    --disable-ldap --disable-ldaps --disable-rtsp \
    --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smb \
    --disable-smtp --disable-gopher --disable-mqtt \
    --disable-manual --disable-docs \
    --without-brotli --without-zstd --without-libpsl \
    --without-libidn2 --without-nghttp2 \
    --silent

# Build and install
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
info "Building (${NPROC} jobs)..."
make -j"$NPROC" --silent

info "Installing to dist/..."
make install --silent

cd "$SCRIPT_DIR"

# Verify
if "${DIST_DIR}/bin/curl" --version | grep -q "ws wss"; then
    info "Success! curl ${CURL_VERSION} installed with WebSocket support."
    "${DIST_DIR}/bin/curl" --version | head -3
else
    die "Build completed but WebSocket protocols not found"
fi

# Cleanup hint
echo ""
info "You can now remove the source files to save space:"
echo "    rm -rf ${SRC_DIR} ${CURL_TARBALL}"
