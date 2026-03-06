# Module Loader Modernization Plan

## Implementation Status (2026-03-06)

**Phase 1 Step 1 is DONE.** The compilation pipeline now produces `.so` files:

```
Lush source → dhc-generate-c → .c → gcc -fPIC -c → .o → gcc -shared → .so → mod-load
```

Changes made:
- `dh-compile.lsh`: `dhc-make-all` now links `.o` → `.so` via `gcc -shared` before
  calling `mod-load` on the `.so` (not the `.o`)
- `dh-compile.lsh`: `-fPIC` added to `dhc-make-command`
- `lsh/libc/make.lsh`: `load` method does the same `.o` → `.so` linking step
- All packages have `.so` files in their `C/x86_64-unknown-linux-gnu/` directories

**BFD is still fully in the loop.** Even for `.so` files, `module.c` calls
`dld_dlopen()` (not raw `dlopen()`). `dld_dlopen` (dldbfd.c:2371) does:
1. `bfd_openr()` to parse the `.so`'s dynamic symbol table
2. Real `dlopen()` to actually load the code
3. Imports every symbol into BFD's `global_symbol_table` hash via `insert_symbol()`
4. Calls `resolve_newly_defined_symbols()` and `perform_all_relocations()` on any
   previously BFD-linked `.o` modules

All symbol lookups (`dynlink_symbol` in module.c:826) go through `dld_get_func()` /
`dld_get_symbol()` which search BFD's global hash — not `dlsym`.

**Phases 2-3 (make BFD optional, remove BFD) are NOT planned.** BFD works. It provides
features that would be non-trivial to reimplement: dependency tracking, undefined symbol
detection, executability checks, `module-depends`, `module-undefined`. The only benefit
of removing BFD would be eliminating the `-lbfd` build dependency, which is not worth
the risk of a major C-level rewrite of `module.c` and the loss of these features. The
MAP_32BIT arena and hand-written relocation stubs are no longer exercised for new code
(since `.so` files handle their own relocations), so the practical 64-bit concerns are
already addressed.

**What the .o → .so change actually bought us:**
- The OS linker handles relocations, so MAP_32BIT arena exhaustion and architecture-
  specific relocation stubs (x86-64 JMP trampolines, ARM64 branch stubs) are no longer
  exercised for newly compiled code
- `-fPIC` means position-independent code, removing the 2GB address space constraint
- Better error messages from `dlopen` vs BFD relocation failures
- Compatibility with modern toolchains that may not support non-PIC .o linking

## Architecture Overview

The Lush module loader is a dual-path dynamic linking system with two backends:

1. **DLDBFD** (`src/dldbfd.c`, ~3000 lines) — A custom dynamic linker built on the GNU
   BFD library. Loads relocatable `.o` files directly, performs symbol resolution, and
   applies relocations in-process. Also wraps `dlopen` for `.so` files, importing their
   symbols into BFD's global hash table for unified symbol resolution.

2. **DLOPEN** (via `dld_dlopen` in `src/dldbfd.c`) — All `.so` files are loaded through
   this path, which calls real `dlopen` but also uses BFD to parse the `.so`'s symbol
   table and integrate it into the global symbol namespace. Raw `dlopen` (without BFD)
   is only used when `HAVE_LIBBFD` is not defined, which is not the default.

The current compilation pipeline is:

```
Lush source → dhc-generate-c → .c file → gcc -fPIC -c → .o file → gcc -shared → .so file → mod-load → dld_dlopen (BFD + dlopen)
```

The DH (Dynamic Handler) system (`src/dh.c`, `src/lisp_c.c`) provides metadata-driven
function dispatch, bridging compiled C functions with the Lush interpreter through an AVL
tree synchronization engine.

---

## Original Motivation (Now Largely Addressed)

### Problem 1: Architecture-Specific Relocation Code — MITIGATED

DLDBFD contains ~500 lines of hand-written relocation overflow handling for `.o` files:
- **x86-64** (dldbfd.c:1639-1666): Manual JMP stub generation for calls beyond ±2GB
- **ARM64** (dldbfd.c:1667-1694): Manual branch stub generation
- **ARM64 BFD patches** (dldbfd.c:887-1036): ~150 lines patching BFD's buggy ARM64
  relocation implementations for ADR, LDST, MOVW, MOVZ/MOVN

**Status:** This code is no longer exercised for new compilations because all new code
is compiled as `-fPIC` `.so` files where the OS linker handles relocations. The code
remains in `dldbfd.c` for any legacy `.o` loading but is dead code in normal operation.

### Problem 2: Memory Model Constraints — MITIGATED

On x86-64, DLDBFD's `.o` loading path allocates code in the low 2GB of address space
(MAP_32BIT) because non-PIC code uses RIP-relative addressing. This required a custom
arena allocator (dldbfd.c:1061-1200).

**Status:** With `-fPIC -shared` compilation, this arena is no longer used for new code.
The MAP_32BIT allocator remains compiled in but is only exercised if someone manually
loads a `.o` file.

### Problem 3: BFD API Instability — ACCEPTED

The BFD library is an internal GNU binutils component, not a stable public API. The
codebase has 3 configure-time API checks (`bfd_hash_table_init` arity,
`bfd_set_section_size` arity, etc.). However, BFD is available on all Linux systems
with `binutils-dev` installed, and the API checks handle known variations. This is an
acceptable maintenance burden given that BFD provides symbol resolution, dependency
tracking, and executability checking that would be non-trivial to reimplement.

### Non-Problem: Removing BFD Entirely

BFD is deeply integrated into the module system. Even for `.so` files, `dld_dlopen()`
uses BFD to parse symbol tables and integrate them into a global hash. All symbol
lookups (`dld_get_func`, `dld_get_symbol`) go through this hash. Module dependency
tracking (`dld_simulate_unlink_by_file`, `dld_function_executable_p`), undefined symbol
detection (`dld_undefined_sym_count`, `dld_list_undefined_sym`), and module unloading
(`dld_unlink_by_file`) all depend on BFD's internal data structures.

Replacing this with raw `dlsym` would require reimplementing dependency tracking,
symbol resolution, and executability checking — all for the sole benefit of removing
one build dependency (`-lbfd`). This is not worth doing.

---

## What Was Done (Phase 1)

The compilation pipeline was changed from:
```
gcc -c source.c -o output.o    →    mod-load output.o (DLDBFD relocates .o in-process)
```
to:
```
gcc -fPIC -c source.c -o output.o  →  gcc -shared -o output.so output.o  →  mod-load output.so (dld_dlopen)
```

**Changes made:**

1. **`lsh/compiler/dh-compile.lsh`** — `dhc-make-all` (lines 1215-1242):
   - After compiling `.o`, links it into `.so` via `gcc -shared`
   - Calls `mod-load` on the `.so` file (not the `.o`)
   - `-fPIC` added to `dhc-make-command`

2. **`lsh/libc/make.lsh`** — `load` method (lines 275-311):
   - Same pattern: if target is `.o`, links to `.so` before `mod-load`
   - Checks modification times to avoid unnecessary re-linking

**What did NOT change:**
- `dhc-make-o-filename` still returns `.o` extension (the `.so` is a secondary step)
- `dhc-make-command` still compiles to `.o` (then a separate link step creates `.so`)
- `src/module.c` dispatch logic unchanged — `.so` extension triggers `dld_dlopen` path
- BFD is still fully involved via `dld_dlopen` (see status section above)
- `-fno-pie` still in Makefile OPTS (harmless, could be removed but not urgent)
- MODULE_STICKY still set for all `.so` files (compiled modules cannot be unloaded)

## What Is NOT Planned

**Phase 2 (Make BFD optional) — CANCELLED.** BFD works and provides valuable
functionality (dependency tracking, executability checks, undefined symbol detection)
that would need to be reimplemented. Not worth the effort.

**Phase 3 (Module lifecycle) — CANCELLED.** The current system works. Module unloading
via `dlclose` is risky and rarely needed.

---

## Detailed Code Paths (Reference)

### How module loading works (current, post-Phase 1)

1. Lush code calls `(mod-load "foo.so")`
2. `module_load()` in `module.c` checks the file extension
3. `.so` → sets `dlopenp = 1`; `.o` → sets `dlopenp = 0`
4. For `.so`: calls `dld_dlopen()` in `dldbfd.c`:
   - `bfd_openr()` parses the `.so`'s dynamic symbol table
   - `dlopen()` actually loads the code
   - Every symbol is inserted into BFD's `global_symbol_table`
   - `resolve_newly_defined_symbols()` resolves any pending refs
5. For `.o`: calls `dld_link()` in `dldbfd.c`:
   - Full BFD relocation: loads sections, resolves symbols, applies relocations
   - Uses MAP_32BIT arena on x86-64
   - This path is no longer used by dhc-make but still exists
6. Init function found via `dld_get_func()` (BFD hash lookup)
7. Init function called to register DX/DH functions with Lush

### Key BFD API surface used by module.c

| dld_* function | Purpose | Used for .so? |
|----------------|---------|---------------|
| `dld_init` | Read executable's own symbols into BFD hash | Yes (startup) |
| `dld_dlopen` | BFD symbol import + real dlopen | Yes (every .so load) |
| `dld_get_func` / `dld_get_symbol` | Symbol lookup via BFD hash | Yes (all lookups) |
| `dld_function_executable_p` | Check if all deps resolved | .o only |
| `dld_simulate_unlink_by_file` | Dependency analysis | .o only |
| `dld_unlink_by_file` | Unload module | .o only |
| `dld_create_reference` | Force-reference a symbol | Exposed to Lush |
| `dld_undefined_sym_count` | Count unresolved symbols | Global state |
| `dld_list_undefined_sym` | List unresolved symbol names | Exposed to Lush |
| `dld_find_executable` | Find program path at startup | Yes (startup) |

---

## Remaining Considerations

### `-fno-pie` in Makefile OPTS

Still present in `src/Makefile` line 37. This was originally required because DLDBFD
loads non-PIC `.o` files. With the `.so` pipeline, it's no longer strictly necessary
for user-compiled code, but it doesn't hurt and may be needed if anyone loads a `.o`
directly. Low priority to remove.

### MODULE_STICKY for compiled code

All `.so` files are marked MODULE_STICKY (cannot be unloaded). This means compiled
Lush functions persist for the lifetime of the session. This is the safe default and
there is no plan to change it.

---

## Appendix: Key Functions Reference

| Function | File | Role |
|----------|------|------|
| `module_load()` | module.c:1286 | Main entry — dispatches to dld_dlopen or dld_link |
| `dld_link()` | dldbfd.c:2187 | DLDBFD: load .o file (no longer used by dhc-make) |
| `dld_dlopen()` | dldbfd.c:2371 | BFD symbol import + real dlopen for .so files |
| `dld_init()` | dldbfd.c:2340 | Initialize BFD, read executable's own symbol table |
| `dld_get_func()` | dldbfd.c:2619 | Symbol lookup in BFD global hash (functions) |
| `dld_get_symbol()` | dldbfd.c:2591 | Symbol lookup in BFD global hash (data) |
| `dynlink_init()` | module.c:797 | Initialize dynamic linking (calls dld_init) |
| `dynlink_symbol()` | module.c:827 | Look up symbol — delegates to dld_get_func/symbol |
| `update_exec_flag()` | module.c:968 | Refresh function pointers after load |
| `cleanup_module()` | module.c | Clean up when unloading |
| `check_exec()` | module.c | Recompute executability for all modules |
| `dhc-make-all` | dh-compile.lsh | Orchestrates: .c → .o → .so → mod-load |
| `dhc-make-command` | dh-compile.lsh | Compilation command template (now has -fPIC) |
