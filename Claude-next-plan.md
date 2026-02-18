# Lush Post-64bit-Audit Improvement Plan

Follow-on improvements identified during the 64-bit audit (Phases 0-6).
Ranked by **risk** (chance of breaking something) and **payoff** (value of the fix).

---

## Tier 1: Low Risk, High Payoff

These are small, isolated changes that fix real bugs or eliminate build warnings
without touching core logic.

### 1.1 Fix remaining format-string warnings in index.c and storage.c

**Risk:** Very low -- only changes printf format specifiers.
**Payoff:** Eliminates the 6 remaining build warnings (2 in index.c, 4 in storage.c).

**Files:** `src/index.c`, `src/storage.c`

**index.c** (lines 129, 133, 2065, 2067):
- `ind->ndim` is `short` -- `%d` is fine, but `ind->dim[d]` is `intg` (= `long`).
- Line 133: `sprintf(s, "%dx", ind->dim[d])` -- `%d` should be `%ld` (or use a
  `PRINT_INTG` macro that expands to `%ld` when `INTG_IS_LONG`, `%d` otherwise).
- Line 2067: `fprintf(f, " %d", ind->dim[j])` -- same fix.
- Line 2065: `fprintf(f, ".MAT %d", ind->ndim)` -- `ndim` is `short`, `%d` is OK.

**storage.c** (lines 390-399):
- `srg->size` is now `intg`. The `%d` format should become `%ld`.
- Also, the `%lx` for the pointer should ideally use `PRIxPTR`.

**Approach:** Define a portable format macro in `include/define.h`:
```c
#ifdef INTG_IS_LONG
#define FMT_INTG "ld"
#else
#define FMT_INTG "d"
#endif
```
Then replace `%d` with `%" FMT_INTG "` in the affected format strings.

### 1.2 Fix `dh_obj_ptr` type in `dh.h`

**Risk:** Low -- only changes the pointer type in the `dharg` union.
**Payoff:** Correctness for object pointers on 64-bit. Currently `int *dh_obj_ptr`
means compiled Lush code accesses objects through a 4-byte pointer type.

**File:** `include/dh.h` (line 224)

**Change:** `int *dh_obj_ptr` -> `intg *dh_obj_ptr`

**Verification:** Search for all uses of `dh_obj_ptr` in the compiler and runtime
to ensure they're compatible with `intg *`. The compiler emits `(intg *)` casts
already (Phase 3 fix in `dh-compile.lsh:413`), so this aligns the declaration.

### 1.3 Fix `dhclassdoc_s.size` type

**Risk:** Low -- only changes a struct field type.
**Payoff:** Eliminates `-Wconversion` warning and ensures `sizeof()` result
isn't truncated.

**File:** `include/dh.h` (line 308)

**Change:** `int size` -> `size_t size`

**Also change:** `int nmet` -> `int nmet` (leave as-is; method count fits in `int`).

### 1.4 Replace `sprintf` with `snprintf` in critical paths

**Risk:** Low -- `snprintf` is a drop-in with an added length parameter.
**Payoff:** Prevents buffer overflows from long symbol names or file paths.

**Files:** `src/at.c`, `src/index.c`, `src/storage.c`, `src/module.c`

**Approach:** Systematic search for `sprintf(string_buffer, ...)` and replace with
`snprintf(string_buffer, STRING_BUFFER, ...)`. Also fix `sprintf(s, ...)` in
`index.c` where `s` is a local buffer.

Do NOT touch `dldbfd.c` -- that file has deeper issues (see Tier 3).

### 1.5 Fix `%s` placeholder macros in `idxmac.h`

**Risk:** Very low -- these macros are dead code (never expanded by the C compiler).
**Payoff:** Cleanup. If anyone ever tries to use `Midx_contiguep4` through
`Midx_contiguep8`, they'll get a compile error from the literal `%s`.

**File:** `include/idxmac.h` (lines 356-379)

**Change:** Replace `(%s)` with `(idx)` to match the macro parameter name.
Also add missing semicolons after `var = 1` (should be `var = 1;`).

---

## Tier 2: Medium Risk, High Payoff

These touch more code paths but fix significant correctness issues.

### 2.1 Fix permutation index macros to use `intg` instead of `long`

**Risk:** Medium -- changes pointer type used to read permutation array data.
**Payoff:** Correctness. Currently `Midx_m1permute` and `Midx_m2permute` hardcode
`long *` to read permutation indices. If the permutation idx stores `int` (ST_I32)
elements, this reads 8 bytes per element instead of 4 on LP64, producing garbage.

**File:** `include/idxops.h` (lines 1654, 1658, 1671-1672, 1678)

**Change:** `long *c2` -> `intg *c2` and `IDX_PTR((per), long)` -> `IDX_PTR((per), intg)`.

**Rationale:** `intg` matches the type that Lush `int-matrix` uses for its elements
via the ST_I32/int storage path. However, this needs careful verification: what
storage type do permutation indices actually use? If they use `int-matrix` (ST_I32),
then on LP64 where `intg` is `long` (8 bytes), using `intg *` would still be wrong.
The real fix may require `int *` (matching ST_I32) or adding proper type dispatch.

**Pre-work:** Trace how permutation matrices are created in the Lush standard library
to determine their actual storage type before deciding the correct pointer type.

### 2.2 Fix `.MAT` file format for 64-bit dimensions

**Risk:** Medium -- changes serialization format.
**Payoff:** Correctness for matrices with dimensions > 2^31.

**File:** `src/index.c` (lines 2065-2067 write, lines 2098+ read)

The `.MAT` format writes dimensions with `%d` (32-bit). On 64-bit, `ind->dim[d]`
is `intg` (64-bit). Writing with `%d` silently truncates; reading with `%d` can't
recover the full value.

**Approach:** Use `%" FMT_INTG "` for writing. For reading, use `strtol()` instead
of relying on `fscanf("%d", ...)`. Add a version marker or detect large dimensions
to maintain backward compatibility with existing `.MAT` files.

**Compatibility:** Old `.MAT` files will still load fine (their dimensions fit in
`int`). New files with large dimensions will not load in old Lush -- this is
acceptable since old Lush can't handle large matrices anyway.

### 2.3 Audit and fix `-Wconversion` warnings in `string.c` (33 warnings)

**Risk:** Medium -- string operations are heavily used.
**Payoff:** The most warning-dense file. Fixing string length types from `int` to
`size_t` prevents truncation for strings > 2GB (unlikely but theoretically possible
in a Lisp system processing large data).

**File:** `src/string.c`

**Approach:** Change string length variables from `int` to `size_t` where they hold
`strlen()` results or buffer sizes. Keep `int` for character values and comparison
results. Special attention to `large_string_add()` which uses `len < 0` as a
sentinel -- this function needs the sentinel replaced with a separate flag or a
`(size_t)-1` constant before changing the parameter type.

---

## Tier 3: Higher Risk, Medium Payoff

These involve significant architectural changes or touch fragile code.

### 3.1 Modernize the module loader (replace BFD/NSLinkModule with dlopen)

**Risk:** High -- changes how compiled Lush extensions are loaded.
**Payoff:** Eliminates the biggest external dependency (libbfd), fixes PIE
incompatibility, removes MAP_32BIT limitations, and works on modern macOS.

**Files:** `src/module.c`, `src/dldbfd.c` (remove), `configure.ac`

**Approach:**
1. The `dlopen()` path already exists in `module.c` (lines 50-56).
2. Instead of linking raw `.o` files via BFD, compile them into `.so` files:
   `cc -shared -fPIC -o foo.so foo.o`
3. Load with `dlopen("foo.so", RTLD_NOW)` and resolve symbols with `dlsym()`.
4. Remove the BFD path entirely (or keep it as a configure-time option for legacy).
5. Update the compiler backend to emit the shared-object compilation step.

**Benefits:**
- Works with PIE (no need for `-fno-pie`)
- Works on modern macOS (no NSLinkModule)
- No libiberty/libbfd dependency
- No MAP_32BIT limitations
- Code model handled by the system linker

**Drawbacks:**
- Slightly different linking semantics (shared vs. relocatable objects)
- Requires a C compiler at runtime (already required by Lush)
- May need symbol visibility annotations

### 3.2 Add `ST_I64` storage type

**Risk:** High -- touches every type-switch in the idx system.
**Payoff:** Enables 64-bit integer arrays (hash values, timestamps, addresses).

**Files:** `include/idx.h`, `include/idxmac.h`, `include/idxops.h`,
`src/index.c`, `src/storage.c`, `lsh/compiler/dh-util.lsh`, `lsh/compiler/dh-macro.lsh`

**Approach:**
1. Add `ST_I64 = 9` (or next available) to the storage type enum.
2. Add `storage_type_size[ST_I64] = 8`.
3. Add type cases in all `Midx_*` macros (idxmac.h, idxops.h).
4. Add `long-matrix` or `i64-matrix` constructor in Lush.
5. Add `dht-i64` / `(-i64-)` type annotation in the compiler.
6. Update binary I/O to handle ST_I64 serialization.

This is a large change that touches ~20 files with hundreds of type-switch cases.
It should be done as a separate, dedicated project.

### 3.3 Remove dead platform support

**Risk:** Low-medium -- removing code is safe if nothing depends on it, but there
may be hidden dependencies.
**Payoff:** Reduces code noise. Dead platforms: HP-UX, IRIX, OSF/Tru64, FreeBSD 2.x,
Alpha, MIPS.

**Files:** `configure.ac`, `src/module.c` (NSLinkModule/HP-UX loader paths),
`src/unix.c`, `include/lushconf.h.in`

**Approach:** Remove configure checks for dead platforms, remove conditional code
blocks for these platforms. Keep x86_64 Linux, aarch64 Linux, and macOS (arm64/x86_64)
as the supported targets.

This is low-value work that can be deferred indefinitely.

---

## Tier 4: Research / Deferred

These need investigation before a plan can be made.

### 4.1 Address pointer-in-double architectural limitation

**Risk:** Very high -- fundamental to how Lush represents values.
**Payoff:** Future-proofs against large virtual address spaces (ARM64 LVA with 52-bit
addresses would exceed `double`'s 53-bit mantissa).

The `at` struct stores all numeric values as `double`. Pointers are cast to `long`
then to `double` for storage. A runtime check was added in Phase 4 (at.c:963-967)
to detect truncation, but the architectural issue remains.

**Options to investigate:**
1. Tagged pointer scheme (use NaN-boxing to encode pointers in double)
2. Separate pointer field in the `at` struct (ABI break)
3. Accept the limitation and document it (practical for current hardware)

This is the deepest architectural issue and should not be attempted without
extensive analysis of all code paths that store pointers as numbers.

### 4.2 Investigate `vfork` and other obsolete POSIX checks

**Risk:** Very low.
**Payoff:** Very low -- these are harmless compatibility checks.

`configure.ac` checks for `vfork` (removed from POSIX in 2008) and `RETSIGTYPE`
(irrelevant since ~1990). These can be removed but there's no urgency.

---

## Implementation Order

If implementing these improvements, the recommended order is:

1. **1.1** (format warnings) -- immediate, visible improvement
2. **1.2** (dh_obj_ptr) -- small fix with correctness impact
3. **1.3** (dhclassdoc size) -- small fix
4. **1.5** (idxmac.h %s) -- dead code cleanup
5. **1.4** (snprintf) -- safety improvement
6. **2.1** (permutation macros) -- after investigating storage types
7. **2.2** (.MAT format) -- after 1.1 is proven stable
8. **2.3** (string.c warnings) -- moderate effort
9. **3.3** (dead platforms) -- low priority cleanup
10. **3.1** (dlopen) -- major project, separate branch
11. **3.2** (ST_I64) -- major project, separate branch
12. **4.1** (pointer-in-double) -- research only for now
