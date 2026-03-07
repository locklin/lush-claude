# Lush Core Test Harness -- Design Notes

## Architecture

Pure-Lush test framework with no external dependencies. Each suite file can
run independently, but the standard entry point is `run-all.lsh` which loads
all suites in order and exits non-zero on any failure.

**Current state:** 324 tests, 1 skipped (the `int-matrix 0` skip in
suite-storage; Lush throws "Cannot compute storage size" for zero-length
matrices).

### File Layout

```
packages/lush-core-tests/
  framework.lsh             -- assertion macros and counters
  run-all.lsh               -- master runner (loads framework + all suites)
  suite-storage.lsh         -- storage creation, element size, allocation
  suite-matrix.lsh          -- matrix creation, dims, element access (all types)
  suite-string.lsh          -- string operations
  suite-htable.lsh          -- hash table operations
  suite-copy.lsh            -- copy-matrix for all type combinations
  suite-matrix-io.lsh       -- save/load/mmap round-trips (.MAT and binary)
  suite-compiled-basic.lsh  -- compiled scalar arithmetic (int, long, flt, real)
  suite-compiled-idx.lsh    -- compiled idx-bloop for all matrix types
  suite-compiled-class.lsh  -- compiled class creation and method dispatch
  suite-i64.lsh             -- ST_I64 / long-matrix comprehensive tests
  test-64bit.lsh            -- thin shim that loads framework + suite-i64
  C/                        -- C helper sources for compiled suites
```

### Execution Order

`run-all.lsh` loads suites in this order:

1. Interpreted suites (no compilation): storage, matrix, string, htable,
   matrix-io, copy
2. Compiled suites (require dhc-make): compiled-basic, compiled-idx,
   compiled-class
3. 64-bit regression suite: i64

## API Summary

All assertions live in `framework.lsh`. Global counters track pass/fail/skip.

| Function | Purpose |
|----------|---------|
| `(test-check desc expr)` | PASS if `expr` is truthy, FAIL otherwise |
| `(test-equal desc actual expected)` | PASS if `(= actual expected)` |
| `(test-approx desc actual expected tol)` | PASS if `abs(actual - expected) < tol` |
| `(test-str-equal desc actual expected)` | PASS if strings are `=` |
| `(test-skip desc reason)` | Record a skipped test with reason |
| `(test-error desc . body)` | PASS if body signals an error (macro) |
| `(test-suite name . body)` | Prints `--- name ---` header, runs body |
| `(test-summary)` | Prints totals; returns 0 on all-pass, 1 otherwise |

### Globals

- `*test-pass-count*`, `*test-fail-count*`, `*test-skip-count*` -- integer
  counters, zeroed at load time.

## Implementation Details

### test-suite

Defined as a `dmd` (macro) that splices body forms into a `progn` after a
`printf` header. No dynamic binding of `*test-current-suite*` -- the original
design doc proposed a `de` with `each`/`eval`, but the macro approach avoids
the overhead and works correctly with Lush scoping.

### test-error

Uses `on-error` to catch signals. Records PASS inside the error handler,
then the error propagates to the toplevel handler. If no error is raised,
the code falls through to record FAIL. Because error propagation exits the
enclosing block, `test-error` calls should be placed **last** in a
`test-suite` block, or isolated in their own block.

### Compiled suites

`suite-compiled-basic.lsh`, `suite-compiled-idx.lsh`, and
`suite-compiled-class.lsh` use `dhc-make` to compile functions and classes
before running assertions. This exercises the full dhc pipeline:
`.c -> gcc -fPIC -c -> .o -> gcc -shared -> .so -> mod-load`.

### Matrix I/O round-trips

`suite-matrix-io.lsh` writes matrices to `/tmp/claude/`, reloads them, and
verifies dimensions and element values are preserved. Covers all numeric
storage types including long-matrix (ST_I64).

## Known Issues / Limitations

1. **`test-error` propagation**: The `on-error` handler records PASS but
   cannot suppress the error. The error still propagates, so subsequent
   tests in the same `test-suite` block are skipped. Always place
   `test-error` last in a block or wrap it in its own `test-suite`.

2. **`int-matrix 0` unsupported**: Lush cannot create zero-length matrices
   (throws "Cannot compute storage size"). This is tested with `test-skip`.

3. **No module-load or module-deps suites**: The original design planned
   `suite-module-load.lsh` and `suite-module-deps.lsh` for testing .so
   loading and dependency resolution. These were never implemented; module
   loading is exercised indirectly through the compiled suites.

4. **No compiled-interop suite**: The planned `suite-compiled-interop.lsh`
   (compiled-to-interpreted interoperation) was not implemented.

5. **`(-int-)` / I32STORAGE type mismatch on 64-bit**: `-int-` maps to
   `intg` (64-bit) but I32STORAGE is 32-bit. This only affects Lush-level
   compiled matrix element access like `(m i)`. C-inline code with explicit
   `IDX_PTR($var, int)` bypasses the issue. See
   `claude-notes/completed/Claude-64bit-audit-report.md`.
