# Lush Programming Language

This is a lisp dialect with a powerful GUI, a helptool, a compilable subset, and the ability to compile and link C code embedded in the language. Its origins date back to the 1980s as SN (Simulateur Neuronal), making it one of the earliest neural network frameworks.

## Overview

Qwen is sandboxed on this machine and doesn't have global write permissions; never ask for access to directories you are not given access to.

## Important Documentation

### Helptool Documentation Format

**CRITICAL**: All new work in Lush packages should be well documented according to these directives. See `claude-notes/helptool-instructions.md` for the complete guide.

Key points:
- Use `#?` markers in `.lsh` source files to define help entries viewable via `(helptool)` or `(apropos)`
- **Only use valid brace tags** (see helptool-instructions.md for the complete list)
- Dot-tags are deprecated; all new entries must use brace-tag syntax exclusively
- Within a single entry body, use EITHER dot-tags OR brace-tags, NEVER both

### Claude's 64-bit Audit Report

Lush was originally a 32-bit system. Claude has mostly updated it to be 64-bit clean. See `claude-notes/completed/Claude-64bit-audit-report.md` for a comprehensive report of:
- Remaining 32-bit bugs in C source
- Tiered fixes for serialization truncation, size overflow, pointer truncation
- Implementation phases for completing the 64-bit cleanup

## Package Architecture

Lush packages are organized around `*-config.lsh` files that specify build dependencies and compilation rules. Key packages include:

### Core Infrastructure Packages

| Package | Purpose | Design Notes |
|---------|---------|--------------|
| **zmq** | ZeroMQ transport bindings for Lush | See `packages/zmq/design-notes.md` |
| **wire** | IPC package enabling multi-process communication over TCP sockets | See `packages/wire/design-notes.md` |
| **ltor** | TorQ-like database management system (uses ZMQ + Wire) | See `packages/ltor/design-notes.md` |
| **datatable** | Columnar table type with C-accelerated CSV I/O | See `packages/datatable/design-notes.md` |

### Database Layer

The **Database** class is the server-side query endpoint:
- Htable of name → DataTable
- `query` method accepts SQL strings
- Used by all LTOR data-holding backends (RDB, Analytics, HDB Writer, HDB Reader)
- See `claude-notes/database-query-design.md` for full design

### Machine Learning Packages

| Package | Purpose | Design Notes |
|---------|---------|--------------|
| **mlcore** | scikit-learn equivalent: ML primitives with compiled C hot-paths | See `packages/mlcore/design-notes.md` |
| **torch9** | libtorch (PyTorch) integration for GPU-accelerated deep learning | See `packages/torch9/design-notes.md` |

### External Library Management

| Package | Purpose | Design Notes |
|---------|---------|--------------|
| **lush-pkg** | Utility library for downloading, verifying, building, and installing external C libraries | See `packages/lush-pkg/design-notes.md` |

## Development Patterns

### 1. Helptool Documentation Style

```lisp
#? *** MyPackage: Widget Library
;; A library for creating and manipulating widgets.
;;
;; Features:
;; {<ul>
;; {<li> Create widgets with {<b> make-widget}.}
;; {<li> Resize with {<b> widget-resize}.}
;; {<li> Supports nested widget hierarchies.}
;; }

#? (make-widget <type> <w> <h>)
;; Create a new widget of the given <type> with dimensions <w> x <h>.
;; Returns an opaque widget handle.
(de make-widget (type w h) ...)
```

### 2. DHC (Dynamic C) Pattern

Embedded C code for performance-critical sections:

```lisp
(dhc-make ("mylib" "src/mylib.c")
  #{ 
    void my_c_function(int x) {
      // C code here
    }
  #})
```

### 3. Package Structure

A typical package has:
- `*-config.lsh` -- lushmake build configuration (compile/link rules)
- `*.lsh` -- main Lush code with embedded `#?` documentation
- `tests/` -- test suite files
- `design-notes.md` -- design decisions and implementation details

## Testing

Run tests for packages:
```bash
TMPDIR=/tmp/claude bin/lush packages/zmq/tests/run-all.lsh
TMPDIR=/tmp/claude bin/lush packages/wire/tests/run-all.lsh
TMPDIR=/tmp/claude bin/lush packages/datatable/tests/run-all.lsh
```

## Key Design Decisions (from Claude)

### Database as Query Server

The Database object is designed to be at the **end of a wire**:
- Not just a local convenience wrapper
- The server-side query endpoint for remote clients
- Each LTOR process type wraps a Database around its DataTables

See `claude-notes/database-query-design.md` for complete architecture.

### Package Management with lush-pkg

External C library dependencies are managed via `lush-pkg.lsh`:
- Decentralized specs (each package declares its own dependency inline)
- Per-user prefix (`~/.lush/local/`)
- Build from source at install time
- Version-safe linking via `-Wl,-rpath`

See `packages/lush-pkg/design-notes.md` for implementation details.

### 64-bit Migration Status

While `intg` (now mapped to `long`) is used in struct fields, many C functions still funnel values through 32-bit `int` temporaries. Claude's audit identified:
- **Tier 1**: Serialization truncation (data-corrupting) — add guards
- **Tier 2**: Size/Offset overflow (crash or wrong result) — fix all
- **Tier 3**: Pointer truncation (64-bit correctness) — fix LLP64 issues
- **Tier 4**: DH compiler code generation — update generated C code
- **Tier 5**: Lower risk — minor issues

## Reference: Package File Maps

### zmq Package
```
packages/zmq/
  zmq-c.h              # Minimal ZMQ API declarations
  zmq-c.c              # C bridge: handle table + wrapper functions
  zmq-config.lsh       # lushmake build configuration
  zmq.lsh              # Main loader: DHC wrappers + high-level API
  zmq-serde.lsh        # bwrite/bread serialization over ZMQ frames
```

### wire Package
```
packages/wire/
  wire.lsh             # Main module: protocol, server, client, WirePool
  wire-serialize.lsh   # DataTable pack/unpack (binary + sexp + compressed)
  wire_helpers.c       # DX functions: memstream, recv-nonblock
```

### torch9 Package
```
packages/torch9/
  torch9-config.lsh          # Backend detection, libtorch download
  torch9.lsh                 # ~149 DHC wrappers + ~121 high-level API functions
  lush_torch9_bridge.h       # extern "C" API declarations
  lush_torch9_bridge.cpp     # C++ implementations wrapping libtorch
```

### mlcore Package
```
packages/mlcore/
  mlcore.lsh           # Main loader (loads all sub-modules)
  model-selection.lsh  # Train/test split, k-fold CV, metrics
  feature-scaling.lsh  # StandardScaler, MinMaxScaler, RobustScaler
  regression.lsh       # lm, ridge, logistic, glmnet
  knn.lsh              # Brute/VP-tree/KD-tree/HNSW search
  naive-bayes.lsh      # GaussianNB, MultinomialNB, BernoulliNB
  rf.lsh               # RandomForest (classify/regress)
  kmeans.lsh           # k-means++ init, Lloyd's algorithm
  conformal.lsh        # Conformal prediction methods
```

### dtor Package (Database + Query Server)
```
packages/ltor/
  ltor-feed.lsh           # Feed handler: libuv WS + ZMQ PUB
  ltor-rdb.lsh            # RDB: SUB + ROUTER
  ltor-analytics.lsh      # Analytics: SUB + ROUTER
  ltor-hdb-writer.lsh     # HDB Writer: SUB + ROUTER
  ltor-hdb-reader.lsh     # HDB Reader: ROUTER only
  ltor-gateway.lsh        # Gateway: ROUTER frontend + DEALER backends
  coinbase-*.lsh          # Business logic (transport-agnostic)
```

## Additional Notes

### The idx Storage System

Lush's `idx`/`struct srg` abstraction directly became Torch7's THTensor/THStorage and PyTorch's ATen. The `packages/torch9/design-notes.md` explains the idx-to-Tensor bridge in detail.

### Zero-Copy Optimization (zmq)

For large messages (>4KB), zero-copy send avoids buffer copies by pinning the Lush buffer in a global variable to prevent GC. See `packages/zmq/design-notes.md:Zero-Copy Send`.

### Thread Safety

Lush is single-threaded. ZMQ sockets are not thread-safe, which is fine for this architecture.