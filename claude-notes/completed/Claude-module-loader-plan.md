# Module Loader Modernization Plan

## Current Architecture Overview

The Lush module loader is a dual-path dynamic linking system with two backends:

1. **DLDBFD** (`src/dldbfd.c`, ~3000 lines) — A custom dynamic linker built on the GNU
   BFD library. Loads relocatable `.o` files directly, performs symbol resolution, and
   applies relocations in-process. This is the primary path used by `dhc-make` when
   compiling Lush functions to C.

2. **DLOPEN** (`src/module.c` DLOPEN path) — Standard `dlopen/dlsym` for pre-built `.so`
   shared libraries. Used for loading external libraries (BLAS, LAPACK, etc.).

The compilation pipeline is:

```
Lush source → dhc-generate-c → .c file → gcc -c → .o file → mod-load → DLDBFD
```

The DH (Dynamic Handler) system (`src/dh.c`, `src/lisp_c.c`) provides metadata-driven
function dispatch, bridging compiled C functions with the Lush interpreter through an AVL
tree synchronization engine.

---

## Why Modernize?

### Problem 1: BFD API Instability

The BFD library is an internal GNU binutils component, not a stable public API. Every
distro update can break compatibility. The codebase already has 3 configure-time API checks
(`bfd_hash_table_init` arity, `bfd_set_section_size` arity, etc.) and this will only get
worse. BFD is increasingly hard to package on modern systems.

### Problem 2: Architecture-Specific Relocation Code

DLDBFD contains ~500 lines of hand-written relocation overflow handling:
- **x86-64** (dldbfd.c:1639-1666): Manual JMP stub generation for calls beyond ±2GB
- **ARM64** (dldbfd.c:1667-1694): Manual branch stub generation
- **ARM64 BFD patches** (dldbfd.c:887-1036): ~150 lines patching BFD's buggy ARM64
  relocation implementations for ADR, LDST, MOVW, MOVZ/MOVN

Every new architecture would require another block of relocation code.

### Problem 3: Memory Model Constraints

On x86-64, DLDBFD must allocate code in the low 2GB of address space (MAP_32BIT) because
GCC generates non-PIC code with RIP-relative addressing. This requires a custom memory
arena allocator (dldbfd.c:1061-1200) and is fragile — large programs can exhaust the 2GB
address space.

### Problem 4: Compilation Produces .o Files

The standard path compiles to relocatable `.o` files, which require the full DLDBFD
machinery to load. Modern practice is to compile to position-independent shared objects
(`.so`) which the OS linker handles natively via `dlopen`.

---

## Proposed Modernization Strategy

### Phase 1: Compile to .so Instead of .o

**Goal:** Change the default compilation pipeline from:
```
gcc -c source.c -o output.o    →    mod-load output.o (DLDBFD)
```
to:
```
gcc -shared -fPIC source.c -o output.so    →    mod-load output.so (DLOPEN)
```

**What changes:**

1. **`lsh/compiler/dh-compile.lsh`** — The `dhc-make-command` variable and related
   functions:
   - `dhc-make-command`: Change from `$CC $DEFS $LUSHFLAGS $INCS -c $SRC -o $OBJ` to a
     two-step or single-step shared object compilation
   - `dhc-make-o-filename`: Change default extension from `.o` to `.so`
   - `dhc-make-lushflags`: Add `-fPIC` to default flags
   - A new `dhc-make-so-command` variable:
     `$CC -shared -fPIC $DEFS $LUSHFLAGS $INCS $SRC -o $OBJ`
   - On Linux: add `-Wl,-export-dynamic` or `-rdynamic` to export symbols
   - On macOS: use `-bundle -flat_namespace` (already in configure.ac as MAKESO)

2. **`src/module.c`** — The `module_load()` function:
   - Currently uses filename extension to choose DLOPEN vs DLDBFD path
   - With `.so` as default, DLOPEN becomes the primary path
   - Keep DLDBFD path for backward compatibility with existing `.o` files
   - The DLOPEN path already handles init function discovery via `dlsym`

3. **`configure.ac`** — Build system:
   - Remove the `require_bfd=yes` default; make BFD optional
   - Add `--with-bfd` / `--without-bfd` flag (already exists but inverted)
   - Set default compilation to shared object mode
   - Detect PIC compilation support
   - The `MAKESO` variable already exists for platform-specific shared object linking

**What stays the same:**
- The DH metadata system (dh.h, dh.c) — unchanged
- The Lisp-C bridge (lisp_c.c) — unchanged
- The init function calling convention — unchanged
- The dhc-generate-c code generation — unchanged (produces valid C either way)

**Key considerations:**

- **Symbol visibility:** With `.o` files + DLDBFD, all symbols are visible. With `.so` +
  dlopen(RTLD_GLOBAL), all exported symbols are visible. The generated init functions are
  already exported (they're non-static global functions), so `dlsym` can find them.

- **Inter-module dependencies:** Currently tracked via the BFD global symbol table. With
  shared objects, the OS linker handles this via RTLD_GLOBAL. If module B depends on
  symbols from module A, both loaded with RTLD_GLOBAL will resolve automatically. For
  explicit control, `dlsym(RTLD_DEFAULT, name)` can look up any globally-loaded symbol.

- **Module unloading:** Currently, DLDBFD supports fine-grained unloading with dependency
  tracking. With dlopen, `dlclose` is available but reference-counted. The current
  MODULE_STICKY flag (set for .so files) prevents unloading. The module cleanup logic in
  `cleanup_module()` handles Lush-side cleanup (destroying class instances, unlinking
  symbols) regardless of the loading mechanism. We would need to decide whether to:
  - Make compiled modules unloadable (remove MODULE_STICKY for non-library .so files)
  - Keep them sticky (simpler, lower risk)
  - Initial implementation: keep sticky, add unload support later

- **`-fno-pie` flag:** Currently required because DLDBFD loads non-PIC .o files. With
  `-fPIC -shared`, this flag is no longer needed and should be removed.

### Phase 2: Simplify DLDBFD (or Make It Optional)

Once the default path is DLOPEN-based:

1. **Make BFD a build-time option** (not required):
   - `./configure` works without BFD installed
   - `./configure --with-bfd` enables the DLDBFD path for legacy .o loading
   - This dramatically simplifies the build dependency chain

2. **Remove BFD workarounds over time:**
   - The ARM64 BFD patches (150 lines) become unnecessary for new compilations
   - The MAP_32BIT arena allocator becomes unnecessary
   - The x86-64/ARM64 trampoline stubs become unnecessary

3. **Deprecation path:**
   - Phase 2a: Default to .so, keep .o loading functional
   - Phase 2b: Add deprecation warning when loading .o files
   - Phase 2c: Remove DLDBFD entirely (potentially, if no users need it)

### Phase 3: Improve Module Lifecycle Management

With the DLOPEN foundation in place, improve the module system:

1. **Symbol tracking without BFD:**
   - Use `dlsym(RTLD_DEFAULT, name)` for global symbol lookups
   - Use `dlsym(handle, name)` for per-module symbol lookups
   - The `dynlink_symbol()` function can dispatch to either BFD or dlsym

2. **Module dependency graph:**
   - Currently tracked in BFD's module_entry linked list
   - Build a Lush-side dependency graph using the existing `module->defs` lists
   - The compiler already tracks dependencies via `libload-dependencies`

3. **Graceful error handling:**
   - `dlopen` returns NULL with `dlerror()` messages on failure
   - Much better error messages than BFD's cryptic relocation errors
   - Add symbol resolution diagnostics: which module provides which symbol

---

## Detailed File-by-File Change Map

### Files That Change

| File | Change | Risk |
|------|--------|------|
| `lsh/compiler/dh-compile.lsh` | Compilation command, file extensions | **Medium** — Core compilation path |
| `src/module.c` | Module load dispatch, DLOPEN path enhancement | **Medium** — Must preserve backward compat |
| `configure.ac` | Make BFD optional, add PIC flags | **Low** — Build system only |
| `Makefile.in` | Possibly adjust MAKESO usage | **Low** |

### Files That Stay the Same

| File | Why |
|------|-----|
| `src/lisp_c.c` | The Lisp↔C bridge doesn't care how code was loaded |
| `src/dh.c` | DH metadata is identical for .o and .so |
| `include/dh.h` | DH structures unchanged |
| `src/dldbfd.c` | Kept for backward compat, just no longer the default |
| `lsh/compiler/dh-macro.lsh` | Type system is independent of loading mechanism |
| `lsh/compiler/dh-util.lsh` | Compiler utilities unchanged |
| `src/storage.c`, `src/index.c` | Data structures unchanged |

### Key Code Paths Affected

**1. `dh-compile.lsh` — Compilation command (lines 939-962)**

Current:
```scheme
(defvar dhc-make-command "$CC $DEFS $LUSHFLAGS $INCS -c $SRC -o $OBJ")
```

New:
```scheme
(defvar dhc-make-command "$CC -shared -fPIC $DEFS $LUSHFLAGS $INCS $SRC -o $OBJ")
```

Also: `dhc-make-o-filename` (line 900) needs to produce `.so` instead of `.o`.

**2. `dh-compile.lsh` — File extension (line 913)**

Current:
```scheme
(setq base (concat base "." (or (getconf "OBJEXT") "o")))
```

New:
```scheme
(setq base (concat base "." (or (getconf "SOEXT") "so")))
```

**3. `module.c` — Module load dispatch (line 857)**

Currently checks for `.so`/`.dylib` suffix to choose DLOPEN. After the change, `.so`
becomes the default. The DLDBFD path remains available for `.o` files:

```c
/* Determine loading method */
int dlopenp = 0;
if (has_suffix(fname, ".so") || has_suffix(fname, ".dylib"))
    dlopenp = 1;
#if DLDBFD
else if (has_suffix(fname, ".o") || has_suffix(fname, ".a"))
    dlopenp = 0;  /* Use BFD for .o files */
#endif
else
    dlopenp = 1;  /* Default to dlopen */
```

**4. `module.c` — DLOPEN init function search (lines 863-878)**

The current DLOPEN path searches for `init_user_dll` then `init_<basename>`. This is
the same convention used by the compiler's generated code. No change needed.

**5. `module.c` — MODULE_STICKY for compiled code**

Currently, `.so` files are automatically marked MODULE_STICKY (cannot be unloaded). For
compiled Lush code loaded as `.so`, we may want to allow unloading. This would require:
- Removing the automatic MODULE_STICKY for `.so` files generated by dhc-make
- Testing that `dlclose()` works correctly with the module cleanup logic
- Initial recommendation: **keep MODULE_STICKY** for safety, revisit later

**6. `configure.ac` — BFD optionality**

Current logic:
```
require_bfd=yes
# ... checks for bfd ...
if test "$require_bfd" = "yes" && test "$ac_cv_lib_bfd_bfd_init" != "yes"; then
    AC_MSG_ERROR([BFD library is required])
fi
```

New logic:
```
require_bfd=no  # Default: don't require BFD
AC_ARG_WITH(bfd, [...], [...])
# ... BFD checks remain available for --with-bfd ...
```

---

## Risk Analysis

### Low Risk
- Changing the compiler command to produce `.so` files
- Making BFD optional in configure.ac
- Adding `-fPIC` to default compiler flags

### Medium Risk
- **Inter-module symbol resolution:** BFD's global symbol table is replaced by the OS
  linker's global namespace. Need to verify RTLD_GLOBAL propagation works for all
  inter-module dependency patterns used by Lush libraries.
- **Init function naming:** The current convention (`init_<basename>`) must be preserved
  exactly. With shared objects, the symbol must be exported (non-static), which it
  already is.
- **Library loading order:** Some external libraries (BLAS, LAPACK) are loaded as `.so` via
  the DLOPEN path. After the change, compiled Lush code and external libraries use the same
  mechanism, which should simplify things but needs verification.

### Higher Risk
- **Module unloading:** If we make compiled `.so` modules unloadable (remove
  MODULE_STICKY), the `cleanup_module()` and `dlclose()` interaction needs careful testing.
  Recommendation: defer this to a later phase.
- **Platform-specific shared object flags:** macOS uses `-bundle -flat_namespace` vs
  Linux's `-shared`. The configure.ac `MAKESO` variable handles this but needs testing on
  both platforms.
- **Existing `.o` file ecosystems:** Users with compiled `.o` files in their C/ directories
  will need to recompile. The DLDBFD backward-compatibility path mitigates this.

---

## Implementation Order

### Step 1: Add .so compilation support alongside .o (non-breaking)

- Add `dhc-make-so-command` variable
- Add `dhc-make-so` function
- Add `dhc-make-so-filename` function
- Test with a few functions compiled as `.so`
- Verify init function discovery via dlsym works

### Step 2: Switch default compilation to .so

- Change `dhc-make-command` to produce `.so`
- Change `dhc-make-o-filename` to produce `.so` extension
- Remove `-fno-pie` from default flags, add `-fPIC`
- Update configure.ac to make BFD optional
- Run full test suite

### Step 3: Verify all Lush standard libraries compile as .so

- Rebuild all libraries in `lsh/` with new default
- Rebuild all packages that use `dhc-make`
- Fix any symbol resolution issues

### Step 4: Add comprehensive module tests (see Test Harness section)

### Step 5: Clean up DLDBFD (optional, future)

- Move DLDBFD behind `--with-bfd` configure flag
- Add deprecation warnings for `.o` loading
- Eventually remove dldbfd.c (~3000 lines)

---

## Test Harness Considerations

The module loader modernization is the highest-risk change in the Lush modernization
effort. Before and during this work, a comprehensive Lush-language test harness is
essential. The harness should test:

### Module Loading Tests
- Load a compiled `.so` module and call a function
- Load two modules where B depends on A's symbols
- Module with compiled class, create instances, call methods
- Module with idx operations on all storage types (including ST_I64)
- Module that accesses global variables across modules
- Error: load nonexistent module
- Error: load module with unresolved symbols

### Compiled Code Tests (regression for all data types)
- Scalar operations: int, long, flt, real, byte, ubyte, short, gptr
- Matrix operations: creation, element access, idx-bloop, copy-matrix
- Save/load round-trips for all matrix types
- Type promotion in mixed expressions
- Compiled function calling compiled function
- Compiled function calling interpreted function (callbacks)
- Compiled class methods

### Platform Tests
- Verify compilation flags on Linux x86-64
- Verify compilation flags on Linux ARM64
- Verify init function visibility in .so files
- Verify RTLD_GLOBAL symbol propagation

The test harness should be written in Lush itself, using a simple framework that
reports PASS/FAIL for each test case. This is described in more detail in the test
harness design that should be implemented before the module loader work begins.

---

## Dependencies

This plan depends on:
1. ~~Tier 3.2 (ST_I64)~~ — Complete
2. **Lush test harness** — Should be built first to validate module loader changes
3. **Tier 3.1 was renumbered as this plan** — This IS the module loader modernization

The recommended execution order is:
1. Build the Lush test harness (comprehensive, covering all types and compilation)
2. Implement Step 1 (add .so support alongside .o)
3. Run full test harness — verify no regressions
4. Implement Step 2 (switch default to .so)
5. Run full test harness — verify no regressions
6. Implement Step 3 (verify standard libraries)
7. Run full test harness — verify no regressions
8. Implement Step 5 (clean up DLDBFD) only after extensive validation

---

## Appendix: Key Functions Reference

| Function | File:Line | Role |
|----------|-----------|------|
| `module_load()` | module.c:795 | Main entry point — dispatches to DLOPEN or DLDBFD |
| `dld_link()` | dldbfd.c:2237 | DLDBFD: load .o file |
| `dld_dlopen()` | dldbfd.c:2445 | DLDBFD: open .so via dlopen + BFD symbol import |
| `dld_init()` | dldbfd.c:2414 | Initialize BFD subsystem |
| `dynlink_init()` | module.c:352 | Initialize dynamic linking |
| `dynlink_symbol()` | module.c:379 | Look up symbol (delegates to BFD or dlsym) |
| `update_exec_flag()` | module.c:508 | Refresh function pointers after load |
| `update_init_flag()` | module.c:570 | Call module's init function |
| `cleanup_module()` | module.c:415 | Clean up when unloading |
| `check_exec()` | module.c:643 | Recompute executability for all modules |
| `dh_listeval()` | lisp_c.c:2659 | Call compiled function from Lush |
| `at_to_dharg()` | lisp_c.c:1823 | Lush → C argument conversion |
| `dharg_to_at()` | lisp_c.c:2102 | C → Lush return value conversion |
| `dhc-make-all` | dh-compile.lsh:1165 | Orchestrates compilation + loading |
| `dhc-make-command` | dh-compile.lsh:961 | Compilation command template |
| `dhc-make-o` | dh-compile.lsh:998 | Execute compilation command |
| `dhc-generate-c` | dh-compile.lsh:93 | Generate C source from Lush functions |
