# Compiler Directory Audit: lsh/compiler/

Audit date: 2026-02-25

## Overview

The `lsh/compiler/` directory contains the DH compiler, which translates
Lush functions with type declarations into C code. It also contains a
standalone runtime (`lush_runtime.c`) and a standalone binary builder
(`make-standalone.lsh`).

Files examined:
- `lush_runtime.c` (107 lines) — standalone C runtime with malloc wrappers
- `dh-compile.lsh` (1369 lines) — DH compiler driver, generates .c files
- `dh-macro.lsh` (~4900 lines) — DH compiler macros for code generation
- `dh-util.lsh` (~2800 lines) — DH compiler utility functions
- `make-standalone.lsh` (290 lines) — standalone binary builder

---

## lush_runtime.c — Multiple Issues

This file is a minimal C runtime used when building standalone Lush binaries.
It has several 32-bit leftovers.

### Line 21: storage_type_size declared as int[], missing ST_I64

```c
int storage_type_size[ST_LAST] = {
  sizeof(at*),
  sizeof(char),
  sizeof(flt),
  sizeof(real),
  sizeof(int),
  sizeof(short),
  sizeof(char),
  sizeof(unsigned char),
  sizeof(gptr),
};
```

**Problems:**
1. Type is `int[]` but the main source (`src/storage.c:77`) and header
   (`include/header.h:916`) declare it as `size_t[]`.
2. Only 9 entries — missing `sizeof(int64_t)` for `ST_I64` (the main
   source has 10 entries).
3. `sizeof(int)` for ST_I32 is technically fragile — it happens to be
   correct on ILP32 and LP64 (int=32-bit on both), but `sizeof(int32_t)`
   would be more explicit.

### Lines 44, 54, 64: malloc/calloc/realloc wrappers take int sizes

```c
void *lush_malloc(int x, char *file, int line)        // line 44
void *lush_calloc(int x, int y, char *file, int line)  // line 54
void *lush_realloc(void *x, int y, char *file, int line) // line 64
```

The header (`include/header.h`) declares these with `size_t`:
```c
LUSHAPI void *lush_malloc(size_t, const char*, int);
LUSHAPI void *lush_calloc(size_t, size_t, const char*, int);
LUSHAPI void *lush_realloc(gptr, size_t, const char*, int);
```

**Problem:** Function signatures don't match the header. Cannot allocate
>2GB on 64-bit systems via these wrappers. Also `const` qualifier missing
on `file` parameter.

### Lines 48, 58, 68-69: fprintf format %d for size values

```c
fprintf(malloc_file,"%p\tmalloc\t%d\t%s:%d\n",z,x,file,line);   // 48
fprintf(malloc_file,"%p\tcalloc\t%d\t%s:%d\n",z,x*y,file,line);  // 58
```

Should use `%zu` for `size_t` values once signatures are fixed.

---

## dh-compile.lsh — Minor Issues

### Lines 266-267: Version variables declared as int

```lisp
(sprintf "int majver_%s = %s;" filename (getconf "LUSH_MAJOR"))
(sprintf "int minver_%s = %s;" filename (getconf "LUSH_MINOR"))
```

Generated C declares `int majver_xxx` and `int minver_xxx`. The main
source (`src/module.c`) reads these as `int`. Version numbers are small,
so this is harmless. But for consistency with the intg convention, could
be changed. Low priority.

---

## dh-util.lsh — Two Remaining Items

### Line 528: %d format for intg parameter

```lisp
(sprintf "%s = C_allocidx_C_pool(%s,%d);" name obst ndim)
```

The extern on line 526 declares the second parameter as `intg` (already
fixed), but the call site still formats `ndim` with `%d`. Should be `%ld`.
In practice, `ndim` is always small (< MAXDIMS=10), so this is cosmetic.

### Line 545: C_allocobj_C_pool extern uses int

```lisp
"extern_c gptr C_allocobj_C_pool(struct CClass_pool*, int, gptr);"
```

The second parameter receives `sizeof(struct_type)` which is `size_t`.
Using `int` is technically wrong but practically fine since struct sizes
are always small. For consistency, could change to `size_t` or `intg`.

---

## dh-macro.lsh — No Additional Issues Found

All previously identified issues have been fixed. The remaining `(int)`
casts in string operations (~18 locations) and string loop counters (lines
3544, 3978) are intentionally left as-is per the approved directives.

---

## make-standalone.lsh — No Significant Issues

The file generates standalone C binaries. It uses `(-int-)` for argc
(which matches the C standard `int argc`). No 32-bit bugs found in the
code generation logic.

---

## Summary

| File | Issues | Severity |
|------|--------|----------|
| `lush_runtime.c` | 6 | Medium — signature mismatch, missing ST_I64 |
| `dh-compile.lsh` | 1 | Low — version vars as int |
| `dh-util.lsh` | 2 | Low — cosmetic format, extern consistency |
| `dh-macro.lsh` | 0 | All fixed |
| `make-standalone.lsh` | 0 | Clean |

### Priority Assessment

**Should fix:**
- `lush_runtime.c` — the `storage_type_size` declaration is wrong (type
  mismatch and missing entry). This could cause real bugs if standalone
  binaries use int64 storages. The malloc wrapper signatures should also
  match the header.

**Nice to fix:**
- `dh-util.lsh:528` — `%d` → `%ld` for consistency
- `dh-util.lsh:545` — `int` → `size_t` for consistency
- `dh-compile.lsh:266-267` — `int` → `intg` for version vars (low value)
