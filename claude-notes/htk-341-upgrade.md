# HTK 3.4.1 Upgrade Summary

## What Was Done

Upgraded the Lush HTK package (`packages/htk/`) from HTK 3.1 (2002) to
HTK 3.4.1 (2009), including automated build via lush-pkg, full API updates,
and five new module bindings.

## Stage 1: Build HTK 3.4.1 from Source

Downloaded from GitHub mirror (open-speech/HTK), commit `7f9b8761`.
Patches required:
- Remove `-m32` flag (x86_64 incompatible)
- Add `-fPIC` for Lush's shared library linking
- Patch `Makefile.in` so top-level `make` is a no-op (only build HTKLib)
- Post-build: copy `HTKLiblv.a` as `libhtk.a` + install headers

Installed to `~/.lush/local/lib/libhtk.a` and `~/.lush/local/include/htk/`.

## Stage 2: Updated Lush Bindings

### htk-config.lsh (Rewritten)
- Removed old `mod-load` hack (which had a "THIS IS COMPLETELY WRONG" comment)
- Uses `lush-pkg-ensure` for automated download/build/install
- Updated `htk-cpheader-cmd` with 5 new 3.4.1 headers

### API Changes Fixed

| File | Change |
|------|--------|
| `htk_adapt.lsh` | Complete rewrite: old RegTransInfo API replaced by XForm framework |
| `htk_fb.lsh` | `InitialiseForBack` lost `rt` param; added `SetTraceFB`, `UseAlignHMMSet` |
| `htk_model.lsh` | `SaveHMMSet` gained `macroext` param; added ~20 XForm functions |
| `htk_rec.lsh` | `ProcessObservation` gained `xform` param |
| `htk_net.lsh` | Added `ReadOneLattice` |
| `htk_shell.lsh` | Added `ReadStringWithLen`, `SetScriptFile` |
| `htk_train.lsh` | `AttachAccs`, `ZeroAccs`, `ShowAccs` gained `uflags`; `DumpAccs` gained `uflags`; `LoadAccs` gained `uflags` |

### New Modules Created

| File | HTK Module | Purpose |
|------|-----------|---------|
| `htk_arc.lsh` | HArc | Lattice arc management |
| `htk_lat.lsh` | HLat | Lattice operations (pruning, sorting, scoring) |
| `htk_fblat.lsh` | HFBLat | Forward-backward on lattices |
| `htk_map.lsh` | HMap | MAP estimation |
| `htk_exactmpe.lsh` | HExactMPE | Exact minimum phone error training |

### Supporting File Updates

- `aux_structure.lsh`: Added 8 new struct allocators (XFInfo, AdaptXForm, etc.);
  removed 6 obsolete structs (RegTransInfo, BaseformAcc, etc.)
- `htk_constant.lsh`: Added XFormKind, AdaptKind, BaseClassKind, LatFBType enums
- `htk.lsh`: Added 5 new libload entries
- `htk.hlp`: Updated to 3.4.1 with new install instructions

## Stage 3: lush-pkg Automation

### lush-pkg.lsh Enhancements
- Added `pre-build` hook (shell command after extraction, before configure)
- Added `post-build` hook (shell command after make install)
- Fixed bug: `static-library-path` was not being prepended (only `shared-library-path` was)

### htk-config.lsh lush-pkg Spec
Uses commit-pinned GitHub URL with SHA-256 verification. Pre-build patches
configure and Makefile.in; post-build builds HTKLib only and installs
library + headers.

## Bugs Found and Fixed During Development

1. `find-static-library "htk"` returns nil -- must use `"libhtk"` (no auto lib prefix)
2. `static-library-path` not set by lush-pkg -- added to path prepending
3. `TransformId`, `BaseformAcc`, `OffsetBMat`, `OffsetTriBMat`, `RegAcc`,
   `RegTransInfo` structs removed in 3.4.1 -- removed from aux_structure.lsh
4. `LatExpand` and `ApplyNGram2LabLat` not in library -- removed from htk_lat.lsh
5. `traceHFB` global removed in 3.4.1 -- removed from htk_fb.lsh dummy function
6. `HEndSpoolGraf` missing in 3.4.1 -- htk_graf.lsh not loaded by htk.lsh (no fix needed)

## Verification

```
$ echo '(libload "htk/htk") (printf "OK\n")' | bin/lush
OK
```
- All 28 modules compile and load without errors
- Cached load time: ~85ms
- First-time compile: ~2-3 minutes (builds all modules from C)
