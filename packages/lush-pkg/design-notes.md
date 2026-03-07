# lush-pkg Design Notes

## Architecture

### Problem

Several Lush packages depend on external C libraries (xgboost, curl, libuv) that
need specific versions or build options not available via system package managers.
Each package had its own bespoke install script and config file with duplicated
download/checksum/build logic.

**Version safety problem**: System-installed libraries may be the wrong version.
For example, system curl (8.5.0) lacks WebSocket support; system libuv (1.0.0) is
missing most of the API. Accidentally linking against a system library instead of
the locally-built one causes segfaults or null pointer dereferences at runtime.

### Solution

`lush-pkg.lsh` is a small utility library (~190 lines) providing common functions
for downloading, verifying, building, and installing external C library
dependencies. Inspired by R's package management model:

- **Decentralized specs.** Each package declares its own dependency inline in its
  `*-config.lsh` file (like R's DESCRIPTION). There is no global registry or
  catalog.

- **Per-user prefix.** Libraries build into `~/.lush/local/` (like R's
  `~/R/x86_64-.../4.x/`). Each user has their own independent copy. Overridable
  via `(defvar *lush-pkg-prefix* "/other/path")` in `lushrc.lsh`.

- **Build from source at install time.** First load triggers a build with progress
  messages. Subsequent loads are instant (artifact check passes, no work done).

- **Version-safe linking.** The `-Wl,-rpath` flag tells the dynamic linker to look
  in `~/.lush/local/lib/` FIRST, before system paths, ensuring the declared
  library version is loaded at runtime.

- **Self-contained.** Zero Lush package dependencies. Uses only base Lush
  functions and standard Linux tools (wget, sha256sum, tar, cmake, make, gcc).

### Package Classification

**Managed by lush-pkg** (external C library, version-sensitive):
- **xgboost** -- cmake build, no system package available (v3.2.0)
- **curl** -- autotools build, system version too old for WebSocket (v8.12.1)
- **libuv** -- cmake build, system version far too old (v1.49.2 vs system 1.0.0)

**Self-contained** (source checked into git, no lush-pkg needed):
- **json** -- yyjson source (2 files) checked in
- **sqlite** -- sqlite3 amalgamation (2 files) checked in

**System library** (no version pinning needed):
- **gsl**, **lapack**, **fftw** -- use `find-shared-library` as before

### Directory Layout

```
~/.lush/local/
  include/       # installed headers (e.g. uv.h, curl/curl.h)
  lib/           # installed .so / .a (e.g. libuv.so, libcurl.so)
  src/           # extracted source trees (build workspace)
  tarballs/      # downloaded archives (cached across rebuilds)
```

### Compiler Integration

On load, `lush-pkg.lsh` prepends `~/.lush/local/include` to `c-include-path` and
`~/.lush/local/lib` to `shared-library-path`. This makes `dhc-generate-include-flags`
automatically emit `-I ~/.lush/local/include`, and `find-shared-library` searches
`~/.lush/local/lib/`. Registration is idempotent.

`lush-pkg-link-flags` returns `-L<prefix>/lib -Wl,-rpath,<prefix>/lib` for
lushmake link rules. The rpath is critical for runtime version safety.

## API Summary

```
(lush-pkg-ensure <name> <spec>)
```
Main entry point. Ensures an external C library is built and installed. If the
check artifact already exists, returns immediately. Otherwise: download, verify
SHA-256, extract, build, install. Idempotent.

Spec is an alist with keys: `version`, `url`, `sha256`, `build` (cmake or
autotools), `check` (relative path to verify), and optional `cmake-args`,
`configure-args`, `srcdir`.

```
(lush-pkg-installed-p <check>)
```
Returns t if `<check>` (path relative to prefix) exists.

```
(lush-pkg-link-flags)
```
Returns `"-L<prefix>/lib -Wl,-rpath,<prefix>/lib"` for lushmake rules.

```
(lush-pkg-prefix)
```
Returns the current prefix path (default `~/.lush/local`).

### Example Usage (from a *-config.lsh)

```lush
(libload "lush-pkg/lush-pkg")
(lush-pkg-ensure "curl"
  '((version . "8.12.1")
    (url . "https://curl.se/download/curl-8.12.1.tar.gz")
    (sha256 . "7b40ea64...")
    (build . autotools)
    (configure-args . "--enable-websockets --with-openssl --disable-ldap --disable-docs")
    (check . "lib/libcurl.so")))
```

## Implementation Details

### Build Strategies

**cmake** (xgboost, libuv): `mkdir build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=$PREFIX $CMAKE_ARGS && make -j$(nproc) && make install`

**autotools** (curl): `./configure --prefix=$PREFIX $CONFIGURE_ARGS && make -j$(nproc) && make install`

Both executed via `(sys ...)` from Lush. Non-zero exit codes raise errors.

### Internal Helpers

- `_lush-pkg-run` -- run shell command via `(sys ...)`, error on non-zero exit
- `_lush-pkg-url-basename` -- extract filename from URL using `(basename url)`
- `_lush-pkg-strip-tar-suffix` -- strip `.tar.gz` or `.tgz` from filename
- `_lush-pkg-build-cmake` -- cmake build strategy
- `_lush-pkg-build-autotools` -- autotools build strategy

### Why wget over curl for downloads

wget is used to avoid a bootstrapping problem: when building libcurl itself, curl
is not yet available.

### Impact on libuv-config.lsh

The largest change. Previously libuv-config.lsh compiled ~49 vendored source files
into a single .so using lushmake (131 lines). After conversion, libuv is built as
a standalone shared library by cmake into `~/.lush/local/`, and libuv-config.lsh
just compiles the thin C wrapper (~25 lines). The `vendor-libuv.sh` script and
vendored `include/`/`src/` directories are no longer needed.

### Converted Packages

All three target packages have been converted:
- `packages/xgboost/xgboost-config.lsh` -- uses lush-pkg with cmake
- `packages/curl/curl-config.lsh` -- uses lush-pkg with autotools
- `packages/libuv/libuv-config.lsh` -- uses lush-pkg with cmake (replacing 131-line vendor build)

## Known Issues / Limitations

- **No dependency resolution between Lush packages.** lush-pkg handles external C
  libraries only, not inter-package Lush dependencies.
- **No central package registry.** Each package declares its own dependency inline.
- **No version constraint solving.** One version per library, pinned by the package
  that needs it.
- **Linux x86_64 only.** No Windows/macOS support.
- **No system library reuse.** Always builds a private copy to guarantee the right
  version. System libraries (GSL, LAPACK) that don't need version pinning continue
  to use `find-shared-library`.
- **External tool requirements.** Requires wget, sha256sum, tar, make, gcc, and
  cmake (for cmake-type packages) to be installed on the system.
