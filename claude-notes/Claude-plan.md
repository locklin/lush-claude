# Lush 64-bit Cleanup Plan

This document describes a phased plan to make Lush 64-bit clean. Each phase
is designed to be independently testable. Phases are ordered by impact and
dependency -- earlier phases unblock later ones.

The guiding principle is: **fix the plumbing first, then the faucets**. The
`intg` typedef and memory allocator are the foundations; everything else
depends on them.

---

## Phase 0: Add 64-bit Diagnostic Warnings (Low Risk, High Diagnostic Value) -- COMPLETED

**Goal:** Without changing any core logic, add runtime warnings so that a user
who hits a 32-bit limit gets a clear message instead of silent corruption.

**Difficulty:** Easy
**Impact:** High (makes all subsequent work debuggable)
**Risk:** Minimal -- purely additive, no behavior changes

### 0.1 Add overflow-check macros in `include/header.h`

Create a macro like:

```c
#ifndef INTG_IS_LONG
#define INTG_CHECK_OVERFLOW(val, context) do { \
    if ((val) > INT_MAX || (val) < INT_MIN) \
        fprintf(stderr, "LUSH 64-bit WARNING: value overflow in %s " \
                "(value=%ld, max=%d)\n", (context), (long)(val), INT_MAX); \
} while(0)
#else
#define INTG_CHECK_OVERFLOW(val, context) ((void)0)
#endif
```

### 0.2 Instrument idx/storage creation and resize

Add overflow checks at these entry points:

- `src/storage.c`: `storage_malloc()`, `storage_realloc()` -- warn if
  requested size exceeds `INT_MAX`
- `src/index.c`: `index_dimension()`, `index_undim()` -- warn if any
  dimension or total element count exceeds `INT_MAX`
- `src/check_func.c`: `srg_resize()`, `srg_resize_compiled()` -- warn if
  `new_size` exceeds `INT_MAX`

### 0.3 Instrument `lush_malloc` / `lush_calloc` / `lush_realloc`

In `src/allocate.c`, add a warning if the `int` size parameter would have
been larger than `INT_MAX` had the caller used `size_t`:

```c
void *lush_malloc(int x, const char *file, int line) {
    if (x < 0) {
        fprintf(stderr, "LUSH 64-bit WARNING: negative malloc size %d "
                "at %s:%d (likely truncated from 64-bit value)\n",
                x, file, line);
    }
    // ... existing code ...
}
```

### 0.4 Add a test script

Create `tests/test-64bit-warnings.lsh` that attempts to create structures
near the 32-bit boundary and verifies warnings are emitted.

---

## Phase 1: Enable `INTG_IS_LONG` and Fix the Foundation (High Risk, High Impact) -- COMPLETED

**Goal:** Make `intg` be `long` on 64-bit platforms and fix everything that
breaks.

**Difficulty:** Medium-Hard
**Impact:** Critical -- this is the single change that unlocks 64-bit idx/storage
**Risk:** High -- touches every file that uses `intg`, `dim`, `mod`, `offset`

### Alternative Approaches

**Option A: Define `INTG_IS_LONG` unconditionally on 64-bit (Recommended)**
- Add `AC_CHECK_SIZEOF([void *])` to `configure.ac`
- If pointer size >= 8, `#define INTG_IS_LONG 1` in `lushconf.h`
- Pros: Clean, follows original developer intent (see header.h comment)
- Cons: Breaks binary compatibility with existing `.dump` files
- Risk: Medium -- the codebase was partially prepared for this

**Option B: Replace `intg` with `ptrdiff_t` everywhere**
- More "modern C" approach
- Pros: Correct width on all platforms automatically
- Cons: Larger diff, more invasive, diverges from codebase style
- Risk: High -- more changes required

**Option C: Replace `intg` with `int64_t` / `ssize_t`**
- Pros: Explicit width, no ambiguity
- Cons: Breaks 32-bit builds entirely, most invasive
- Risk: High

**Recommendation:** Option A. The original developers designed for this path.

### 1.1 Add `AC_CHECK_SIZEOF` to `configure.ac`

```m4
AC_CHECK_SIZEOF([void *])
AC_CHECK_SIZEOF([long])
AC_CHECK_SIZEOF([int])
if test "$ac_cv_sizeof_void_p" -ge 8; then
    AC_DEFINE([INTG_IS_LONG], [1],
              [Define if intg should be long for 64-bit platforms])
fi
```

### 1.2 Fix `Midx_declare` in `include/idxmac.h` (line 239-240)

**This is the highest-priority fix after enabling `INTG_IS_LONG`.**

Change:
```c
int name2(_dim_,newi)[ndim];
int name2(_mod_,newi)[ndim];
```
To:
```c
intg name2(_dim_,newi)[ndim];
intg name2(_mod_,newi)[ndim];
```

Without this fix, enabling `INTG_IS_LONG` causes immediate memory corruption
because `struct idx` expects `intg *dim` and `intg *mod` (now `long *`) but
`Midx_declare` allocates `int[]` arrays.

### 1.3 Fix `L_ACCESS` macro in `include/idxmac.h` (line 39)

Change `(int *)` cast to `(intg *)`:
```c
#define L_ACCESS(lname, n) \
    ((intg *) ((char *) (((struct srg *) (lname))->data) + sizeof(dharg) * (n)))
```

### 1.4 Fix `lush_malloc` / `lush_calloc` / `lush_realloc` signatures

In `include/header.h` (lines 370-372) and `src/allocate.c`:

Change `int` size parameters to `size_t`:
```c
LUSHAPI void *lush_malloc(size_t, const char*, int);
LUSHAPI void *lush_calloc(size_t, size_t, const char*, int);
LUSHAPI void *lush_realloc(gptr, size_t, const char*, int);
```

This change ripples through every `malloc()` call in the codebase, but since
the macros on lines 378-382 redirect transparently, most call sites need no
change.

### 1.5 Fix `srg_resize` and `srg_resize_compiled` in `src/check_func.c`

Change `int new_size` to `intg new_size`, and change the local `int st_size`
to `size_t st_size` to prevent overflow in the multiplication
`storage_type_size[sr->type] * new_size`.

### 1.6 Fix `storage_type_size` array type

In `include/header.h` (line 910), change:
```c
extern LUSHAPI int storage_type_size[ST_LAST];
```
To:
```c
extern LUSHAPI size_t storage_type_size[ST_LAST];
```

And update the definition in `src/storage.c`.

### 1.7 Fix loop variables in idx macros

In `include/idxmac.h` and `include/idxops.h`, change `int` loop variables
that hold dimension/modulo values to `intg`:

- `idxmac.h`: `Midx_update_mod_from_dim` (line 97), `Midx_diagonal`
  (line 173), `Midx_select` (line 196), `Midx_clone` et al. (lines 284+)
- `idxops.h`: `Midx_m2dotm2acc` (line 687), `Midx_m4dotm2acc` (line 1246)

### 1.8 Fix `lisp_c.c` idx allocation

In `src/lisp_c.c` (lines 970, 974), the compiled-side idx allocation
uses `sizeof(int)` and `int*` for dim/mod arrays. Change to
`sizeof(intg)` and `intg*`.

### 1.9 Fix the `define.h` fallback `memset`

In `include/define.h` (line 163), change `int nn` to `size_t nn`.

### 1.10 Build and run test suite

Verify the build succeeds with `INTG_IS_LONG` enabled and run existing
tests plus the Phase 0 diagnostic tests.

---

## Phase 2: Fix Binary Serialization and I/O (Medium Risk, Medium Impact)

**Goal:** Ensure dump/undump, matrix I/O, and file operations work correctly
for both old 32-bit format files and new 64-bit data.

**Difficulty:** Medium
**Impact:** Medium -- affects file I/O and persistence
**Risk:** Medium -- must maintain backward compatibility with existing files

### Alternative Approaches for File Format

**Option A: Keep 32-bit file format, add 64-bit extension (Recommended)**
- Existing IDX files use 4-byte dimension words
- Add a new magic number / format version for 64-bit dimensions
- Read old files by widening 4-byte values to `intg`
- Write 64-bit format only when dimensions exceed `INT_MAX`
- Pros: Full backward compatibility
- Cons: More code paths to maintain
- Risk: Low

**Option B: Switch entirely to 64-bit file format**
- All dimension words become 8 bytes
- Pros: Simpler code
- Cons: Breaks all existing `.mat` and `.dump` files
- Risk: High

### 2.1 Fix `read4` / `write4` in `include/header.h` (lines 677-678)

These are correct for their purpose (reading/writing 4-byte values). Add
companion `read8` / `write8` functions for 64-bit serialization.

### 2.2 Fix `dump.c` `write32` / `read32`

In `src/dump.c` (lines 62-81):
- Add `write64` / `read64` variants
- Update the dump format to use a new magic number when 64-bit values present
- Keep backward-compatible reading of old 32-bit dumps

### 2.3 Fix `import_raw_matrix` file offset

In `include/header.h` (line 1096), change `int offset` to `off_t offset`.
Update `src/index.c` accordingly.

### 2.4 Fix `save_matrix_len` return type

In `include/header.h` (line 1098), change return type from `int` to `intg`
or `size_t`.

### 2.5 Update `lsh/libidx/idx-io.lsh`

The IDX binary format reads/writes 4-byte dimension words via `fread-int`.
Add logic to detect and handle a 64-bit format variant. Consider adding
a `save-idx64` / `load-idx64` function pair.

### 2.6 Fix `file-size` in `lsh/libc/stdio.lsh`

Change return type from `(-int-)` to `(-real-)` or add an `(-int-)` type
that maps to `intg` (see Phase 3).

---

## Phase 3: Fix the Lisp Compiler's C Code Generation (High Risk, High Impact)

**Goal:** Make the compiled Lisp-to-C path generate 64-bit-correct C code.

**Difficulty:** Hard
**Impact:** Critical -- compiled Lush code will silently corrupt data otherwise
**Risk:** High -- the compiler is complex and changes affect all compiled code

### Alternative Approaches

**Option A: Change `dht-int` to emit `intg` instead of `int` (Recommended)**
- In `dh-util.lsh`: `dhc-type-to-c-decl` returns `"intg"` for `dht-int`
- Pros: Minimal change, follows the `intg` convention
- Cons: All Lush `(-int-)` declarations become 64-bit; may break FFI calls
  expecting actual `int`
- Risk: Medium

**Option B: Add a new `dht-intg` type alongside `dht-int`**
- `(-int-)` stays 32-bit C `int`, new `(-intg-)` maps to C `intg`
- Pros: Backward compatible, explicit control
- Cons: Significant compiler changes, users must choose types
- Risk: Medium-High

**Option C: Add a separate `dht-long` type**
- Similar to Option B but uses `long` directly
- Risk: Medium

**Recommendation:** Option A for simplicity, with targeted exceptions for
actual C FFI calls that need true `int`. Option B is safer but more work.

### 3.1 Fix `dhc-type-to-c-decl` in `lsh/compiler/dh-util.lsh`

Change the `dht-int` case from `"int"` to `"intg"` (line 381).

### 3.2 Fix `(int)` casts in `lsh/compiler/dh-macro.lsh`

Replace all `(int)` casts on idx dimension/size/step values with `(intg)`:
- `unfold` (lines 1878-1880)
- `select` (lines 1955-1956)
- `diagonal` (lines 2022-2023)
- `narrow` (lines 2060-2062)

### 3.3 Fix loop variable declarations in `dh-macro.lsh`

Change generated `int _XXX_max` and `int _XXX_mod` declarations to `intg`:
- `idx-bloop` (lines 690, 694, 708)

### 3.4 Fix `idx-dim`, `idx-modulo`, `idx-offset` return types

In `dh-macro.lsh`, change these from `dht-int` to the appropriate 64-bit
type (lines 1395, 1412, 1437).

### 3.5 Fix `idx-size` return type and calculation

In `dh-macro.lsh` (line 1483), the element count calculation uses `int i`
and returns `dht-int`. Both should be `intg`.

### 3.6 Fix `(int *)` object pointer cast in `dh-compile.lsh`

In `dh-compile.lsh` (lines 412-413), change `(int *)` to `(intg *)`.

### 3.7 Fix `to-int` / `int` cast operations in `dh-macro.lsh`

The `to-int` operation generates `(int)` casts (lines 204-207). This needs
to generate `(intg)` on 64-bit, or a new `to-intg` should be added.

### 3.8 Fix `libc.lsh` function signatures

- `malloc`: `(-int-) n` -> appropriate size type (line 41)
- `memcpy`: `(-int-) size` -> appropriate size type (line 57)
- `gptr+`: `(-int-) n` -> `(-int-)` is OK if `int` means `intg` (line 105)
- `gptr-`: `(int)` cast -> `(intg)` or `(ptrdiff_t)` (line 113)

---

## Phase 4: Fix Pointer/Integer Conversions and Hashing (Medium Risk, Medium Impact)

**Goal:** Fix all pointer-to-integer conversions and hash functions for 64-bit.

**Difficulty:** Medium
**Impact:** Medium -- affects correctness of hash tables, object identity,
and display
**Risk:** Low-Medium -- most changes are mechanical

### 4.1 Fix `tlsizeof` in `src/calls.c` (line 199-200)

`sizeof("long")` returns `sizeof(int)` -- must return `sizeof(long)`.

Also fix the `*(long*)&delta` type-pun bug at line 147 (reads 8 bytes from
a 4-byte `float` on LP64). Replace with `memcmp` or a union.

### 4.2 Fix hash rotation masks

These 32-bit masks only rotate the lower 32 bits on a 64-bit `unsigned long`:
- `src/symbol.c` line 77: `0xfc000000` -> use `sizeof(unsigned long)`-aware
  rotation
- `src/date.c` line 366: same fix
- `src/htable.c` line 274: `0x80000000` -> proper high-bit test

### 4.3 Fix hash mixing in `src/htable.c`

Line 256: `x = x ^ (x>>16)` only mixes low 32 bits. Add a 64-bit-aware
mixing step: `x ^= (x >> 32); x ^= (x >> 16);`

### 4.4 Fix `complex_hash` in `src/arith.c` (line 99)

The `union { real r; long l[2]; }` is sized incorrectly on LP64 where
`sizeof(long) == sizeof(double) == 8`, making `l[2]` be 16 bytes vs `r`
at 8 bytes. Fix: use `int l[2]` or `uint32_t l[2]`.

### 4.5 Fix pointer display format strings

Change `(long)ptr` + `%lx` to `(uintptr_t)ptr` + `PRIxPTR` or just `%p`:
- `src/at.c` lines 1055, 1058
- `src/oostruct.c` line 375
- `src/graphics.c` line 76

### 4.6 Fix pointer-through-double round-trip

In `src/at.c` (line 963) and `src/lisp_c.c` (line 899), pointers are
stored in `double` (`real`). On 64-bit, `double` has only 53 bits of
mantissa, so addresses above 2^53 lose precision. This is an architectural
issue that may need a tagged-pointer or separate slot approach.

**Alternative approaches:**
- **Option A:** Accept the 53-bit limitation (addresses above 8PB).
  Current Linux user-space is limited to 47-48 bits, so this is safe
  **for now**. Add a runtime check. (Recommended)
- **Option B:** Add a separate `intptr_t` field to `struct at` for pointer
  storage. More correct but invasive.

---

## Phase 5: Fix Time and Miscellaneous Type Issues (Low Risk, Low-Medium Impact)

**Goal:** Fix Y2038 issues, remaining type mismatches, and cosmetic 32-bit
assumptions.

**Difficulty:** Easy-Medium
**Impact:** Low-Medium
**Risk:** Low

### 5.1 Fix Y2038 time truncation

- `src/unix.c` lines 769, 774, 779: `time_t` to `int` truncation
- `src/event.c` line 362: `real` to `int` for timestamps
- `src/event.c` lines 223-226: `struct evtime` uses `int sec`; change to
  `long` or `time_t`

### 5.2 Fix `length()` return type

In `include/header.h` (line 279), change return type to `intg`.

### 5.3 Fix `new_string_bylen` and string functions

- `new_string_bylen(int n)` -> `new_string_bylen(size_t n)` (header.h:533)
- `large_string_add` `int len` -> `size_t len` (header.h:563)
- `str_index` `int start` -> `intg start` (header.h:534)

### 5.4 Fix `struct alloc_root` element sizes

In `include/header.h` (lines 340-341), change `int elemsize` to
`size_t elemsize`.

### 5.5 Fix `new_htable` and `makelist` signatures

- `new_htable(int nelems, ...)` -> `intg nelems` (header.h:714)
- `makelist(int n, ...)` -> `intg n` (header.h:742)

### 5.6 Fix `struct hashtable` field types

In `src/htable.c` (line 49), change `int size` and `int nelems` to
`intg` or `size_t`.

### 5.7 Fix `graphics.c` image buffer sizes

- `static unsigned int imagesize` -> `size_t` (line 740)
- Fix `int` arithmetic overflow in image size calculations (lines 994-1001,
  1395-1403)

### 5.8 Fix `nan.c` type-pun assumptions

Line 521 checks `sizeof(int)==4` at runtime. The unions at lines 96-97 and
148-149 should use `uint32_t` / `int32_t` for clarity.

---

## Phase 6: Update Build System (Low Risk, Medium Impact)

**Goal:** Modernize the autoconf build for 64-bit awareness without
changing the GNU Autoconf build style.

**Difficulty:** Easy-Medium
**Impact:** Medium
**Risk:** Low

### 6.1 Add size checks to `configure.ac`

```m4
AC_CHECK_SIZEOF([void *])
AC_CHECK_SIZEOF([long])
AC_CHECK_SIZEOF([int])
AC_CHECK_SIZEOF([size_t])
```

### 6.2 Auto-define `INTG_IS_LONG`

Based on `AC_CHECK_SIZEOF` results, emit `#define INTG_IS_LONG` into
`lushconf.h`.

### 6.3 Fix deprecated autoconf macros

- Replace `AC_HEADER_DIRENT` (deprecated in 2.70)
- Replace `AC_HEADER_SYS_WAIT` (deprecated in 2.70)
- Remove `TIME_WITH_SYS_TIME` (marked obsolete in the source itself)
- Remove `vfork` and `cfree` checks (removed from POSIX long ago)

### 6.4 Fix the `HAVE_BFD_SET_SECTOIN_SIZE` typo

In `configure.ac` (line 194), `SECTOIN` -> `SECTION`.

### 6.5 Regenerate configure

Run `autoconf` to regenerate `configure` from `configure.ac`. Verify the
generated script works on current Linux.

### 6.6 Add `-Wconversion` diagnostic build target

Add a Makefile target `make check-warnings` that builds with
`-Wconversion -Wsign-conversion` and pipes warnings to a file for review.

---

## Summary: Difficulty and Impact Rankings

| Phase | Description | Difficulty | Impact | Risk |
|-------|-------------|------------|--------|------|
| 0 | Diagnostic warnings | Easy | High (debugging) | Minimal |
| 1 | `INTG_IS_LONG` + foundation | Medium-Hard | Critical | High |
| 2 | Binary I/O format | Medium | Medium | Medium |
| 3 | Lisp compiler codegen | Hard | Critical | High |
| 4 | Pointer/hash fixes | Medium | Medium | Low-Medium |
| 5 | Time, strings, misc | Easy-Medium | Low-Medium | Low |
| 6 | Build system | Easy-Medium | Medium | Low |

### Recommended Execution Order

1. **Phase 0** -- immediate, zero risk, enables diagnosis
2. **Phase 6.1-6.2** -- enable `INTG_IS_LONG` in configure
3. **Phase 1** -- fix the C foundation
4. **Phase 3** -- fix the compiler's C generation
5. **Phase 4** -- fix pointer/hash issues
6. **Phase 2** -- fix serialization
7. **Phase 5** -- cleanup
8. **Phase 6.3-6.6** -- build system modernization

### Dependencies

```
Phase 0 ──> Phase 1 ──> Phase 3
                    ├──> Phase 2
                    ├──> Phase 4
                    └──> Phase 5
Phase 6.1-6.2 ──> Phase 1
Phase 6.3-6.6 (independent)
```

---

## Test Strategy

Each phase should include:

1. **Build test:** `make clean && make` succeeds with zero warnings on the
   changed files (using `-Wall -Wextra`)
2. **Smoke test:** Run `bin/lush` and execute basic operations
3. **Regression test:** Run existing demos/examples
4. **64-bit specific test:** Test with arrays near and above 2^31 elements
5. **Serialization test:** (Phase 2+) Verify old `.mat` files still load

A `tests/` directory should be created with `.lsh` scripts for automated
testing.
