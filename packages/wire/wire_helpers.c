/* wire_helpers.c -- DX-registered C functions for wire IPC
 *
 * Provides:
 *   - memstream-based bwrite/bread (eliminates temp-file serialization)
 *   - non-blocking recv for incremental reads
 *   - time function for keepalive/timeout
 *
 * Loaded via LushMake (mod-load), registered via init_wire_helpers.
 */

#include "wire_helpers.h"
#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>

/* Helper: get data pointer from a struct index (the Lush-side index, not dhc idx) */
#define INDEX_DATA_PTR(ind) \
  ((unsigned char*)(ind)->st->srg.data + (ind)->offset)

/* ================================================================
 * Memstream state (module-level statics)
 *
 * Same pattern as bwrite/bread: one active memstream at a time.
 * Each call to _wire-memstream-open cleans up any previous state.
 * ================================================================ */

static FILE   *ms_file = NULL;
static char   *ms_buf  = NULL;
static size_t  ms_size = 0;
static at     *ms_at   = NULL;  /* the Lush at* wrapping ms_file */

/* Clean up previous memstream, preventing GC double-close */
static void ms_cleanup(void)
{
  if (ms_at) {
    /* Prevent GC file_dispose from closing our FILE* */
    ms_at->Object = NULL;
    UNLOCK(ms_at);
    ms_at = NULL;
  }
  if (ms_file) {
    fclose(ms_file);
    ms_file = NULL;
  }
  if (ms_buf) {
    free(ms_buf);
    ms_buf = NULL;
  }
  ms_size = 0;
}

/* ----------------------------------------------------------------
 * _wire-memstream-open () -> FILE_WO
 *
 * Create an open_memstream-backed write handle suitable for
 * (writing <handle> (bwrite obj)).
 * ---------------------------------------------------------------- */
DX(xwire_memstream_open)
{
  at *result;

  ARG_NUMBER(0);

  /* Clean up any previous memstream */
  ms_cleanup();

  ms_file = open_memstream(&ms_buf, &ms_size);
  if (!ms_file)
    error(NIL, "open_memstream failed", NIL);

  result = new_extern(&file_W_class, ms_file);
  ms_at = result;
  LOCK(ms_at);   /* prevent premature GC */
  return result;
}

/* ----------------------------------------------------------------
 * _wire-memstream-get-size () -> int
 *
 * Return the current memstream buffer size (after fflush).
 * Must be called after (writing ...) which does fflush but not fclose.
 * ---------------------------------------------------------------- */
DX(xwire_memstream_get_size)
{
  ARG_NUMBER(0);

  if (!ms_file)
    error(NIL, "no active memstream", NIL);

  fflush(ms_file);
  return NEW_NUMBER((int)ms_size);
}

/* ----------------------------------------------------------------
 * _wire-memstream-get-bytes <idx1-ubyte> -> int
 *
 * Copy memstream data into the provided idx1 of ubyte.
 * Returns the number of bytes copied.
 * The idx must be large enough to hold ms_size bytes.
 * ---------------------------------------------------------------- */
DX(xwire_memstream_get_bytes)
{
  struct index *ind;
  unsigned char *dest;
  int n;

  ARG_NUMBER(1);
  ALL_ARGS_EVAL;

  if (!ms_file || !ms_buf)
    error(NIL, "no active memstream", NIL);

  fflush(ms_file);
  n = (int)ms_size;

  /* Get the index pointer (struct index, not struct idx) */
  ind = AINDEX(1);
  dest = INDEX_DATA_PTR(ind);

  if (ind->dim[0] < n)
    error(NIL, "idx too small for memstream data", NEW_NUMBER(n));

  memcpy(dest, ms_buf, n);
  return NEW_NUMBER(n);
}

/* ----------------------------------------------------------------
 * _wire-memstream-close () -> ()
 *
 * Close the memstream FILE* and free the buffer.
 * Must be called explicitly since (writing f ...) only does fflush.
 * ---------------------------------------------------------------- */
DX(xwire_memstream_close)
{
  ARG_NUMBER(0);
  ms_cleanup();
  return NIL;
}

/* ----------------------------------------------------------------
 * _wire-fmemopen-read <idx1-ubyte> <int> -> FILE_RO
 *
 * Create a fmemopen-backed read handle for (reading <handle> (bread)).
 * The idx data is copied to an internal buffer so fmemopen has a
 * stable pointer to read from.
 * ---------------------------------------------------------------- */

static char *fmem_buf = NULL;

DX(xwire_fmemopen_read)
{
  struct index *ind;
  unsigned char *src;
  int n;
  FILE *f;

  ARG_NUMBER(2);
  ALL_ARGS_EVAL;
  ind = AINDEX(1);
  n = AINTEGER(2);

  src = INDEX_DATA_PTR(ind);

  /* Copy data to internal buffer */
  if (fmem_buf) { free(fmem_buf); fmem_buf = NULL; }
  fmem_buf = malloc(n > 0 ? n : 1);
  if (!fmem_buf)
    error(NIL, "malloc failed", NIL);
  if (n > 0)
    memcpy(fmem_buf, src, n);

  f = fmemopen(fmem_buf, n > 0 ? n : 1, "rb");
  if (!f) {
    free(fmem_buf);
    fmem_buf = NULL;
    error(NIL, "fmemopen failed", NIL);
  }

  return new_extern(&file_R_class, f);
}

/* ----------------------------------------------------------------
 * _wire-recv-nonblock <fd> <idx1-ubyte> <offset> <maxlen> -> int
 *
 * Non-blocking receive: returns >0 bytes read, 0=EAGAIN, -1=EOF/error.
 * Uses recv(fd, ..., MSG_DONTWAIT).
 * ---------------------------------------------------------------- */
DX(xwire_recv_nonblock)
{
  struct index *ind;
  unsigned char *buf;
  int fd, offset, maxlen, got;

  ARG_NUMBER(4);
  ALL_ARGS_EVAL;
  fd = AINTEGER(1);
  ind = AINDEX(2);
  offset = AINTEGER(3);
  maxlen = AINTEGER(4);

  buf = INDEX_DATA_PTR(ind);

  got = recv(fd, buf + offset, maxlen, MSG_DONTWAIT);
  if (got < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return NEW_NUMBER(0);
    return NEW_NUMBER(-1);
  }
  if (got == 0)
    return NEW_NUMBER(-1);  /* EOF */
  return NEW_NUMBER(got);
}

/* ----------------------------------------------------------------
 * _wire-time-seconds () -> int
 *
 * Current time in seconds (epoch). For timeout/keepalive.
 * ---------------------------------------------------------------- */
DX(xwire_time_seconds)
{
  ARG_NUMBER(0);
  return NEW_NUMBER((int)time(NULL));
}

/* ================================================================
 * Initialization — called by mod-load (SN3-style: void(void))
 * Version symbols for compatibility checking.
 * ================================================================ */

int majver_wire_helpers = TLOPEN_MAJOR;
int minver_wire_helpers = TLOPEN_MINOR;

void init_wire_helpers(void)
{
  dx_define("_wire-memstream-open",      xwire_memstream_open);
  dx_define("_wire-memstream-get-size",   xwire_memstream_get_size);
  dx_define("_wire-memstream-get-bytes",  xwire_memstream_get_bytes);
  dx_define("_wire-memstream-close",      xwire_memstream_close);
  dx_define("_wire-fmemopen-read",        xwire_fmemopen_read);
  dx_define("_wire-recv-nonblock",        xwire_recv_nonblock);
  dx_define("_wire-time-seconds",         xwire_time_seconds);
}
