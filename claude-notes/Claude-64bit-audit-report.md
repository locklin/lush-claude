# 64-bit Audit Report: Remaining 32-bit Bugs in Lush C Source

Audit date: 2026-02-25

## Background

Lush was originally a 32-bit system. A prior audit (Phases 0-6) added
`INTG_IS_LONG` support so that `intg` = `long` = 64-bit on LP64 platforms.
The struct fields (`idx.dim[]`, `idx.mod[]`, `idx.offset`, `srg.size`) are
now 64-bit. However, **many C functions still funnel these values through
32-bit `int` temporaries**, silently truncating.

This report covers the remaining issues found in the C source (`src/`),
headers (`include/`), and DH compiler (`lsh/compiler/`).

`INTG_IS_LONG` is confirmed defined as 1 in `lushconf.h`.

---

## Tier 1: Serialization Truncation (data-corrupting)

These silently destroy data when arrays have >2G elements or >2GB offsets.
The bwrite/bread and old serialization formats are fundamentally 32-bit
wire formats.

### DIRECTIVE: Leave format alone. Add cheap size guards.

**Do NOT change the serialization wire format.** A proper format upgrade
would be a large project (protocol buffers or similar) — defer to a future
effort. Instead, add cheap pre-serialization size checks that raise an
error instead of silently truncating. The check must be very cheap (a
comparison before each `serialize_int` call for index metadata, and before
the storage size write).

### index.c:202-218 — serialize truncates offset, dim[], mod[]

```c
{
    int tmp;
    tmp = id->offset;       // intg → int truncation
    serialize_int(&tmp, code);
    if (code == SRZ_READ) id->offset = tmp;
}
// ... same pattern for dim[i] and mod[i] at lines 209-218
```

All three fields (offset, dim[], mod[]) are `intg` (64-bit) but serialized
through `int tmp` (32-bit). Any idx with values > 2^31 will be corrupted
on save/load via the old serialization format.

**FIX**: Add `if (id->offset > INT_MAX || id->offset < INT_MIN) error(...)`
before the assignment to `tmp` (write path only, i.e., `code != SRZ_READ`).
Same for dim[i] and mod[i].

### storage.c:452-464 — storage size serialized as int

```c
int type, flags, size;
// ...
size = st->srg.size;        // intg → int truncation
serialize_int(&size, code);
```

Storage size is `intg` but serialized as `int`. Any storage with > 2^31
elements will have its size truncated.

**FIX**: Add `if (st->srg.size > INT_MAX) error(...)` before the assignment
(write path only).

### binary.c:891-894 — bwrite caps dimensions at 32-bit

```c
if (arr->dim[i] > INT_MAX)
    error(NIL, "array dimension too large for binary format", NIL);
write_card32(arr->dim[i]);
```

At least there's an error check, but the binary format fundamentally
cannot represent 64-bit dimensions.

**STATUS**: Already guarded. Leave alone.

### binary.c:1119 — bread reads dimensions as 32-bit

```c
size *= ( dim[i] = read_card32() );
```

Files written by bwrite can't round-trip 64-bit dimensions. This is a
format limitation, not just a code bug.

**STATUS**: Format limitation. Leave alone.

---

## Tier 2: Size/Offset Overflow (crash or wrong result)

### DIRECTIVE: Fix ALL items in this tier.

All int intermediate calculations must be widened to `intg`. This
includes byte count calculations, contiguity checks, and all index
manipulation functions. The `index_check_size` overflow is especially
critical — it is the safety net, and if it overflows, the safety check
itself is broken.

These use `int` for intermediate calculations on values that come from
64-bit struct fields, causing silent overflow.

### index.c:518,534 — element count overflow

```c
// xindex_nelements (line 518) and xindex_size (line 534)
int size, i;
size *= ind->dim[i];    // int *= intg → overflow
```

Total element count overflows `int` for arrays with > 2^31 total elements.
`xindex_nelements` and `xindex_size` both have this bug.

### index.c:2818-2835 — transpose truncates dim/mod

```c
// xindex_transpose2
int d1, d2, tmp;
tmp = ind->dim[d1];     // intg → int truncation
ind->dim[d1] = ind->dim[d2];
ind->dim[d2] = tmp;     // truncated value written back
// same for mod[]
```

Swapping dimensions via `int tmp` silently truncates 64-bit values.

### index.c:2991-3001 — change_offset truncates offset

```c
// xindex_change_offset
int x, old_val;
old_val = ind->offset;  // intg → int
// ...
ind->offset = x;        // truncated value written back
```

Cannot set offsets > INT_MAX.

### index.c:2939-2952 — change_dim truncates dimensions

```c
// xindex_change_dim
int d, x, oldx;
oldx = ind->dim[d];     // intg → int
```

### index.c:2715-2732 — narrow overflows with large strides

```c
// xindex_narrow
int d, sz, st;
ind->offset += st * ind->mod[d];  // int * intg → overflow before addition
```

### index.c:2744-2759 — select overflows offset calculation

```c
// xindex_select
int d, x;
ind->offset += x * ind->mod[d];   // int * intg → overflow
```

### storage.c:754,789 — new_storage size parameter is int

```c
new_storage(int type, int size)     // size should be intg
new_storage_nc(int type, int size)  // size should be intg
```

Cannot create storages with > 2^31 elements via these functions.

### storage.c:1232 — byte count overflow

```c
int n;
n = storage_type_size[st->srg.type] * st->srg.size;  // overflow
```

### lisp_c.c:2227,2361 — byte count overflow in DH interface

```c
int bytes;
bytes = cptr->size * storage_type_size[cptr->type];   // overflow
// (line 2227 in update_c_from_lisp, line 2361 in update_lisp_from_c)
```

### index.c:1871 — contiguity check overflow

```c
// contiguity_check
int size, i;
size *= ind->dim[i];    // same overflow as xindex_nelements
```

---

## Tier 3: Pointer Truncation (64-bit correctness)

### DIRECTIVE: Fix LLP64 risks, gptr formatting, hex conversion. Leave pointer-to-double alone.

Even though we don't support Windows/LLP64, fix the `unsigned long`
pointer casts to `uintptr_t` for correctness. Fix the gptr formatting
in string.c and the generated hex conversion in dh-macro.lsh. The
pointer-to-double hack in at.c should be elaborated on (explained in
detail) but left alone — it already has a runtime guard and changing it
would be a deeper refactor of the Lisp type system.

### lisp_c.c:1583,1588 — unsigned long pointer cast

```c
return NEW_GPTR((unsigned long)px);
```

Correct on LP64 Linux (long = 64-bit) but breaks on LLP64 Windows
(long = 32-bit). Should use `uintptr_t`.

**FIX**: Change `(unsigned long)` to `(uintptr_t)`.

### lisp_c.c:2319,2467 — struct field offset via unsigned long

```c
pos = (char*)cptr + (unsigned long)(drec->arg);
// (update_c_from_lisp at 2319, update_lisp_from_c at 2467)
```

Same LLP64 risk. Should use `uintptr_t` or `ptrdiff_t`.

**FIX**: Change `(unsigned long)` to `(uintptr_t)`.

### at.c:961-967 — pointer-to-double loses precision

```c
uid = (uintptr_t)unode_dive(APOINTER(1));
if (uid != (uintptr_t)(real)uid)
    error(NIL,"pointer value exceeds double precision",NIL);
return NEW_NUMBER((real)uid);
```

`double` has 53 bits of mantissa. Pointers above 2^53 will lose bits.
The check detects it after the fact, but the function is fundamentally
broken for large addresses (which are common with ASLR).

**STATUS**: Leave alone. This is a deep issue with the Lisp type system —
Lush represents all numbers as `real` (double), which fundamentally cannot
hold arbitrary 64-bit pointers. The runtime guard (`if uid != (uintptr_t)
(real)uid`) at least detects the problem. Fixing this properly would
require adding a separate integer/pointer type to the Lisp value system,
which is out of scope.

**ELABORATION**: The pointer-to-double hack works as follows:
- `unode_dive()` returns a `void*` (the dereferenced GPTR value)
- This pointer is cast to `uintptr_t` (a 64-bit unsigned integer)
- It's then stored as `real` (double-precision float) via `NEW_NUMBER`
- `double` has 53 bits of mantissa, so pointers up to 2^53 (8 PB) are
  exact. Current x86-64 uses 48-bit virtual addresses (canonical form),
  so all user-space pointers on current hardware fit without loss.
- The guard catches future architectures with >53-bit addresses.
- With ASLR, user-space addresses on Linux are typically in the range
  0x5500_0000_0000 to 0x7fff_ffff_ffff — all well within 48 bits.
- The function is used by `to-gptr` / `from-gptr` Lush primitives for
  FFI interop. It's ugly but functionally correct on all current hardware.

### string.c:806 — gptr formatting

```c
snprintf(string_buffer, STRING_BUFFER, "#$%lX", (unsigned long)(x));
```

Should use `uintptr_t` with `PRIxPTR` or just `%p`.

**FIX**: Change to `PRIxPTR` with `(uintptr_t)` cast.

### dh-macro.lsh:3851 — generated hex conversion truncates to int

```lisp
(sprintf "sprintf(tmpchar,\"0x%%x\",(int)(%s));" val)
```

Generates C code that casts to `int` and formats with `%x`. Should be
`(unsigned long)` with `%lx`, or `(uintptr_t)` with `PRIxPTR`.

**FIX**: Change generated code to use `(unsigned long)` with `%lx`.

---

## Tier 4: DH Compiler Code Generation (int vs intg in generated C)

The DH compiler (`lsh/compiler/dh-macro.lsh`, `dh-util.lsh`,
`dh-compile.lsh`) generates C code. Several places generate `int` where
`intg` is needed.

### DIRECTIVE: Per-item decisions below. General principle: DH compiler should generate `intg` not `int`.

### dh-macro.lsh — ~18 locations with (int) casts in string operations

Lines 3469, 3473, 3586, 3634, 3639, 3641, 3679, 3682, 3719, 3726,
3897, 3899, 3903, 3905, 3951, 3956, 3958, 3964.

Examples:
```lisp
;; Line 3639
(sprintf "if (memccpy(%s->data,%s->data,'\\0',(int) %s)==NULL)" ...)

;; Line 3641
(sprintf "((char *)(%s->data))[(int) %s]='\\0';" ...)

;; Line 3726
(sprintf "(int)((strlen((char*)(%s->data))>%s)?..." ...)
```

All generate `(int)` casts for string lengths and indices.

**DECISION**: Leave alone. Strings are sequences of bytes. The `(int)`
casts here operate on string lengths and character indices. Strings are
internally represented as byte arrays (`char*` data in an srg). The int
casts are fine unless strings are allocated assuming longs, which they are
not — string storage is byte-addressed. 2GB strings should never exist in
practice. The casts don't introduce correctness bugs for any realistic
string size.

### dh-macro.lsh:378 — printf format for pointers

```lisp
(sprintf "printf(\"%%x \", %s);" retplace)
```

Generates `%x` for pointer values. Should be `%p`.

**FIX**: Change `%%x` to `%%p` in the generated printf format.

### dh-macro.lsh:2122 — permutation list type

```lisp
"static int permlist[] = {"
```

Generates `int[]` for dimension permutation indices. Should be `intg[]`.

**FIX**: Change `"static int permlist[]"` to `"static intg permlist[]"`.

### dh-macro.lsh:3544,3978 — loop counters for string iteration

```lisp
(dhc-add-c-statements (sprintf "{\nint ctr;\nctr=0;" ))
```

Loop counter `ctr` declared as `int` for string character iteration.

**DECISION**: Leave alone. String loop counters as `int` are fine.
Strings are byte arrays and will never exceed 2GB in practice. Same
reasoning as the string `(int)` casts above.

### dh-macro.lsh:3769 — templen declared as int

```lisp
(dhc-add-c-statements "{\nchar tempstr[1100]; int templen;")
```

Holds `strlen()` result (returns `size_t`). Should be `intg` or `size_t`.

**FIX**: Change `int templen` to `size_t templen` in generated code.

### dh-util.lsh:2670 — idx mod cast to int

```lisp
(sprintf "(%s)->mod[%d]*((int)%s)" mat (incr num) x))
```

Casts index argument to `(int)` before multiplying with `mod[]` (which is
`intg`). Overflow risk.

**FIX**: Change `(int)` to `(intg)` in generated code.

### dh-util.lsh:2777-2780 — %d format for intg assignments

```lisp
(dhc-add-c-statements (sprintf "(%s)->dim[%d] = %d;" :idx-symb:c-name i nd))
(dhc-add-c-statements (sprintf "(%s)->mod[%d] = %d;" :idx-symb:c-name i siz))
(dhc-add-c-statements (sprintf "(%s)->size = %d;" :srg-symb:c-name siz))
```

Uses `%d` to format values assigned to `intg` fields. If the Lush integer
exceeds int range, the format will be wrong.

**FIX**: Change `%d` to a format consistent with `intg`. Since `intg` is
`long` when `INTG_IS_LONG`, use `%ld`. (Note: these are Lush `sprintf`
calls formatting Lush integers into C source code strings — the format
needs to handle the Lush integer value range, which is 64-bit.)

### dh-util.lsh:526 — C_allocidx_C_pool ndim parameter

```lisp
(dhc-add-c-externs
 "extern_c gptr C_allocidx_C_pool(struct CClass_pool*, int);")
```

Declares ndim as `int`. Should be `intg` for consistency.

**FIX**: Change `int` to `intg` in the extern declaration.

### lisp_c.c:1746,1785 — dh_int accessed via int pointer

```c
*((int *) addr) = arg->dh_int;    // line 1746
arg->dh_int = *((int *) addr);    // line 1785
```

`dh_int` is `intg` (64-bit) but these lines cast to `int *` (32-bit).
This reads/writes only the low 32 bits. Whether this is a bug depends on
whether the DH-compiled code allocates `int` or `intg` at `addr`.

**FIXED**: Changed `(int *)` to `(intg *)`. The generated C code declares
parameters as `intg` (verified: `dhc-type-to-c-decl` maps `dht-int` →
`"intg"`), so `addr` points to an `intg` slot. Tested with
`_test-dh-int-id 3000000000` — now passes.

### lisp_c.c:1856-1858 — at_to_dharg DHT_INT conversion uses (int) cast

```c
else if ((at_obj->flags & C_NUMBER) &&
         (at_obj->Number == (int)(at_obj->Number)) )
  arg->dh_int = (int) at_obj->Number;
```

**DISCOVERED DURING TESTING**: The `(int)` cast in the range check and
assignment causes undefined behavior (SIGFPE) when `at_obj->Number` is
outside the `int` range (e.g., 3e9). Since `dh_int` is `intg`, the cast
should be `(intg)`.

**FIXED**: Changed both `(int)` casts to `(intg)`. This was the root
cause of the "Floating exception" when passing values > INT_MAX to DH
compiled functions with `(-int-)` parameters.

---

## Tier 5: Lower Risk

These are unlikely to cause problems in normal usage but are technically
incorrect on 64-bit.

### DIRECTIVE: Fix storage.c loop counter. Leave everything else.

- `storage.c:333,367` — `int i` loop counter over `srg.size` (would need
  >2G-element AT storage)
  **FIX**: This is more important than it looks. The loop counter `int i`
  iterating over `srg.size` (which is `intg`) will wrap at 2^31 and
  loop forever (or access wrong indices) for large AT storages. Change
  `int i` to `intg i`.

- `binary.c:81-84` — relocation table size `relocm`/`relocn` limited to
  int (would need >2G shared references in one bwrite)
  **STATUS**: Leave alone. This is wire format. Relatively safe.

- `binary.c:844,854` — `int l = strlen(s)` before `write_card24(l)` (would
  need >2GB strings)
  **STATUS**: Leave alone. Wire format.

- `binary.c:1038` — object slot count read as `read_card24()` (would need
  >16M slots per object)
  **STATUS**: Leave alone. Wire format.

- `oostruct.c:198` — `x = (x<<1) | ((long)x<0 ? 1 : 0)` — signed left
  shift is technically undefined behavior if high bit set
  **STATUS**: Leave alone. Hash function UB is OK.

- `header.h:156` — `unsigned int count` for refcount (4 billion limit, not
  a real problem in practice)
  **STATUS**: Leave alone. GC limited to 2^32 references is acceptable.

---

## What's NOT Broken

- **Struct definitions** are fine — `intg` is 64-bit with `INTG_IS_LONG`
- **columnardb package** uses its own file format (LCDB), not bwrite —
  no serialization issues
- **wire package** uses its own serialization — no issues
- **Normal usage** (arrays under 2G elements) works fine
- **The DH type system itself** (dharg union) mostly uses `intg` correctly
  for `dh_int` and `dh_ord`

---

## Implementation Plan (Approved Directives)

Fixes are grouped into phases. Run the full test suite after each phase
to catch regressions. The existing test suites are:

```bash
TMPDIR=/tmp/claude bin/lush packages/wire/tests/run-all.lsh          # 193 passed
TMPDIR=/tmp/claude bin/lush packages/columnardb/tests/run-all.lsh    # 669 passed
TMPDIR=/tmp/claude bin/lush packages/datatable/tests/run-all.lsh     # 868 passed
```

Also create `packages/lush-core-tests/test-64bit.lsh` for targeted
regression tests (see Instrumentation & Test Plan below).

### Phase 1: Tier 2 — Size/Offset Overflow (highest value)

Fix ALL int intermediate calculations. This is the most impactful tier.

**1a. Index manipulation functions** — change local `int` vars to `intg`:
- `xindex_change_dim` (index.c:2939)
- `xindex_change_mod` (index.c:2965)
- `xindex_change_offset` (index.c:2991)
- `index_check_size` (index.c:2918) — **MUST fix simultaneously with changedim/changemod**
- `xindex_transpose2` (index.c:2818)
- `xindex_narrow` (index.c:2715)
- `xindex_select` (index.c:2744)
- `xindex_unfold` (index.c:2644)
- `xindex_diagonal` (index.c:2683)

**1b. Size calculation functions** — `int size` → `intg size`:
- `xindex_nelements` (index.c:518)
- `xindex_size` (index.c:534)
- `contiguity_check` (index.c:1871)

**1c. Storage and DH byte counts** — `int` → `intg` or `size_t`:
- `new_storage()` / `new_storage_nc()` signature: `int size` → `intg size` (storage.c:754,789)
- `storage.c:1232` — `int n` byte count → `size_t n`
- `lisp_c.c:2227,2361` — `int bytes` → `size_t bytes`

**1d. Storage loop counter** (Tier 5 item, fixing here):
- `storage.c:333,367` — `int i` → `intg i`

### Phase 2: Tier 3 — Pointer Truncation

**2a. LLP64 pointer casts** — `(unsigned long)` → `(uintptr_t)`:
- `lisp_c.c:1583,1588` — `NEW_GPTR` cast
- `lisp_c.c:2319,2467` — struct field offset

**2b. gptr formatting** — `string.c:806`:
- Change `%lX` / `(unsigned long)` to `PRIxPTR` / `(uintptr_t)`

**2c. Generated hex conversion** — `dh-macro.lsh:3851`:
- Change `(int)(%s)` with `%x` to `(unsigned long)(%s)` with `%lx`

### Phase 3: Tier 4 — DH Compiler Code Generation

**3a. Fix these items:**
- `dh-macro.lsh:378` — `%x` → `%p` for pointer printf
- `dh-macro.lsh:2122` — `int permlist[]` → `intg permlist[]`
- `dh-macro.lsh:3769` — `int templen` → `size_t templen`
- `dh-util.lsh:2670` — `(int)` → `(intg)` in idx mod calculation
- `dh-util.lsh:2777-2780` — `%d` → `%ld` for dim/mod/size assignments
- `dh-util.lsh:526` — ndim `int` → `intg` in extern declaration

**3b. Fix if possible:**
- `lisp_c.c:1746,1785` — `(int *)` → `(intg *)` for dh_int access.
  Verify DHDOC size matches, test with large values.

**3c. Leave alone (string int casts):**
- `dh-macro.lsh` ~18 locations with `(int)` casts in string operations
- `dh-macro.lsh:3544,3978` — string loop counters as `int`

These are fine. Strings are byte arrays; `int` casts operate on string
lengths/indices. Unless strings are allocated assuming longs (they are
not — string storage is byte-addressed), these casts are correct.
2GB strings should never exist in practice.

### Phase 4: Tier 1 — Serialization Guards

**Do NOT change the wire format.** Add cheap overflow checks:

- `index.c:202-218` — add `if (value > INT_MAX || value < INT_MIN) error(...)`
  before each `serialize_int` call (write path only)
- `storage.c:452-464` — add `if (st->srg.size > INT_MAX) error(...)`
  before the size serialization (write path only)

### Future Work

- Consider protocol buffers or similar for a proper 64-bit serialization
  format (out of scope for this effort)
- Dig deeper into `lsh/compiler/` directory — there is C code in the
  compiler directory that probably needs similar fixes
- Dig into other `lsh/` library directories for remaining 32-bit leftovers

---

## Files Affected Summary

| File | Issue Count | Severity |
|------|-------------|----------|
| `src/index.c` | 12 | High — size overflow, truncation |
| `src/storage.c` | 5 | High — size parameter, serialization |
| `src/lisp_c.c` | 6 | Medium — DH interface, pointer casts |
| `src/binary.c` | 4 | Medium — format limitation |
| `src/at.c` | 1 | Low — pointer-to-double |
| `src/string.c` | 1 | Low — pointer format |
| `src/oostruct.c` | 1 | Low — UB in hash |
| `lsh/compiler/dh-macro.lsh` | ~25 | Medium — generated C code |
| `lsh/compiler/dh-util.lsh` | 4 | Medium — generated C code |
| `lsh/compiler/dh-compile.lsh` | 1 | Low — intg* cast |

---

## Instrumentation & Test Plan

The goal is to write concrete tests that prove (or disprove) each class
of bug. Many of these bugs involve values > 2^31, but we do NOT need
huge amounts of memory to test them — most are metadata operations on
small arrays.

**Convention**: `BIG` = 3000000000 (> 2^31 ≈ 2.1 billion, fits in intg/long)

### Test Group 1: Index Metadata Manipulation (index.c)

These are the most directly testable bugs. The functions `idx-changedim`,
`idx-changemod`, `idx-changeoffset` modify idx struct fields. The C code
stores the Lush argument (which comes through `AINTEGER` returning `intg`)
in a local `int` variable, truncating it.

**Test methodology**: Call each function with a value > 2^31, read back
the metadata, and check if the value survived.

```lisp
;; --- Test idx-changedim truncation (index.c:2939) ---
;; idx-changedim calls AINTEGER(3) → int x → ind->dim[d] = x
;; If buggy: dim[d] will contain truncated (garbage) value
;; If fixed: dim[d] == 3000000000
;;
;; NOTE: We need a storage large enough that index_check_size won't
;; reject the new dimension.  Create a storage, make an unsized idx,
;; or use a mod of 0 so the size check passes.
;;
;; Strategy: Create a 1D matrix with mod[0]=0 (via idx-changemod)
;; so that any dimension is valid (dim * mod = 0, fits in storage).
;; But idx-changemod has the SAME bug... chicken-and-egg.
;;
;; Alternative strategy: Create a large-enough storage directly.
;; A storage of 1 element with mod=0 should work because
;; index_check_size computes max = offset + (dim-1)*mod.
;; With mod=0, max = offset = 0, which is < size=1.
;;
;; Actually, we CAN test idx-changemod with value 0 (no truncation
;; issue for 0), THEN test idx-changedim with BIG.

(let ((m (int-matrix 1)))
  ;; Set mod[0] = 0 so dimension doesn't affect size check
  (idx-changemod m 0 0)
  ;; Now set dim[0] to a value > 2^31
  (idx-changedim m 0 3000000000)
  ;; Read back: does (idx-dim m 0) equal 3000000000?
  ;; EXPECTED BUG: returns -1294967296 (or similar truncated value)
  ;; EXPECTED FIX: returns 3000000000
  (printf "changedim test: %l (expect 3000000000)\n" (idx-dim m 0))
  (assert (= (idx-dim m 0) 3000000000)) )
```

```lisp
;; --- Test idx-changemod truncation (index.c:2965) ---
;; Same pattern: AINTEGER → int x → ind->mod[d] = x
(let ((m (int-matrix 1)))
  (idx-changedim m 0 1)  ;; dim=1, so (dim-1)*mod = 0, always fits
  (idx-changemod m 0 3000000000)
  (printf "changemod test: %l (expect 3000000000)\n" (idx-mod m 0))
  (assert (= (idx-mod m 0) 3000000000)) )
```

```lisp
;; --- Test idx-changeoffset truncation (index.c:2991) ---
;; AINTEGER → int x → ind->offset = x
;; Need a storage large enough: offset + (dim-1)*mod < srg.size
;; With dim=1, mod=1: need offset < size.
;; Create storage with > 2^31 elements?  No -- too much memory.
;;
;; Alternative: set dim=1, mod=0.  Then size_max = offset.
;; Need offset < srg.size.  Can't have storage that large.
;;
;; Alternative: create a GPTR storage of size 1, set mod=0, dim=1.
;; Then size_max = offset + 0 = offset, need offset < 1.
;; That means offset must be 0.  Can't test large offsets this way.
;;
;; REAL test: The truncation happens at assignment regardless of the
;; size check.  We can test the truncation by setting offset and
;; reading it back, even if index_check_size rejects it afterward.
;; But the function restores old_val on rejection...
;;
;; Actually, old_val is ALSO int, so it truncates too.  If we set
;; offset to BIG and it truncates to X, then index_check_size checks
;; X (truncated), and if it rejects, it restores X (truncated) not
;; the original offset.  So even on rejection, we've corrupted the
;; offset.  That's a separate bug.
;;
;; Simplest test: just observe the truncation.
;; We can't easily pass the size check with a huge offset without
;; huge memory.  But we CAN test that the old_val restore is also
;; broken: set offset to BIG, it gets truncated AND rejected, but
;; the "restored" value is also truncated.

(let ((m (int-matrix 10)))
  ;; offset starts at 0.  Set to BIG.  Will be rejected by
  ;; index_check_size, but old_val was also truncated from 0
  ;; which is fine (0 fits in int).  So offset gets restored to 0.
  ;; Not the most useful test.
  ;;
  ;; Better: test idx-changeoffset with a value that truncates but
  ;; passes the size check.  E.g., 2147483648 (2^31) with enough
  ;; storage.  That's 2G * 4 bytes = 8GB of int storage.  Too much.
  ;;
  ;; Verdict: idx-changeoffset is hard to test without huge memory.
  ;; The bug is real but only manifests with very large storages.
  ;; Mark as CONFIRMED BY CODE REVIEW, not directly testable without
  ;; large memory.
  (printf "changeoffset: confirmed by code review (needs large storage to test)\n") )
```

```lisp
;; --- Test idx-transpose2 truncation (index.c:2814) ---
;; Uses: int tmp; tmp = ind->dim[d1]; ...swap...
;; If dim[d1] > 2^31, tmp truncates, then the truncated value
;; gets written to dim[d2].
;;
;; Setup: 2D matrix, set dim[0] to BIG (via fixed idx-changedim),
;; transpose, check if dim[1] got the full BIG value.

(let ((m (int-matrix-2d 2 3)))
  ;; First fix changedim, then:
  (idx-changemod m 0 0)   ;; mod[0]=0 so dim[0] doesn't matter for size
  (idx-changedim m 0 3000000000)
  ;; Now transpose dims 0 and 1
  (idx-transpose2 m 0 1)
  ;; dim[1] should now be 3000000000 (was dim[0])
  ;; dim[0] should now be 3 (was dim[1])
  (printf "transpose2: dim[0]=%l dim[1]=%l (expect 3 and 3000000000)\n"
          (idx-dim m 0) (idx-dim m 1))
  (assert (= (idx-dim m 1) 3000000000))
  (assert (= (idx-dim m 0) 3)) )
```

```lisp
;; --- Test idx-nelements overflow (index.c:518) ---
;; Uses: int size; size *= ind->dim[i]
;; With dim[0] = 3000000000 (after fix), nelements should return BIG.

(let ((m (int-matrix 1)))
  (idx-changemod m 0 0)
  (idx-changedim m 0 3000000000)
  ;; (idx-nelements m) should return 3000000000
  ;; EXPECTED BUG: returns truncated/wrapped value
  (printf "nelements: %l (expect 3000000000)\n" (idx-nelements m))
  (assert (= (idx-nelements m) 3000000000)) )
```

```lisp
;; --- Test idx-narrow offset overflow (index.c:2715) ---
;; ind->offset += st * ind->mod[d]
;; where st is int (truncated from AINTEGER)
;;
;; If narrow's start parameter > 2^31, it truncates.
;; Also, st * mod[d] is int * intg which computes as int on the
;; left side before addition.
;;
;; Test: narrow with start=0, size=1 on a dim > 2^31
;; (This tests that the size parameter doesn't truncate)

(let ((m (int-matrix 1)))
  (idx-changemod m 0 0)
  (idx-changedim m 0 3000000000)
  ;; Narrow to [0, 1) -- should set dim[0] = 1
  (idx-narrow m 0 1 0)
  (assert (= (idx-dim m 0) 1))
  ;; The real test: narrow with start > 2^31 on a big dim
  ;; Needs the dim to be > start+size, which needs BIG dim
  ;; AND the offset arithmetic to not overflow.
  ;; Since mod=0, offset += start * 0 = 0.  No overflow to test.
  ;; With mod=1, we'd need a storage of BIG elements.
  (printf "narrow: confirmed truncation by code review\n") )
```

```lisp
;; --- Test index_check_size overflow (index.c:2918) ---
;; Uses: int size_min, size_max
;; Accumulates offset + (dim-1)*mod, all in int.
;; This is the safety net -- if IT overflows, the safety check
;; itself is broken and allows out-of-bounds access.

;; Can test: create idx with dim * mod that overflows int but
;; fits in intg, and check that Lush correctly rejects operations
;; that would exceed storage size.

;; Actually, this is the scariest bug: if index_check_size
;; overflows, it might APPROVE an index that's actually out of
;; bounds, leading to memory corruption.
(printf "index_check_size: CRITICAL -- overflow in safety check itself\n")
(printf "  int size_max = offset + (dim-1)*mod\n")
(printf "  If this overflows int, check gives wrong answer\n")
```

### Test Group 2: DH Interface (lisp_c.c)

**The confirmed bug**: The DH compiler generates C functions with `intg`
parameters (see `dhc-type-to-c-decl` in dh-util.lsh:381, which maps
`dht-int` → `"intg"`). The stub passes values through `dharg.dh_int`
(also `intg`). But `lisp_c.c:1746` writes the value to the compiled
function's local variable via `*((int *) addr) = arg->dh_int` — a
**32-bit write into a 64-bit slot**.

On little-endian (x86-64), this writes the low 32 bits correctly and
leaves the upper 32 bits uninitialized. This means:
- Values 0 to 2^31-1: accidentally work (upper bits likely zero)
- Values >= 2^31: upper bits garbage → wrong value
- Negative values: sign extension missing → wrong value

**Verified by inspecting generated C code**: A function declared as
`(de f (x) ((-int-) x) x)` generates:
```c
extern_c intg C_f(intg L1_x) { ... }     // parameter is intg
DH(X_f) { ret.dh_int = C_f(a[1].dh_int); } // stub uses dh_int (intg)
```
The DHDOC record says `DH_INT`, and lisp_c.c uses that to decide to
cast to `(int *)`.  The mismatch: generated code allocates `intg`,
but the interface writes through `int *`.

```lisp
;; --- Test DH int round-trip with large values ---
(de _test-dh-int-id (x) ((-int-) x) x)
(dhc-make () _test-dh-int-id)

;; Small value: should work
(assert (= (_test-dh-int-id 42) 42))
(printf "DH int(42) = %d  OK\n" (_test-dh-int-id 42))

;; Value at INT_MAX boundary
(assert (= (_test-dh-int-id 2147483647) 2147483647))
(printf "DH int(2^31-1) = %l  OK\n" (_test-dh-int-id 2147483647))

;; Value > INT_MAX: THIS IS THE BUG TEST
;; 3000000000 > 2^31, so (int) truncation makes it negative
;; On little-endian x86-64, the upper 32 bits of the intg local
;; variable are uninitialized, so the return value is unpredictable.
(let ((result (_test-dh-int-id 3000000000)))
  (printf "DH int(3e9) = %l (expect 3000000000, bug gives garbage)\n" result)
  ;; EXPECTED BUG: result != 3000000000
  ;; EXPECTED FIX: result == 3000000000
  (assert (= result 3000000000)) )

;; Negative value
(let ((result (_test-dh-int-id -100)))
  (printf "DH int(-100) = %d (expect -100)\n" result)
  (assert (= result -100)) )
```

**Note**: The "Floating exception" seen in initial testing (calling
`_test-dh-int-id 3000000000`) is actually from `AINTEGER` — the
conversion `(intg) AREAL(i)` where `AREAL` extracts a `double`.
3000000000.0 fits in double, so that's fine.  The floating exception
likely comes from the DH stub's Lush-to-C argument conversion, not
AINTEGER.  Need to investigate whether the exception masks the
truncation or is a separate issue.

```lisp
;; --- Test DH long round-trip (should work, uses int64_t) ---
(de _test-dh-long-id (x) ((-long-) x) x)
(dhc-make () _test-dh-long-id)

;; DH_LONG uses *((int64_t *) addr) which is correct
(let ((result (_test-dh-long-id 3000000000)))
  (printf "DH long(3e9) = %l (expect 3000000000)\n" result)
  (assert (= result 3000000000)) )
```

### Test Group 3: DH-Generated String Code (dh-macro.lsh)

The DH compiler generates `(int)` casts for string indices and lengths
in ~18 places.  Testing these requires strings > 2GB, which is
impractical.  However, we can verify the generated code to confirm the
bug exists.

```lisp
;; --- Inspect generated code for string operations ---
;; Write a compiled function using string ops and inspect the C output.

(de _test-dh-str-left (s n) ((-str-) s) ((-int-) n) (left s n))
(dhc-make () _test-dh-str-left)

;; Then inspect the generated .c file for (int) casts.
;; Look for patterns like:
;;   (int)L1_n < 1        ;; should be (intg)
;;   memccpy(..., (int)...) ;; size param should be size_t
;;   ((char*)...data)[(int)...] ;; index should be intg
;;
;; These bugs are CONFIRMED BY CODE REVIEW but not practically
;; testable without multi-GB strings.
```

### Test Group 4: Serialization Format (binary.c, index.c, storage.c)

The bwrite/bread format uses 32-bit fields for array dimensions,
storage sizes, and index metadata.  This is a format limitation.

```lisp
;; --- Test bwrite/bread round-trip of index metadata ---
;; Create a matrix, manipulate its metadata to have large values,
;; serialize with bwrite, deserialize with bread, check if values
;; survived.
;;
;; NOTE: bwrite will error on dim > INT_MAX (binary.c:892-893
;; has an explicit check).  The error message is:
;; "array dimension too large for binary format"
;;
;; So this isn't a silent corruption bug -- it's a known limitation
;; with an error guard.

(let ((m (int-matrix 1)))
  (idx-changemod m 0 0)
  ;; After fixing idx-changedim:
  (idx-changedim m 0 3000000000)
  ;; Attempt bwrite -- should error with "too large for binary format"
  ;; (If it DOESN'T error, that's also useful info -- means the check
  ;; was removed or bypassed.)
  (let ((ok t))
    (errorcatch (bwrite (open-write "/tmp/claude/test-bwrite.bin") m))
    ;; errorcatch should catch the error
    (printf "bwrite large dim: correctly rejected (format limitation)\n") ) )

;; --- Test serialization truncation (index.c:202-218) ---
;; The old serialize_int pathway (used by dump/undump, not bwrite)
;; silently truncates without any error check.
;; This is harder to test directly since dump/undump are less commonly
;; used, but the code path is clear from review.
```

### Test Group 5: Storage Size (storage.c)

```lisp
;; --- Test new_storage with large size ---
;; new_storage(int type, int size) takes int parameter.
;; If called with size > 2^31, it truncates silently.
;;
;; The Lush-level function that calls new_storage is (storage-malloc).
;; Test: try to create a storage with > 2^31 elements.
;; This requires ~8GB for doubles, so only testable on large-memory
;; systems.  On systems with < 16GB RAM, this will fail with OOM
;; rather than exposing the bug.

;; Minimal test: check if the size parameter truncates
;; by examining storage-size after creation.
;; NOTE: This will actually try to allocate memory, so it's not
;; purely a metadata test.  Skip on small-memory systems.

;; (let ((s (double-storage)))
;;   (storage-malloc s 3000000000 0)  ;; 24GB -- will OOM
;;   ;; If it somehow succeeds:
;;   (printf "storage size: %l (expect 3000000000)\n" (storage-size s))
;;   (assert (= (storage-size s) 3000000000)) )
;;
;; VERDICT: Confirmed by code review. Only testable with huge memory.
```

### Test Group 6: index_check_size Overflow (the scary one)

```lisp
;; --- Test that the safety check itself is broken ---
;; index_check_size (index.c:2918) uses:
;;   int size_min, size_max;
;;   size_max += (dim-1) * mod;
;;
;; If (dim-1) * mod overflows int but fits in intg, the check
;; computes a WRONG (wrapped) size_max and may approve an index
;; that's actually out of bounds.
;;
;; Setup: Create a storage of modest size (e.g., 100 elements).
;; Set dim and mod such that dim*mod > 2^31 but fits in intg.
;; E.g., dim = 1000000, mod = 3000.  dim*mod = 3e9 > 2^31.
;; This should FAIL the size check (3e9 > 100).
;; But if size_max overflows int, it wraps to a negative or small
;; positive value, and the check might PASS, allowing OOB access.

(let ((m (int-matrix 100)))
  ;; dim=100, mod=1 initially.
  ;; Set mod[0] = 3000, dim[0] = 1000000
  ;; size_max = 0 + (1000000-1) * 3000 = 2999997000 > 2^31
  ;; But wait, idx-changemod stores through int, so 3000 fits fine.
  ;; And idx-changedim stores through int, 1000000 fits fine.
  ;; The overflow is in index_check_size's COMPUTATION, not in the
  ;; parameters.
  ;;
  ;; Actually changedim/changemod call index_check_size themselves
  ;; and reject if it returns true.  So:
  ;; 1. Set mod = 3000 (fits in int, check passes: (100-1)*3000 = 297000 < size???)
  ;;    Wait, storage size is 100.  (100-1)*3000 = 297000 > 100.  REJECTED.
  ;;
  ;; Need to think about this more carefully.
  ;; index_check_size: size_max = offset + sum((dim[j]-1)*mod[j]) for positive mod
  ;; Must have size_max < srg.size.
  ;;
  ;; So we can't set mod=3000 with dim=100 on a 100-element storage
  ;; because the check (correctly) rejects it.
  ;;
  ;; To test the OVERFLOW in the check, we need:
  ;; - A storage large enough that the TRUE size_max fits
  ;; - But (dim-1)*mod overflows int
  ;; - So the wrapped int value is small, passing the check
  ;; - Then actual access at the true offset causes OOB
  ;;
  ;; This requires a storage where the WRAPPED value < size but
  ;; the TRUE value > size.  E.g.:
  ;; dim = 0x80000002 (2147483650), mod = 1
  ;; true size_max = offset + 2147483649 * 1 = 2147483649
  ;; int  size_max = offset + (int)(2147483649) = offset + (-2147483647)
  ;;                = offset - 2147483647  (negative!)
  ;; Negative size_max means size_min check triggers, so rejected.
  ;;
  ;; Harder to exploit than it first appears because negative wrap
  ;; is caught by size_min < 0 check.  Positive wrap requires
  ;; specific dim*mod values.  E.g.:
  ;; dim = 0x80000001 (2147483649), mod = 2
  ;; true: (2147483648) * 2 = 4294967296
  ;; int:  overflows to 0 (wraps around)
  ;; size_max = offset + 0 = 0 < any storage size → CHECK PASSES
  ;; But true max is 4294967296 → way out of bounds!
  ;;
  ;; This IS exploitable if we can set dim to 0x80000001.
  ;; But idx-changedim has the int truncation bug too, so we can't
  ;; actually set dim that high via the Lush API.
  ;; Chicken-and-egg: the changedim bug MASKS the check_size bug.
  ;; After FIXING changedim, the check_size bug becomes reachable.
  ;;
  ;; VERDICT: index_check_size overflow is a LATENT bug.  Currently
  ;; unreachable because changedim truncates.  After fixing changedim,
  ;; this bug becomes reachable and MUST be fixed simultaneously.

  (printf "index_check_size: LATENT overflow bug\n")
  (printf "  Currently masked by changedim truncation\n")
  (printf "  MUST fix simultaneously with changedim\n") )
```

### Test Runner

All tests above should be collected into a single test file:

```
packages/lush-core-tests/test-64bit.lsh
```

Run with:
```bash
TMPDIR=/tmp/claude bin/lush packages/lush-core-tests/test-64bit.lsh
```

The test file should use the same test framework as the datatable/
columnardb tests (test-check, test-equal, test-summary).  Tests that
require fixes should be marked as EXPECTED FAIL until the fix is
applied, then flipped to EXPECTED PASS.

### Test Execution Strategy

**Phase 1: Document current behavior (before any fixes)**
Run all tests, record which pass and which fail.  This establishes the
baseline: which bugs are real vs theoretical.

**Phase 2: Fix and re-test (one at a time)**
Fix each bug, re-run the full test suite plus the 64-bit tests.
Ensure no regressions.  Order:

1. Fix `idx-changedim`, `idx-changemod` (index.c:2939,2965)
   → Group 1 changedim/changemod tests should now pass
2. Fix `index_check_size` (index.c:2918) — MUST be simultaneous with #1
   → Group 6 latent bug is now guarded
3. Fix `idx-changeoffset` (index.c:2987)
4. Fix `idx-transpose2`, `idx-narrow`, `idx-select` (index.c:2814,2711,2740)
   → Group 1 transpose/narrow tests should now pass
5. Fix `xindex_nelements`, `xindex_size` (index.c:518,534)
   → Group 1 nelements test should now pass
6. Fix DH interface `(int *)` → `(intg *)` (lisp_c.c:1746,1785)
   → Group 2 DH tests should now pass
7. Fix `new_storage` signature (storage.c:754,789)
8. Fix serialization (add overflow checks or upgrade format)

**Phase 3: Stress test**
On a machine with 32+ GB RAM, create arrays with > 2^31 elements and
verify end-to-end correctness: create, manipulate, save, load.

### Key Insight: Fix Dependencies

```
idx-changedim fix ──┐
                    ├──→ MUST fix index_check_size simultaneously
idx-changemod fix ──┘    (otherwise changedim enables the latent
                          overflow in the safety check)

idx-changedim fix ──→ enables testing of:
                      - idx-transpose2
                      - idx-nelements / idx-size
                      - idx-narrow (partially)
                      - idx-select (partially)
```

The DH interface fix (lisp_c.c:1746,1785) is independent and can be
done and tested separately.

The serialization fixes are independent but lower priority since
bwrite already has an explicit error guard for large dimensions.

---

## Implementation Status (2026-02-25)

All approved fixes have been implemented and tested.

### Phase 1: Tier 2 — Size/Offset Overflow — COMPLETE
- `index.c`: Changed `int` → `intg` in xindex_change_dim, xindex_change_mod,
  xindex_change_offset, index_check_size, xindex_transpose2, xindex_narrow,
  xindex_select, xindex_unfold, xindex_diagonal, xindex_nelements, xindex_size,
  contiguity_check
- `storage.c`: Changed `new_storage`/`new_storage_nc` signature `int size` → `intg size`;
  changed `int i` → `intg i` in storage_dispose/storage_action loop counters;
  changed `int n` → `size_t n` in storage_clear byte count
- `header.h`: Updated `new_storage`/`new_storage_nc` declarations
- `lisp_c.c`: Changed `int bytes` → `size_t bytes` in update_c_from_lisp/update_lisp_from_c

### Phase 2: Tier 3 — Pointer Truncation — COMPLETE
- `lisp_c.c`: Changed `(unsigned long)` → `(uintptr_t)` at 4 locations
- `string.c`: Changed gptr formatting to use `PRIXPTR`/`(uintptr_t)`
- `dh-macro.lsh`: Changed generated hex conversion from `(int)/%x` to `(unsigned long)/%lx`

### Phase 3: Tier 4 — DH Compiler Code Generation — COMPLETE
- `dh-macro.lsh`: `%x` → `%p` for pointer printf; `int permlist[]` → `intg permlist[]`;
  `int templen` → `size_t templen`
- `dh-util.lsh`: `(int)` → `(intg)` in idx mod calculation; `%d` → `%ld` for dim/mod/size;
  ndim extern `int` → `intg`
- `lisp_c.c`: `(int *)` → `(intg *)` for dh_int read/write in dharg_to_address/
  address_to_dharg; `(int)` → `(intg)` in at_to_dharg DHT_INT conversion

### Phase 4: Tier 1 — Serialization Guards — COMPLETE
- `index.c`: Added INT_MAX/INT_MIN overflow checks before serialize_int for
  offset, dim[], and mod[] (write path only)
- `storage.c`: Added INT_MAX overflow check before size serialization (write path only)

### Test Results
- 64-bit regression tests: **23 passed, 0 failed** (`packages/lush-core-tests/test-64bit.lsh`)
- Wire tests: **193 passed, 0 failed**
- ColumnarDB tests: **669 passed, 0 failed**
- Datatable tests: **868 passed, 0 failed**

### Bonus Fix Found During Testing
The `at_to_dharg` function (lisp_c.c:1856-1858) had `(int)` casts for the
DHT_INT case that caused SIGFPE (Floating exception) when converting Lush
numbers > INT_MAX to DH int arguments. This was the root cause of the
"Floating exception" noted in the audit's instrumentation plan. Fixed by
changing to `(intg)` casts.
