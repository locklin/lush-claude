# libc Directory Audit: lsh/libc/

Audit date: 2026-02-25

## Overview

The `lsh/libc/` directory provides C-interface utilities: stdio wrappers, file
descriptor operations, memory allocation, pointer manipulation, endian handling,
and build helpers.

Files examined (11 .lsh, 2 .c):
- `stdio.lsh` (789 lines) — C stdio wrappers (fopen, fread, fwrite, fseek, etc.)
- `libc.lsh` (302 lines) — malloc, memcpy, pointer ops, peek/poke
- `files.lsh` (205 lines) — Unix file descriptor wrappers
- `shell.lsh` (356 lines) — pure Lush shell utilities (no C code)
- `cparse.lsh` (276 lines) — pure Lush C header parser
- `constants.lsh` (114 lines) — pure Lush symbolic constants
- `fortran.lsh` (130 lines) — pure Lush fortran library locator
- `make.lsh` (323 lines) — pure Lush build system
- `mallinfo.lsh` (89 lines) — mallinfo/mallopt wrappers
- `stopwatch.lsh` (47 lines) — gettimeofday timer
- `add-makehelp.lsh` (119 lines) — pure Lush help generator
- `C/stdio.c` (1598 lines) — DH-generated C from stdio.lsh
- `C/files.c` (163 lines) — DH-generated C from files.lsh

Note: `C/stdio.c` and `C/files.c` are DH-compiler generated. Fixes go in the
`.lsh` source files; the C files are regenerated on next `dhc-make`.

---

## stdio.lsh — Two Real Issues

### Issue 1: fscan-int — %d format with intg variable (line 609)

```lisp
(de fscan-int (fp)
  ((-gptr- "FILE *") fp)
  (let*((result 0))
    ((-int-) result)
    #{ fscanf($fp,"%d",&$result); #}
    result))
```

`result` is `(-int-)` which compiles to `intg` (= `long`, 8 bytes on LP64).
But `%d` tells fscanf to write `sizeof(int)` = 4 bytes into `&result`.

**Problem:** This is undefined behavior (size mismatch in fscanf). On
little-endian x86_64, it happens to work for non-negative values (writes low
4 bytes, high 4 remain 0 from init). But negative values are broken: fscanf
writes 4-byte 2's complement (e.g. 0xFFFFFFFF for -1), the high 4 bytes
remain 0, so the result is 4294967295 instead of -1.

**Fix:** Use `%ld` to match `intg`/`long*`:
```lisp
#{ fscanf($fp,"%ld",&$result); #}
```

### Issue 2: fread-int — sizeof(int) read into intg variable (lines 430-439)

```lisp
(de fread-int (file)
  ...
  (let ((val 0))
    ((-int-) val)
    #{
      fread((char *)&$val, sizeof(int), 1, (FILE *)$file);
      if (little_endian_p) C_reverse_n(&$val, sizeof(int), 1);
    #}
  val ))
```

`val` is `intg` (8 bytes). `fread` reads `sizeof(int)` = 4 bytes into `&val`.
On LE x86_64 this works due to the 0-init filling the high bytes. On BE it
would be completely wrong (4 value bytes in high position, zeros in low).

The complementary `fwrite-int` handles this correctly by using a temp:
```c
int s = $val;  // truncate to 4 bytes
fwrite((char *)&s, sizeof(int), 1, ...);
```

**Fix:** Use a matching temp int for the fread side:
```lisp
#{{
  int s = 0;
  fread((char *)&s, sizeof(int), 1, (FILE *)$file);
  if (little_endian_p) C_reverse_n(&s, sizeof(int), 1);
  $val = s;
}#}
```

---

## libc.lsh — One Minor Issue

### Issue 3: ptr-str — int len for strlen result (line 123)

```lisp
#{{
   int len = strlen((char*)$g);
   Msrg_resize($s, len+1);
   memcpy($s->data, $g, len);
   ((char*)($s->data))[len] = 0;
}#}
```

`strlen` returns `size_t`; storing in `int` truncates for strings > 2GB.

**Severity:** Very low. Strings > 2GB are unrealistic. But for consistency with
the rest of the 64-bit cleanup, could change to `size_t len` or `intg len`.

---

## Items Examined and Found Clean

### libc.lsh — malloc, memcpy, gptr+, gptr-
All use `(-int-)` which maps to `intg` = 64-bit `long`. The C functions
receive `long` values that implicitly convert to `size_t`. No issue.

### libc.lsh — peek-int / poke-int (lines 205-211)
Intentionally dereference as `int *` to read/write 4-byte C int values.
The `(int)$v` cast in poke-int is correct truncation for a 4-byte write.

### libc.lsh — testbit (line 73)
Uses `(int)($v & (1<<$b))`. Limited to bits 0-31 by design (doc says
"b must be between 0 and 31"). The `(int)` cast is harmless since it's
testing truthiness.

### stdio.lsh — fwrite-int (lines 452-461)
Uses `int s = $val;` temp before `fwrite` — correct for writing 4 bytes.

### stdio.lsh — ftell/fseek (lines 188-272)
Already handles large files correctly: ftell returns `(-real-)` (double),
fseek uses `fseeko`/`off_t` when `HAVE_FSEEKO` is defined. Clean.

### stdio.lsh — file-size (lines 682-694)
Returns `(-real-)` (double) from `buf.st_size` via `(double)buf.st_size`.
Handles files up to 2^53 bytes. Clean.

### stdio.lsh — fgets, fscan-str inline C
Use `int size` for `strlen` result, but buffer sizes are bounded (1024 for
fscan-str, user-provided maxsize for fgets). Practical non-issue.

### files.lsh — fd-open, fd-close, fd-fcntl
All use `(-int-)` which maps to `intg`. File descriptors are small ints.
The `(long)` cast in fcntl is correct. Clean.

### mallinfo.lsh — mallinfo, mallopt
Uses `%d` for mallinfo struct fields, which are `int` in the struct
definition. Correct.

### stopwatch.lsh
Uses `(-double-)` and `(-gptr-)`. No int issues. Clean.

### Pure Lush files (no C code)
shell.lsh, cparse.lsh, constants.lsh, fortran.lsh, make.lsh,
add-makehelp.lsh — no C code, no 32-bit issues.

---

## Summary

| File | Issues | Severity |
|------|--------|----------|
| `stdio.lsh` | 2 | Medium — fscanf format mismatch (real UB), fread size mismatch |
| `libc.lsh` | 1 | Very low — int len for strlen |
| All others | 0 | Clean |

### Priority Assessment

**Should fix:**
- `stdio.lsh:609` — fscanf `%d` with intg variable. Real UB, negative values
  broken. Change to `%ld`.
- `stdio.lsh:436-437` — fread into intg with sizeof(int). Use int temp like
  fwrite-int does.

**Nice to fix:**
- `libc.lsh:123` — `int len` → `size_t len` for strlen in ptr-str.
