# Lush Package Manager Plan (v4)

## Problem

Several Lush packages depend on external C libraries (xgboost, curl, libuv) that
aren't available via system package managers with the right versions/features, or
need specific build options. Today each package has a bespoke install script
(bash) and config file (lsh) with duplicated logic for downloading, checksum
verification, building, and discovery.

There is also a **version safety problem**. System-installed libraries may be
present but wrong:

| Library | System version | Lush package needs | Problem |
|---------|---------------|--------------------|---------|
| libcurl | 8.5.0         | 8.12.1             | System curl lacks WebSocket support |
| libuv   | 1.0.0         | 1.49.2             | System libuv is ancient, missing most API |
| xgboost | not installed | 3.2.0              | Must build from source |

If a *-config.lsh file accidentally links against the system library instead of
the locally-built one, functions will be missing at runtime (segfaults, null
pointer dereferences). Every package that needs a specific version must build
its own copy and link against it explicitly.

## Existing Patterns (as-is)

### 1. "Download & Build" (xgboost, curl)

```
packages/xgboost/
  install-xgboost-locally.sh   # bash: download tarball, cmake, make install
  dist/                        # output: include/ + lib/  (gitignored)
  xgboost-config.lsh           # lushmake: -I dist/include -L dist/lib -Wl,-rpath
  xgboost-c.c / xgboost-c.h   # thin C wrapper compiled by lushmake
  xgboost.lsh                  # Lush API: libload config, dhc-make
```

Works correctly when `dist/` is populated. The rpath ensures the right .so is
loaded at runtime. But the install script must be run manually, and the logic
for downloading/verifying/building is duplicated between xgboost and curl.

### 2. "Vendor Source" (libuv, json, sqlite)

```
packages/libuv/
  vendor-libuv.sh              # bash: download tarball, extract ~49 .c/.h files
  include/ src/                # vendored source (gitignored, NOT checked in)
  libuv-config.lsh             # lushmake: compile all .c into one .so
  libuv-c.c / libuv-c.h       # wrapper layer compiled alongside
  libuv.lsh                    # Lush API
```

**libuv** vendors ~49 source files that are NOT checked into git. The vendor
script must be run before the package works. Currently broken:
`(libload "libuv/libuv")` fails with "Do not know how to make: src/strtok.c"
because the vendored sources are missing.

**json** and **sqlite** have their source fully checked into git (yyjson.c/h
and sqlite3.c/h respectively). These work as-is with no external step.

### 3. "System Library" (GSL, LAPACK, FFTW)

Uses `find-shared-library` to locate system-installed headers and .so files.
Works when system packages are installed. No version pinning.

## Package Classification

After analysis, packages fall into three groups:

**Need lush-pkg** (external C library, version-sensitive):
- **xgboost** -- cmake build, no system package available
- **curl** -- autotools build, system version too old (no WebSocket)
- **libuv** -- cmake build, system version far too old (1.0.0 vs 1.49.2)

**Self-contained** (source checked into git, no lush-pkg needed):
- **json** -- yyjson source (2 files) checked in
- **sqlite** -- sqlite3 amalgamation (2 files) checked in

**System library** (no version pinning, no lush-pkg needed):
- **gsl**, **lapack**, **fftw** -- use `find-shared-library` as today

## Design: `lush-pkg`

A small Lush utility library (`packages/lush-pkg/lush-pkg.lsh`) that provides
common functions for downloading, verifying, building, and installing external
C library dependencies. Inspired by R's package management model:

- **Each package is self-describing.** Like R's DESCRIPTION file, each Lush
  package that needs an external C library declares its own dependency spec
  inline in its *-config.lsh file. There is no global registry.

- **Per-user library directory.** Like R's `~/R/x86_64-.../4.x/`, Lush uses
  `~/.lush/local/` as the per-user prefix for locally-built C libraries.
  Each user has their own independent copy. Disk is cheap.

- **Build from source at install time.** Like R on Linux, the first time a
  package is loaded, the user sees a build with progress messages. Users are
  accustomed to this from R, pip, cargo, etc.

### Design Principles

- **Self-contained utility.** `lush-pkg.lsh` has zero Lush package
  dependencies. It uses only base Lush functions and standard Linux tools
  (wget, sha256sum, tar, cmake, make).

- **Decentralized specs.** Each package declares its own dependency inline.
  `lush-pkg` provides the machinery, not the catalog.

- **Per-user prefix.** Libraries are built into `~/.lush/local/`. Each user
  maintains their own independent copy.

- **Version-safe.** By building into a dedicated prefix and linking with
  `-Wl,-rpath`, packages always use the version they declare, never
  accidentally falling through to a wrong system library.

- **Idempotent.** If the check artifact already exists, skip everything.

### Directory Layout

```
~/.lush/
  lushrc.lsh                   # user's lush config (already exists)
  lsh/                         # varlushdir compiled output (dhc-make creates)
  packages/                    # varlushdir compiled output (dhc-make creates)
  local/                       # per-user locally-built C libraries (NEW)
    include/                   # installed headers (e.g. uv.h, curl/curl.h)
    lib/                       # installed .so / .a (e.g. libuv.so, libcurl.so)
    src/                       # extracted source trees (build workspace)
    tarballs/                  # downloaded archives (cached across rebuilds)
```

Why `~/.lush/local/`:

- `~/.lush/` is the established per-user Lush directory.
- Follows R's model: per-user library, separate from the system install.
- Each user has their own independent copy---no sharing, no conflicts.
- Overridable: `(defvar *lush-pkg-prefix* "/other/path")` in `lushrc.lsh`.

### Compiler Integration

On load, `lush-pkg.lsh` prepends to the global paths:

```lush
(defvar *lush-pkg-prefix*
  (concat-fname (concat-fname (getenv "HOME") ".lush") "local"))

(let ((incdir (concat-fname *lush-pkg-prefix* "include"))
      (libdir (concat-fname *lush-pkg-prefix* "lib")))
  (when (not (member incdir c-include-path))
    (setq c-include-path (cons incdir c-include-path)))
  (when (not (member libdir shared-library-path))
    (setq shared-library-path (cons libdir shared-library-path))))
```

This means `dhc-generate-include-flags` automatically emits
`-I ~/.lush/local/include`, and `find-shared-library` searches
`~/.lush/local/lib/`. The per-package `-I dist/include` pattern becomes
unnecessary.

The link-flags helper provides `-L` and `-Wl,-rpath` for lushmake rules:

```lush
(de lush-pkg-link-flags ()
  (let ((libdir (concat-fname *lush-pkg-prefix* "lib")))
    (concat "-L" libdir " -Wl,-rpath," libdir)))
```

The `-Wl,-rpath` is critical for version safety: it tells the dynamic linker
to look in `~/.lush/local/lib/` FIRST, before system paths, ensuring the
right library version is loaded at runtime.

### Package Specifications

Each package declares its own dependency inline in its *-config.lsh:

```lush
;;; xgboost-config.lsh:
(libload "lush-pkg/lush-pkg")
(lush-pkg-ensure "xgboost"
  `((version   . "3.2.0")
    (url       . "https://github.com/dmlc/xgboost/releases/download/v3.2.0/xgboost-src-3.2.0.tar.gz")
    (sha256    . "16a31dfbc0c54544c9c36ab5f696fa7b646c125f161c52c814d757a58241a404")
    (build     . cmake)
    (cmake-args . "-DBUILD_STATIC_LIB=OFF")
    (check     . "lib/libxgboost.so")))
```

```lush
;;; curl-config.lsh:
(libload "lush-pkg/lush-pkg")
(lush-pkg-ensure "curl"
  `((version   . "8.12.1")
    (url       . "https://curl.se/download/curl-8.12.1.tar.gz")
    (sha256    . "7b40ea64947e0b440716a4d7f0b7aa56230a5341c8377d7b609649d4aea8dbcf")
    (build     . autotools)
    (configure-args . "--enable-websockets --with-openssl --disable-ldap --disable-docs")
    (check     . "lib/libcurl.so")))
```

```lush
;;; libuv-config.lsh:
(libload "lush-pkg/lush-pkg")
(lush-pkg-ensure "libuv"
  `((version   . "1.49.2")
    (url       . "https://dist.libuv.org/dist/v1.49.2/libuv-v1.49.2.tar.gz")
    (sha256    . "...")  ;; to be filled in
    (build     . cmake)
    (cmake-args . "-DBUILD_TESTING=OFF -DLIBUV_BUILD_SHARED=ON")
    (check     . "lib/libuv.so")))
```

### Core API

```lush
#? (lush-pkg-ensure <name> <spec>)
;; Ensure an external C library is available.
;; If the check artifact exists, returns immediately.
;; Otherwise: download, verify, build, install. Shows progress.
;; Idempotent: safe to call every time the package is loaded.

#? (lush-pkg-installed-p <check>)
;; Returns t if <check> (relative path) exists under *lush-pkg-prefix*.

#? (lush-pkg-link-flags)
;; Return "-L <prefix>/lib -Wl,-rpath,<prefix>/lib" for lushmake rules.

#? (lush-pkg-prefix)
;; Return the current prefix path.
```

### Build Strategies

**cmake** (xgboost, libuv):
```bash
cd $SRCDIR && mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$PREFIX $CMAKE_ARGS
make -j$(nproc)
make install
```

**autotools** (curl):
```bash
cd $SRCDIR
./configure --prefix=$PREFIX $CONFIGURE_ARGS
make -j$(nproc)
make install
```

Executed via `(sys ...)` from Lush.

### External Tool Dependencies

- **wget**: Download tarballs. Preferred over curl to avoid bootstrapping
  confusion when building libcurl itself.
- **sha256sum**: Verify checksums. Part of coreutils.
- **tar**, **cmake**, **make**, **gcc**: Standard build tools.

No Lush packages required. Pure interpreted Lush using `(sys ...)`.

### User Experience

First load of a package with external dependency:

```
? (libload "libuv/libuv")
[lush-pkg] libuv 1.49.2 not found, installing...
[lush-pkg] Downloading libuv-v1.49.2.tar.gz...
[lush-pkg] Verifying checksum...
[lush-pkg] Building with cmake (this may take a few minutes)...
[lush-pkg] Installed libuv 1.49.2 to /home/scott/.lush/local/
```

Subsequent loads: instant (artifact check passes, no work done).

### Impact on libuv-config.lsh

The biggest architectural change. Currently libuv-config.lsh compiles ~49
vendored source files into a single .so using lushmake (131 lines). After
conversion, libuv is built as a standalone shared library by cmake into
`~/.lush/local/`, and libuv-config.lsh just compiles the thin wrapper:

**Before** (131 lines, compiles 49 source files):
```lush
(libload "libc/make")
(let ((dir (dirname file-being-loaded)))
  (let ((lm (new lushmake)) ...)
    (==> lm setflags (concat " -I" dir "/include -I" dir "/src ..."))
    (==> lm rule "fs-poll.o" '("src/fs-poll.c" ...))
    (==> lm rule "idna.o" '("src/idna.c" ...))
    ;; ... 40+ more rules ...
    (==> lm rule "libuv-c.so" '("fs-poll.o" "idna.o" ... "libuv-c.o")
         (concat cc " -shared -o $OBJ $SOURCES -lpthread -ldl"))
    (==> lm make)
    (==> lm load "libuv-c.so")))
```

**After** (~25 lines, compiles only the wrapper):
```lush
(libload "lush-pkg/lush-pkg")
(lush-pkg-ensure "libuv"
  `((version   . "1.49.2")
    (url       . "https://dist.libuv.org/dist/v1.49.2/libuv-v1.49.2.tar.gz")
    (sha256    . "...")
    (build     . cmake)
    (cmake-args . "-DBUILD_TESTING=OFF -DLIBUV_BUILD_SHARED=ON")
    (check     . "lib/libuv.so")))

(libload "libc/make")
(let ((dir (dirname file-being-loaded)))
  (let ((lm (new lushmake))
        (cc (or (getconf "CC") "gcc")))
    ;; Headers found via c-include-path (lush-pkg added ~/.lush/local/include)
    (==> lm setflags " -O2 -fPIC")
    (==> lm rule "libuv-c.o" '("libuv-c.c" "libuv-c.h"))
    (==> lm rule "libuv-c.so" '("libuv-c.o")
         (concat cc " -shared -o $OBJ $SOURCES"
                 " " (lush-pkg-link-flags) " -luv -lpthread"))
    (==> lm make)
    (==> lm load "libuv-c.so")))
```

The vendor-libuv.sh script and the vendored include/src directories become
unnecessary and can be removed.

## Implementation Plan

### Stage 1: Prove the prefix approach works

**Goal**: Verify that prepending `~/.lush/local/{include,lib}` to the global
paths makes them visible to `dhc-generate-include-flags` and lushmake.

1. **User**: Create the directory structure:
   ```bash
   mkdir -p ~/.lush/local/include ~/.lush/local/lib
   ```

2. **Claude**: Write a test script that prepends the paths and calls
   `(dhc-generate-include-flags)`, printing the result.

3. **User**: Run the test and confirm `-I ~/.lush/local/include` appears.

### Stage 2: Write lush-pkg.lsh

**Goal**: Implement the core utility library (~200 lines).

**File**: `packages/lush-pkg/lush-pkg.lsh`

Key functions:
- `lush-pkg-ensure` -- the main entry point
- `lush-pkg-installed-p` -- check artifact existence
- `lush-pkg-link-flags` -- link flags for lushmake
- `lush-pkg-prefix` -- return prefix path
- `_lush-pkg-run` -- run shell command, error on failure
- `_lush-pkg-url-basename` -- extract filename from URL
- `_lush-pkg-find-srcdir` -- find extracted directory
- `_lush-pkg-build-cmake` -- cmake build strategy
- `_lush-pkg-build-autotools` -- autotools build strategy

### Stage 3: Claude tests (non-network)

Claude uses `/tmp/claude-1000/lush-pkg-test/` as a writable prefix to test:

- Path registration into `c-include-path` / `shared-library-path`
- `lush-pkg-installed-p` with a fake check artifact
- `_lush-pkg-url-basename` correctness
- Idempotent re-entry (lush-pkg-ensure when check already exists)
- Error paths

### Stage 4: User tests real build (curl)

1. **User**: `rm -rf ~/.lush/local`
2. **User**: Load the converted curl package:
   ```bash
   ./src/lush <<'LUSHEOF'
   (addpath "packages")
   (libload "curl/curl")
   LUSHEOF
   ```
3. **Expected**: Download + build messages, then curl loads. 1-3 minutes.
4. **User**: Run again---should be instant.
5. **Verification**: `~/.lush/local/lib/libcurl.so` exists, curl functions work.

### Stage 5: User tests real build (libuv)

Same as Stage 4 but for libuv. This tests the cmake path and the major
libuv-config.lsh rewrite.

### Stage 6: Convert all three packages

1. **xgboost-config.lsh**: Replace `dist/` paths with lush-pkg
2. **curl-config.lsh**: Replace `dist/` paths with lush-pkg
3. **libuv-config.lsh**: Replace 131-line vendored build with lush-pkg +
   thin wrapper (~25 lines)

Old install scripts and dist/ directories kept as fallback initially.

## Non-Goals

- **No dependency resolution between Lush packages.** `lush-pkg` handles
  external C libraries only.
- **No central package registry.** Each package declares its own dependency.
- **No version constraint solving.** One version per library, pinned by the
  package that needs it.
- **No Windows/macOS support.** Linux x86_64 only.
- **No system library detection/reuse.** We always build our own copy to
  guarantee the right version. System libraries (GSL, LAPACK) that don't
  need version pinning continue to use `find-shared-library`.

## File List

```
packages/lush-pkg/
  lush-pkg.lsh              # Utility library (~200 lines)
```

Plus modifications to:
- `packages/xgboost/xgboost-config.lsh` (replace dist/ with lush-pkg)
- `packages/curl/curl-config.lsh` (replace dist/ with lush-pkg)
- `packages/libuv/libuv-config.lsh` (replace 131-line vendor build with
  lush-pkg + thin wrapper)
