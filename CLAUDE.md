# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Lush (Lisp Universal Shell) — an object-oriented Lisp interpreter/compiler from the 1980s (ancestor of Torch/PyTorch). Features a compilable strongly-typed subset, embedded C/C++ code, GUI toolkit (ogre), and literate documentation system (helptool). Originally 32-bit; ongoing 64-bit cleanup.

## Build & Run

```bash
./configure && make world     # Full build: compile + lush binary + stdenv.dump
make test                     # Quick test suite (tests/run-all.lsh --quick)
make test-large               # Full test suite including large-dimension tests
make cleanse                  # Remove all C/ directories (stale compiled artifacts)
```

The compiled binary is at `bin/x86_64-unknown-linux-gnu/lush`. The system-installed binary is at `/usr/local/bin/lush`.

### Running Package Tests

```bash
TMPDIR=/tmp/claude bin/lush packages/datatable/tests/run-all.lsh   # 868 tests
TMPDIR=/tmp/claude bin/lush packages/wire/tests/run-all.lsh        # 193 tests
TMPDIR=/tmp/claude bin/lush packages/zmq/tests/run-all.lsh
```

### Test Framework

Tests use `tests/framework.lsh`:

```lisp
(libload "tests/framework.lsh")
(test-suite "My Tests"
  (test-equal "name" (+ 1 1) 2)
  (test-check "truthy" t)
  (test-near "approx" 3.14 3.14159 0.01)
  (test-str-equal "strings" "hello" "hello"))
(test-summary)  ;; prints results, returns 0 (pass) or 1 (fail)
```

Bypass-wire test pattern: set `*xxx-no-autostart*` to suppress autostart, then call handlers directly without IPC.

## Architecture

### Directory Layout

- `src/` — C source for the Lush interpreter (~94 .c files)
- `sys/` — Core runtime (stdenv.dump, stdenv.lsh) — Lush cannot run without this
- `lsh/` — Standard library: libstd/, libc/, libidx/, libogre/, libgraph/, compiler/
- `packages/` — Extension packages (47 packages)
- `claude-notes/` — Design docs and work logs; `completed/` has finished audit reports

### Compilation Pipeline (Two Stages)

1. **dhc (Dynamic Hash Compiler)**: Compiles strongly-typed Lush → C, generates `C/<name>.c`, auto-caches `.so` in `C/`
2. **lushmake**: Compiles manual C files via `(new lushmake)` rules, produces `.o` and `.so` files

**Critical**: Stale `.c`/`.so` files in `packages/*/C/` cause confusing errors. Run `make cleanse` to fix.

### Package Structure

Every package follows this pattern:
- `<name>-config.lsh` — lushmake build configuration (compile/link rules)
- `<name>.lsh` — Main Lush code loaded by users via `(libload "pkgname/pkgname")`
- `<name>-c.c` / `<name>-c.h` — C bridge code
- `tests/run-all.lsh` — Test runner
- `design-notes.md` — Architecture documentation

### Key Packages

**lush-pkg** (`packages/lush-pkg/`): Package manager for external C libraries. Per-user prefix (`~/.lush/local/`), builds from source, version-safe linking via `-Wl,-rpath`. Each package declares its own dependency inline in `*-config.lsh` using `(lush-pkg-ensure name spec)`.

**datatable** (`packages/datatable/`): Columnar table storage (~12K lines). In-memory operations (query, join, groupby, multiindex, compiled hot-paths) via `(libload "datatable/datatable")`. Persistence layer (save, load, mmap, append, compress, partitions) via `(libload "datatable/columnardb")`.

**wire** (`packages/wire/`): IPC protocol (~1.6K lines). 16-byte binary header (magic "LWIR", version, type, encoding, msgID, payload length). `select()`-based event loop. Localhost-only by default.

**ltor** (`packages/ltor/`): TorQ-inspired real-time data pipeline. Feed Handler → RDB → HDB Writer → HDB Reader → Analytics → Gateway. All use DataTable + Database wrapper for SQL queries, connected via ZMQ + Wire.

## Helptool Documentation Format

**CRITICAL**: All new package code must include helptool documentation. Full rules in `claude-notes/helptool-instructions.md`.

```lisp
#? *** MyPackage: Widget Library
;; A library for creating widgets.
;; {<ul>
;; {<li> Create widgets with {<b> make-widget}.}
;; {<li> Resize with {<b> widget-resize}.}
;; }

#? (make-widget <type> <w> <h>)
;; Create a new widget of the given <type> with dimensions <w> x <h>.
(de make-widget (type w h) ...)
```

- Hierarchy: `#? ****` (book) → `#? ***` (section) → `#? **` (subsection) → `#? *` (sub-subsection)
- Body: consecutive `;;` comment lines after `#?` marker
- **Only valid brace tags**: `p`, `pre`, `code`, `li`, `br`, `tt`, `b`, `i`, `u`, `c`, `font`, `center`, `img`, `ul`, `div`, `h1`-`h3`, `hlink`, `see`, `if-html`, `if-latex`, `if-ogre`, `if-text`, `lit`, `meta`, `ex`
- **Do NOT use**: `<ol>`, `<table>`, `<span>`, `<em>` (causes "illegal tag" errors)
- Always wrap `{<li>}` items in explicit `{<ul>}` blocks
- Dot-tags (`;;.PP`, `;;.SEE`) are deprecated — use brace-tags only in new code
- Never mix dot-tags and brace-tags within a single entry

## Lush Language Gotchas

- `t` is a reserved boolean literal — never use as a variable name
- No `regex-quote` builtin — use alternative string escaping
- `for*` is not valid — use `for`
- `mid` (substring) uses 1-based indexing
- `cadddr`/`cddddr` ARE defined in wire.lsh — do not replace them
- `boundp`, `return`, `objectp`, `is-a`, `ceil` may not be builtins — verify before using
- Zero-length matrices may not be supported
- `error` syntax differs from other Lisps — verify calling convention
- Valid string escapes: `\t`, `\n`, `\r`, `\b`, `\e`, `\\`, `\"` — `\!` is NOT valid
- `find-static-library` does NOT auto-prepend "lib" — use `"libhtk"` not `"htk"`
- `static-library-path` must be explicitly set (lush-pkg handles this)
- `dhc-make-with-libs` caches .so; stale files in `C/` cause confusing errors

## Environment & Sandbox

- This environment uses blaude/bubblewrap sandboxing — network access is restricted, cannot kill bwrap processes
- When editing files, verify you are editing the ACTIVE copy (system install vs workspace) before making changes
- Data directories default to `/datafast1/experiment/` paths — always confirm the correct path

## Working Conventions

- When asked to write a plan, make it CONCRETE: exact function signatures, file paths, code snippets, and specific line-level changes. Vague plans will be rejected.
- When eliminating a package, FULLY DELETE — no shims, no backwards-compatibility wrappers. Update ALL documentation references.
- Always verify actual dependency chains in code before claiming something can be made optional.
- Always run the full test suite after implementation changes — do not assume tests pass.
- When tests fail, read the ACTUAL error output carefully before guessing at fixes.
