# Pointer-in-Double Analysis

## The `at` Union Architecture

Every Lush value is an `at` struct (include/header.h:181-207):

```c
typedef struct at {
  unsigned int count;       // 4 bytes  (refcount)
  unsigned short flags;     // 2 bytes  (type tag)
  unsigned short ident;     // 2 bytes
  union {                   // 16 bytes (the payload)
    struct { struct at *at_car, *at_cdr; } at_cons;   // two 8-byte pointers
    real   at_number;                                  // 8-byte double
    gptr   at_gptr;                                    // 8-byte void*
    struct { gptr at_object; struct class *at_class; } at_extern; // two 8-byte ptrs
  } at_union;
} at;   // total: 24 bytes
```

Shorthand macros: `Car`, `Cdr`, `Number`, `Gptr`, `Object`, `Class`.

On 64-bit systems the union is 16 bytes. The memory layout overlap:

```
Union bytes 0-7:   Number (double)  |  Gptr (void*)  |  Car (at*)    |  Object (gptr)
Union bytes 8-15:  [unused by Num]  |  [unused]      |  Cdr (at*)    |  Class (class*)
```

The `flags` field (C_CONS, C_NUMBER, C_GPTR, C_EXTERN) determines which union variant
is active. Code checks flags before accessing any field. This is type-safe as long as
nothing stores a pointer VALUE in a Number field or vice versa.

## The Three Layers of the Problem

### Layer 1: Direct pointer-in-Number (2 sites)

These are the only places where a **pointer address** is intentionally stored in or
retrieved from a `double` (Number) field:

**Site A — `src/lisp_c.c:901` (pointer FROM Number):**
```c
// xlisp_c_map: user passes a numeric address, we convert to C pointer
else if (p->flags & C_NUMBER)
    return lisp_c_map((void*)(uintptr_t)(p->Number));
```
A Lush number (double) is interpreted as a memory address. If the address has non-zero
bits above position 52, the retrieved pointer will be corrupted. No precision check.

**Site B — `src/at.c:964-967` (pointer TO Number):**
```c
// xunode_uid: convert a pointer identity to a Lush number
uid = (uintptr_t)unode_dive(APOINTER(1));
if (uid != (uintptr_t)(real)uid)                    // <-- PRECISION CHECK
    error(NIL,"pointer value exceeds double precision",NIL);
return NEW_NUMBER((real)uid);
```
This is the ONLY place in the codebase that acknowledges the precision problem.
It checks for data loss and errors out. But the error means `unode-uid` will simply
fail on systems with >52-bit addresses.

**Current safety margin:** Linux x86-64 uses 48-bit virtual addresses (4-level page
tables). 48 < 53 (double mantissa), so pointer-to-double round-trips work by accident.
Intel's 5-level paging extends this to 57 bits, which WOULD break these conversions.

### Layer 2: Large integer precision loss (~30 sites)

Lush has no integer type at the interpreter level — ALL numbers are `double`. When C
code returns a 64-bit integer via `NEW_NUMBER()`, values > 2^53 lose precision.

**Most impactful sites:**

| File | Line | What | Risk |
|------|------|------|------|
| `src/lisp_c.c` | 2114 | `NEW_NUMBER(arg->dh_long)` — DHT_LONG return | int64_t > 2^53 loses bits |
| `src/index.c` | 532 | `NEW_NUMBER(size)` — total element count | >9 quadrillion elements (theoretical) |
| `src/index.c` | 549 | `NEW_NUMBER(size)` — byte size | >8 PB matrices (theoretical) |
| `src/index.c` | 570 | `NEW_NUMBER(ind->offset)` | huge offsets from narrow/select chains |
| `src/index.c` | 620,659 | `NEW_NUMBER(ind->dim[n])` / `ind->mod[n]` | >9 quadrillion dim (theoretical) |
| `src/fileio.c` | 501 | `NEW_NUMBER(buf.st_ino)` — inode number | XFS inodes can exceed 2^53 |
| `src/fileio.c` | 505 | `NEW_NUMBER(buf.st_size)` — file size | files > 8 PB (theoretical) |
| `src/storage.c` | ~9 sites | storage sizes/offsets | same as index |

**Practical impact:** Low for matrix operations (nobody has 9 quadrillion elements).
But `DHT_LONG` is a real concern — a compiled Lush function returning `int64_t` values
near INT64_MAX will silently lose precision when the value crosses back to Lush.

The reverse path also has issues: `src/lisp_c.c:1866`:
```c
case DHT_LONG:
    arg->dh_long = (int64_t) at_obj->Number;
```
An int64_t value that was stored as a double and lost precision will be truncated
differently when read back as int64_t.

### Layer 3: The union overlap itself (all code)

Every source file that touches `at` structs (all 40 .c files) relies on the union
architecture. But this layer is NOT a bug — the flags system correctly discriminates
variants. The union saves 8 bytes per `at` object (24 bytes instead of 32), which
matters when there are millions of cons cells.

**The union is safe** as long as:
1. Code checks flags before accessing a field (it does, everywhere)
2. Pointer VALUES are never stored in Number (violated in 2 places — Layer 1)
3. Integer values fit in double's 53-bit mantissa (violated for large int64_t — Layer 2)

## Quantification

### How many files and sites?

| Category | Files | Sites | Severity |
|----------|-------|-------|----------|
| Pointer-in-Number (Layer 1) | 2 | 2 | HIGH — will break on 57-bit VA systems |
| Large int precision loss (Layer 2) | 5 | ~30 | MEDIUM — affects int64_t round-trips |
| Union field access via flags (Layer 3) | 40 | ~760 | NONE — architecturally sound |
| `NEW_NUMBER()` calls total | 24 | 277 | Most are safe (small ints, floats) |
| `->Number` reads total | 19 | ~120 | Most are numeric extraction (safe) |
| `->Gptr` reads total | 8 | ~19 | All correct (use C_GPTR flag) |
| `->Car` / `->Cdr` reads total | 26 | ~500+ | All correct (use C_CONS flag) |

### Layer 1 fix scope

Only 2 call sites need changes. But the **real fix** for pointer-in-Number would be to
change `unode-uid` and `lisp-c-map` to use `NEW_GPTR()` / C_GPTR for pointer values
instead of encoding them as doubles. This is a small, contained change.

### Layer 2 fix scope

The int64_t precision loss is harder. Options:

1. **Accept it** — doubles represent integers exactly up to 2^53, which covers most
   practical cases. Document the limitation.

2. **Add a bignum or int64 type** — a new C_INT64 flag and `at_int64` union member.
   This would require changes to:
   - `include/header.h` — new flag and union member (~5 lines)
   - `src/at.c` — new constructor, printer, comparison (~50 lines)
   - `src/calls.c` — equality/comparison for int64 (~30 lines)
   - `src/arith.c` — arithmetic operators (~100 lines)
   - `src/io.c` — print formatting (~20 lines)
   - `src/binary.c` — serialization (~20 lines)
   - `src/lisp_c.c` — DHT_LONG conversion (~10 lines)
   - `lsh/compiler/` — compiler type mapping (~20 lines)
   - **Total: ~255 lines across 8 files**

3. **Use Gptr for large ints** — store int64_t values > 2^53 as boxed gptr objects.
   Awkward but possible without changing the union.

### What does NOT need to change

- The union itself — the overlapping layout is fine, the flags system works
- Car/Cdr access — always used on C_CONS objects, never confused with Number
- Object/Class access — always used on C_EXTERN objects
- Gptr access — always used on C_GPTR objects
- The 277 `NEW_NUMBER()` sites that store actual numeric values (most of them)
- The 120 `->Number` reads that extract actual numeric values

## Summary

The pointer-in-double issue in Lush has three distinct sub-problems:

1. **Pointer addresses in doubles** — 2 sites, trivially fixable by using GPTR instead
2. **int64_t precision loss in doubles** — ~30 sites, would need a new integer type to
   fully fix, but practically low-impact since values > 2^53 are rare
3. **Union overlap architecture** — not a bug, well-protected by flags, touches all code
   but needs no changes

The codebase is NOT "fundamentally broken" by this issue. The union architecture is
sound. The actual vulnerabilities are confined to 2 pointer-encoding sites and the
DHT_LONG round-trip path.
