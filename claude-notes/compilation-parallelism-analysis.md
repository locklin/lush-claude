# Compilation Parallelism Analysis

Investigated March 2026. Conclusion: **not worth pursuing** -- the gains
are modest and would add fragility to the build pipeline.

## How Lush Compiles Code

Lush has two compilation pipelines, neither of which uses GNU make:

### DHC Pipeline (`dhc-make` / `dhc-make-with-libs`)

File: `lsh/compiler/dh-compile.lsh`

1. Generates a **single .c file** containing ALL functions passed to `dhc-make`
2. Compiles it to one `.o` via `sys("gcc ...")`
3. Links to one `.so` via `sys("gcc -shared ...")`
4. Loads via `mod-load`

No parallelism possible -- it's inherently one .c → one .o → one .so.

### LushMake Pipeline (`libc/make.lsh`)

Used by `*-config.lsh` files for vendored C libraries (sqlite, json, etc.).

- Class `LushMake` provides make-like dependency rules
- The `rebuild` method walks rules recursively, calling `sys("gcc ...")` one
  target at a time -- strictly sequential
- No Makefile is ever generated (except in the unused `make-standalone` path)

## Packages With Multiple Independent .o Files

These are the only packages where parallel compilation of .o files could help:

| Package    | Independent .o files | Notes |
|------------|---------------------|-------|
| sn28       | 11 files            | Biggest theoretical win |
| lasvm      | 4 files             | Moderate |
| sqlite     | 2 files             | sqlite3.o (261K lines) dominates; sqlite-c.o is tiny |
| json       | 2 files             | yyjson.o is large; json-c.o is tiny |
| jpeg       | 2 files             | Both small |

Single-file packages (no benefit): mapper, datatable, libuv, xgboost, zmq,
curl, wire, lz4, lap.

## Why It's Not Worth Doing

1. **LushMake's mtime caching already prevents unnecessary recompiles.**
   The slow first-time compile of sqlite3.c or yyjson.c only happens once.
   Subsequent loads skip compilation entirely if sources haven't changed.

2. **Most packages compile a single .o file.** Parallelism has zero benefit
   for the majority of the codebase.

3. **Where parallelism helps most (sn28, 11 files), the files are small.**
   The compile time per file is modest; the total wall time is acceptable.

4. **For sqlite/json, one large file dominates.** Compiling sqlite3.o and
   sqlite-c.o in parallel saves only the time to compile sqlite-c.c
   (< 1 second) while sqlite3.o takes 30+ seconds regardless.

5. **Linking is always serial.** The `gcc -shared` step can't be parallelized.

6. **Implementation options all add complexity:**
   - Generating temp Makefiles and invoking `make -j` -- adds GNU make as
     a runtime dependency for the Lush compiler, temp file management
   - Shell backgrounding (`cmd &` + `wait`) -- loses exit codes, error
     handling becomes fragile
   - Refactoring `rebuild` for async execution -- significant rework of
     the recursive dependency walker

## Other Considered Optimizations

- **ccache**: Would help with recompiles, but mtime checks already prevent
  most unnecessary recompiles. Marginal benefit.
- **Reducing -O2 to -O1 for vendored code**: sqlite3.c and yyjson.c are
  already internally optimized. Using -O1 would speed up their compilation
  but the compile only happens once per machine.
- **Splitting DHC output into multiple .c files**: Would require major
  refactoring of the DHC compiler. Generated code has shared type
  definitions and interdependencies.
