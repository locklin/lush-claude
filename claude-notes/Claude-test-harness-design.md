# Lush Test Harness Design

## Purpose

A comprehensive testing framework written in Lush itself, designed to:

1. Validate all data types (including the new ST_I64/long) work correctly end-to-end
2. Provide regression coverage for the module loader modernization
3. Serve as living documentation of Lush's type system and compilation behavior
4. Be extensible for future changes

## Design Principles

- **Pure Lush:** Written entirely in Lush, no external dependencies
- **Self-contained:** Each test file can run independently
- **Structured output:** Machine-parseable PASS/FAIL output with summary counts
- **Grouped:** Tests organized into suites by topic
- **Compiled + Interpreted:** Tests that exercise both paths explicitly

## Framework API

The framework is minimal — just a few functions and a counter:

```scheme
;; Core test assertion
(test-check "description" <expression>)
;; → Evaluates expression. If truthy: PASS. If nil/error: FAIL.

;; Equality test
(test-equal "description" <actual> <expected>)
;; → Tests (= actual expected)

;; String equality
(test-str-equal "description" <actual> <expected>)
;; → Tests (= actual expected) for strings

;; Expected error
(test-error "description" <expression>)
;; → Expects expression to signal an error. PASS if error, FAIL if no error.

;; Suite grouping
(test-suite "Storage Types"
   ... tests ...)

;; Summary
(test-summary)
;; → Prints total pass/fail counts, returns exit code
```

## Test Organization

```
tests/
  framework.lsh          — The test framework itself
  run-all.lsh            — Master runner that loads all suites

  ;; Core Type Tests
  suite-storage.lsh      — Storage types: creation, malloc, size, access
  suite-matrix.lsh       — Matrix operations: creation, element access, dimensions
  suite-matrix-io.lsh    — Matrix save/load/mmap round-trips (.MAT and binary)
  suite-copy.lsh         — copy-matrix for all type combinations
  suite-string.lsh       — String operations
  suite-htable.lsh       — Hash table operations

  ;; Compiled Code Tests
  suite-compiled-basic.lsh    — Basic compiled functions (all scalar types)
  suite-compiled-idx.lsh      — Compiled idx operations (all matrix types)
  suite-compiled-class.lsh    — Compiled classes and methods
  suite-compiled-interop.lsh  — Compiled↔interpreted interoperation

  ;; Module System Tests
  suite-module-load.lsh       — Module loading and symbol resolution
  suite-module-deps.lsh       — Module dependencies

  ;; 64-bit Specific Tests
  suite-i64.lsh              — ST_I64 / long-matrix comprehensive tests
```

## Framework Implementation

```scheme
;; tests/framework.lsh

(defvar *test-pass-count* 0)
(defvar *test-fail-count* 0)
(defvar *test-current-suite* "")

(de test-suite (name . body)
  "Run a group of tests under a named suite."
  (printf "\n--- %s ---\n" name)
  (let ((*test-current-suite* name))
    (each ((expr body)) (eval expr))))

(de test-check (desc expr)
  "Assert that expr is truthy."
  (if expr
      (progn
        (setq *test-pass-count* (+ *test-pass-count* 1))
        (printf "  PASS: %s\n" desc))
    (setq *test-fail-count* (+ *test-fail-count* 1))
    (printf "  FAIL: %s\n" desc)))

(de test-equal (desc actual expected)
  "Assert that actual equals expected."
  (if (= actual expected)
      (progn
        (setq *test-pass-count* (+ *test-pass-count* 1))
        (printf "  PASS: %s\n" desc))
    (setq *test-fail-count* (+ *test-fail-count* 1))
    (printf "  FAIL: %s (got %l, expected %l)\n" desc actual expected)))

(de test-str-equal (desc actual expected)
  "Assert string equality."
  (if (and (stringp actual) (stringp expected) (= actual expected))
      (progn
        (setq *test-pass-count* (+ *test-pass-count* 1))
        (printf "  PASS: %s\n" desc))
    (setq *test-fail-count* (+ *test-fail-count* 1))
    (printf "  FAIL: %s (got \"%s\", expected \"%s\")\n" desc actual expected)))

(de test-summary ()
  "Print summary and return 0 on success, 1 on any failure."
  (printf "\n========================================\n")
  (printf "Results: %d passed, %d failed\n" *test-pass-count* *test-fail-count*)
  (printf "========================================\n")
  (if (= *test-fail-count* 0) 0 1))
```

## Example Test Suite: Storage Types

```scheme
;; tests/suite-storage.lsh

(test-suite "Float Storage"
  (let ((s (float-storage 100)))
    (test-check "float-storage created" s)
    (test-equal "float-storage element size" (sizeof 'float) 4)))

(test-suite "Long Storage"
  (let ((s (long-storage 100)))
    (test-check "long-storage created" s)))

(test-suite "Matrix Creation - All Types"
  (let ((fm (float-matrix 10 20))
        (dm (double-matrix 10 20))
        (im (int-matrix 10 20))
        (sm (short-matrix 10 20))
        (bm (byte-matrix 10 20))
        (um (ubyte-matrix 10 20))
        (lm (long-matrix 10 20)))
    (test-equal "float-matrix dims" (list (idx-dim fm 0) (idx-dim fm 1)) '(10 20))
    (test-equal "double-matrix dims" (list (idx-dim dm 0) (idx-dim dm 1)) '(10 20))
    (test-equal "int-matrix dims" (list (idx-dim im 0) (idx-dim im 1)) '(10 20))
    (test-equal "short-matrix dims" (list (idx-dim sm 0) (idx-dim sm 1)) '(10 20))
    (test-equal "byte-matrix dims" (list (idx-dim bm 0) (idx-dim bm 1)) '(10 20))
    (test-equal "ubyte-matrix dims" (list (idx-dim um 0) (idx-dim um 1)) '(10 20))
    (test-equal "long-matrix dims" (list (idx-dim lm 0) (idx-dim lm 1)) '(10 20))))
```

## Example Test Suite: Compiled Code

```scheme
;; tests/suite-compiled-basic.lsh

;; Define compiled functions for each type
(de test-add-int (a b)
  ((-int-) a b)
  (+ a b))

(de test-add-long (a b)
  ((-long-) a b)
  (+ a b))

(de test-add-flt (a b)
  ((-flt-) a b)
  (+ a b))

(de test-add-real (a b)
  ((-real-) a b)
  (+ a b))

;; Force compilation by calling with typed args
(test-suite "Compiled Scalar Arithmetic"
  (test-equal "compiled int add" (test-add-int 10 20) 30)
  (test-equal "compiled long add" (test-add-long 10 20) 30)
  (test-equal "compiled flt add" (test-add-flt 1.5 2.5) 4.0)
  (test-equal "compiled real add" (test-add-real 1.5 2.5) 4.0))
```

## Example Test Suite: Matrix I/O Round-Trips

```scheme
;; tests/suite-matrix-io.lsh

(de test-matrix-roundtrip (matrix-fn type-name testval path)
  "Test save/load round-trip for a matrix type."
  (let* ((m (matrix-fn 10 20))
         (_ (m 5 10 testval)))
    (save-matrix m path)
    (let ((m2 (load-matrix path)))
      (test-equal (sprintf "%s dimensions preserved" type-name)
                  (list (idx-dim m2 0) (idx-dim m2 1))
                  '(10 20))
      (test-equal (sprintf "%s value preserved" type-name)
                  (m2 5 10) testval))))

(test-suite "Matrix I/O Round-Trips"
  (test-matrix-roundtrip float-matrix "float" 42.5 "/tmp/claude/test-flt.mat")
  (test-matrix-roundtrip double-matrix "double" 42.5 "/tmp/claude/test-dbl.mat")
  (test-matrix-roundtrip int-matrix "int" 42 "/tmp/claude/test-int.mat")
  (test-matrix-roundtrip short-matrix "short" 42 "/tmp/claude/test-short.mat")
  (test-matrix-roundtrip byte-matrix "byte" 42 "/tmp/claude/test-byte.mat")
  (test-matrix-roundtrip ubyte-matrix "ubyte" 42 "/tmp/claude/test-ubyte.mat")
  (test-matrix-roundtrip long-matrix "long" 12345678 "/tmp/claude/test-long.mat"))
```

## Runner

```scheme
;; tests/run-all.lsh

(load (concat-fname (dirname file-being-loaded) "framework.lsh"))

(printf "=== Lush Test Harness ===\n")
(printf "Running all test suites...\n")

(load (concat-fname (dirname file-being-loaded) "suite-storage.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-matrix.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-matrix-io.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-copy.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-string.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-compiled-basic.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-compiled-idx.lsh"))
(load (concat-fname (dirname file-being-loaded) "suite-i64.lsh"))

(let ((exitcode (test-summary)))
  (exit exitcode))
```

## Running

```bash
# Run all tests
./bin/lush tests/run-all.lsh

# Run a single suite
./bin/lush tests/suite-i64.lsh

# CI-style: check exit code
./bin/lush tests/run-all.lsh && echo "ALL TESTS PASSED" || echo "TESTS FAILED"
```

## Coverage Goals

### Before Module Loader Work
- [ ] All 10 storage types work: AT, P, F, D, I32, I16, I8, U8, GPTR, I64
- [ ] Matrix creation for all numeric types
- [ ] Matrix I/O (save/load) round-trips for all types
- [ ] copy-matrix for same-type copies
- [ ] Compiled functions with all scalar type annotations
- [ ] Compiled idx-bloop for all matrix types
- [ ] Compiled class creation and method calls
- [ ] String operations
- [ ] Hash table operations
- [ ] Binary serialization (bwrite/bread) round-trips

### During Module Loader Work
- [ ] Load compiled .so module, call function
- [ ] Two-module dependency resolution
- [ ] Compiled class loaded from module
- [ ] Module with all type operations (verify nothing breaks)
- [ ] Error handling: missing module, unresolved symbols

### After Module Loader Work
- [ ] All above still pass
- [ ] Module unloading (if implemented)
- [ ] Recompilation and reload
- [ ] Platform-specific validation
