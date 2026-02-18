# Lush 64-bit Cleanup: Observations and Notes

Notes on interesting, questionable, or potentially problematic things found
during the 64-bit audit of the Lush codebase. These are not part of the
immediate plan but should be kept in mind.

---

## 1. Architectural Observations

### 1.1 The `intg` migration was planned but never completed

The comment at `include/header.h:80-86` describes a two-step plan:

> 1- Replace all relevant occurences of 'int' by 'intg'.
> 2- Change the definition of intx from 'int' to 'long'.

Step 1 was partially done -- `struct srg`, `struct idx`, and `struct index`
use `intg` for size, dim, mod, and offset. But many functions that operate
on these structs still use plain `int`, and `INTG_IS_LONG` is never defined
anywhere in the build system. The comment even says "intx" instead of "intg",
suggesting this was written early and the naming changed.

### 1.2 Pointers stored in `double` (Number field)

The Lisp `at` structure stores all numeric values as `double` (`real`).
When the system needs to round-trip a pointer through a Lisp number
(e.g., `at.c:963`, `lisp_c.c:899`), it casts `void*` -> `long` -> `double`.
On 64-bit, `double` has 53 bits of mantissa, which is sufficient for
current x86_64 user-space addresses (47-48 bits) but not future-proof.
ARM64 with LVA (Large Virtual Address) can use 52 bits. This is a latent
time bomb.

### 1.3 The `tlsizeof` function returns wrong size for "long"

In `src/calls.c:199-200`:
```c
if( ! strcmp( s , "long" ) )
    return sizeof(int);        /* <-- BUG: should be sizeof(long) */
```

This means any compiled Lush code using `(to-sizeof "long")` gets 4 instead
of 8 on LP64. This could cause buffer allocation errors in compiled code
that interacts with C `long` values.

### 1.4 No `ST_I64` or `ST_LONG` storage type

The storage type system (`idx.h`, `header.h`) defines `ST_I32` mapped to
C `int` but has no 64-bit integer storage type. If someone needs to store
64-bit integers in an idx (e.g., for hash values, large counters, or
address-sized data), there is no way to do it without using `ST_D` (double)
and losing precision. Adding `ST_I64` would be a useful extension but is
a significant undertaking (every type-switch macro in `idx.h`, `idxmac.h`,
`idxops.h` would need a new case).

---

## 2. Binutils / BFD Dependency Concerns

### 2.1 BFD is an unstable internal API

The `dldbfd.c` file (3,889 lines) is the largest source file in the project
and directly accesses BFD internal structure fields: `abfd->sections`,
`p->_raw_size`, `p->_cooked_size`, `sym->flags`, `sym->value`, etc. The BFD
library was never designed as a stable external API -- it is an internal
component of GNU binutils. Every major binutils release can (and does) change
these internal layouts.

The code has compatibility shims for various BFD versions (lines 84-109),
but these only cover versions up to ~2.34. Modern binutils (2.38+) may have
further changes.

### 2.2 `bfd_log2` redefinition will break with modern binutils

`dldbfd.c:114-126` provides its own `bfd_log2()` implementation. Modern
`libbfd` exports this function, causing duplicate symbol errors at link time.
This is currently masked if the system's BFD version happens not to export it,
but is a ticking problem.

**Mitigation:** Wrap the definition in `#ifndef HAVE_BFD_LOG2` and add an
autoconf check, or simply remove it and use the BFD-provided version.

### 2.3 PIE mode incompatibility

`dldbfd.c` explicitly checks for PIE mode at compile time (lines 2496-2498):
```c
#if defined(__pie__) || defined(__pic__)
# error "DLDBFD is known to fail when compiled in PIE mode on x86_64"
#endif
```

Modern Linux distributions enable PIE by default. The build works around this
with `-fno-pie -no-pie`, which disables ASLR for the lush binary. This is a
security regression and may cause issues with distributions that mandate PIE.

**Mitigation options:**
- Accept `-fno-pie` and document the requirement
- Switch to `-mcmodel=large` (eliminates the 2GB code model limitation but
  has performance cost)
- Switch entirely to `dlopen()` instead of BFD (see 2.5)

### 2.4 MAP_32BIT memory allocation

On x86_64, `dldbfd.c` uses `mmap()` with `MAP_32BIT` to allocate memory in
the low 2GB for loaded object code (lines 1912-2037). This is required because
the default small code model uses 32-bit relative addressing. Limitations:
- `MAP_32BIT` is Linux-specific
- The low 2GB of address space is limited
- On aarch64, `MAP_32BIT` may not exist; the fallback uses plain `malloc()`
  which does not guarantee the required branch range (+/- 128MB for ARM64)

### 2.5 The `dlopen` alternative

`src/module.c` has a `dlopen()`-based loading path (lines 50-56) as an
alternative to BFD. This is more portable and stable, but requires compiling
Lush C extensions into shared objects (`.so`) rather than linking raw `.o`
files. If BFD becomes too burdensome to maintain, switching entirely to
`dlopen()` with a compilation step (`cc -shared -o foo.so foo.o`) would be
the most sustainable long-term approach.

### 2.6 macOS `NSLinkModule` is dead

The `NSLinkModule`-based loader in `module.c` (lines 84-465) uses APIs that
were deprecated in macOS 10.4 and removed in macOS 12. The code also shells
out to `system("nm ...")` and `system("cc -bundle ...")` which is fragile
and has security implications. On modern macOS, `dlopen()` is the only
viable approach.

### 2.7 libiberty dependency

The configure script checks for `libiberty` (`configure.ac:132`). This
library is increasingly hard to find as a separate package on modern systems.
It provides `xmalloc` and other utilities used by BFD. If the system's
`libbfd` is dynamically linked, `libiberty` may not be needed separately.

---

## 3. Dark Corners and Questionable Code

### 3.1 The `*(long*)&delta` type pun in `calls.c:147`

```c
float delta = (float)(p->Number - q->Number);
if (! *(long*)&delta)
```

This is inside a `#if defined(WIN32)` block, so it does not affect Linux
builds. But it reads 8 bytes from a 4-byte `float` on LP64, which is
undefined behavior. The intent is to check if a float is bitwise zero
(including negative zero), but the size mismatch makes it incorrect.

### 3.2 The `complex_hash` union in `arith.c:99`

```c
union { real r; long l[2]; } u[2];
```

On LP64, `sizeof(real)` = 8 and `sizeof(long)` = 8, so `l[2]` is 16 bytes
while `r` is 8 bytes. Accessing `u[0].l[1]` reads uninitialized memory.
This was written for ILP32 where `sizeof(long)` = 4.

### 3.3 The `%s` placeholder in `idxmac.h` macros (lines 356-379)

The `Midx_contiguep4` through `Midx_contiguep8` macros contain literal `%s`
placeholders:
```c
size *= (%s)->dim[i]; }}
```

These macros appear to be templates intended for use with `sprintf()` or
similar string formatting, not direct C macro expansion. They would produce
compile errors if used directly.

### 3.4 Permutation indices hardcode `long*` in `idxops.h`

`Midx_m1permute` and `Midx_m2permute` (lines 1654, 1671-1672) use:
```c
long *c2;
c2 = IDX_PTR((per), long);
```

This assumes the permutation idx has elements of type `long`. On LP64
`long` is 8 bytes, but if the permutation idx is `ST_I32` (4-byte `int`),
this is a type mismatch. The correct approach would be to use the storage's
actual element type.

### 3.5 The `dh_bool` and `dh_obj_ptr` fields in `dh.h`

```c
int           dh_bool;
int          *dh_obj_ptr;
```

`dh_obj_ptr` is `int*` used to represent an object reference. If objects
have fields wider than `int`, accessing through `int*` is incorrect.

### 3.6 The `dhclassdoc_s.size` field stores `sizeof()` in `int`

In `dh.h` (lines 308-309, 326), the `DHCLASSDOC` macro stores the result
of `sizeof(struct CClass_...)` into `int size`. On LP64, `sizeof()` returns
`size_t` (8 bytes), but the field is `int` (4 bytes). For structs larger
than 2GB this would overflow, but more importantly it's a type mismatch
that triggers `-Wconversion` warnings.

### 3.7 Graphics dithering hardcodes `sizeof(int)==4`

In `src/graphics.c:1391`:
```c
if (sizeof(int)!=4 && sizeof(short)!=2)
```

The dithering code (lines 1418-1466) uses `*(int*)im` with hardcoded
4-byte stride (`im += 4`). This is correct on all current LP64 platforms
where `int` is 4 bytes, but is brittle.

### 3.8 Static buffers and `sprintf` overflow risk

Several locations use fixed-size static buffers with `sprintf`:
- `dldbfd.c:248`: `static char error_buffer[256]`
- `module.c:214`: `static char buffer[512]`
- `src/at.c:1055`: `string_buffer` (1024 bytes, from `define.h`)

Symbol names or file paths longer than the buffer will overflow. These should
eventually be converted to `snprintf`.

---

## 4. Build System Notes

### 4.1 Typo in configure.ac

Line 194: `HAVE_BFD_SET_SECTOIN_SIZE_WANTS_3_ARGS` -- "SECTOIN" should be
"SECTION". This means the BFD section-size API adaptation may not work
correctly.

### 4.2 Deprecated autoconf macros

These are deprecated as of autoconf 2.70+ and will generate warnings or
errors with newer autoconf versions:
- `AC_HEADER_DIRENT`
- `AC_HEADER_SYS_WAIT`
- `TIME_WITH_SYS_TIME` (also marked obsolete in the source code itself)

### 4.3 Dead platform support

The configure script and source code contain support for platforms that
no longer exist or are irrelevant:
- HP-UX (`shl_load`, `dld` library)
- IRIX
- OSF/Tru64 (Alpha)
- FreeBSD 2.x
- Alpha architecture
- MIPS architecture

These are harmless but add noise. The relevant platforms today are
x86_64 Linux, aarch64 Linux, and possibly macOS (arm64/x86_64).

### 4.4 The `-fPIC` vs `-fpic` choice

`configure.ac:349` uses `-fPIC`, which is correct. `-fpic` has GOT size
limitations on some architectures (notably x86 with more than 8192 GOT
entries), while `-fPIC` does not. No change needed.

### 4.5 Current build produces only 6 warnings

Building with the default flags (`-Wall -O3`) produces only 6 warnings,
all related to use-after-free in malloc debugging code and file close.
This is remarkably clean for a codebase of this age.

Building with `-Wconversion` reveals 1,885 conversion warnings, of which
44 are `size_t` to `int` truncation and 276 are `long` to `double`
conversion. The `size_t` to `int` warnings are the most relevant for
64-bit cleanup.

### 4.6 Files with the most `size_t`/`int` truncation warnings

Ordered by warning count:
1. `string.c` (33) -- string length calculations
2. `x11_driver.c` (12) -- X11 API calls
3. `fileio.c` (10) -- file I/O operations
4. `graphics.c` (9) -- image buffer calculations
5. `dldbfd.c` (8) -- BFD API interactions
6. `storage.c` (7) -- storage allocation
7. `comdraw_driver.c` (6)
8. `unix.c` (4)
9. `toplevel.c` (3)
10. `index.c` (3)

---

## 5. Interesting Historical Context

### 5.1 The `RETSIGTYPE` check

`configure.ac:296` contains a manual implementation of the `RETSIGTYPE`
autoconf pattern, which checks whether signal handlers return `void` or `int`.
This was relevant for pre-POSIX.1 systems. All systems since ~1990 use
`void` signal handlers. The check is harmless but archaic.

### 5.2 The `vfork` check

`configure.ac:311` checks for `vfork`. This function was removed from
POSIX in 2008. Modern systems either provide it as an alias for `fork` or
don't have it at all. The `cfree()` check on line 316 is similarly obsolete
(removed from POSIX long ago).

### 5.3 The SN heritage

The configure.ac comments mention "SN/TL3/SN3.2", referencing the SN
(Simulateur Neuronal) heritage. The variable names in configure.ac still use
`sn_LIBS` and similar prefixes. The README references a history dating to
the late 1980s. The neural network packages (`sn28`, `gblearn2`, `torch`)
represent some of the earliest practical neural network implementations.

### 5.4 Lush2 branch exists but is a dead end

Per the user's notes, the `lush2` branch in the git history attempted a
more complete 64-bit cleanup but diverged significantly and no longer
compiles. Some ideas may be worth extracting (particularly around the
`intg` migration), but the branch should be treated as reference material
rather than a merge target.

---

## 6. Potential Future Improvements (Out of Scope)

These are not part of the 64-bit cleanup but are worth noting:

- **Replace BFD with dlopen:** The most sustainable path for dynamic loading
  on modern systems. Would eliminate the biggest external dependency.
- **Add `ST_I64` storage type:** Enable 64-bit integer arrays for
  applications that need them.
- **Replace `sprintf` with `snprintf`:** Prevent buffer overflows throughout.
- **Add `uintptr_t` for pointer-integer conversions:** Replace `unsigned long`
  casts with the standard type.
- **Consider `stdint.h` types:** `int32_t`, `int64_t`, `uint32_t` for
  explicit-width fields in serialization and type-punning code.
