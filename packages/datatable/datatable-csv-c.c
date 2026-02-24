/* datatable-csv-c.c -- C-accelerated CSV parser for Lush DataTable
 *
 * Fast CSV/TSV reading with per-column type inference.
 * Uses mmap for regular files, getline for .gz pipes.
 * Supports comma and tab delimiters (auto-detected from header).
 * Handles quoted fields, CR/LF line endings.
 * Date/time columns are auto-detected and parsed as int64 microseconds.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "datatable-csv-c.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* ================================================================
 * I/O Abstraction: mmap for regular files, getline for pipes
 * ================================================================ */

typedef struct {
    int use_mmap;
    /* mmap fields */
    char *mmap_base;
    char *mmap_pos;
    char *mmap_end;
    size_t mmap_size;
    int fd;
    /* getline fields */
    FILE *fp;
    int is_pipe;
    char *line_buf;
    size_t line_cap;
} _dt_io;

/* Open a file for reading. Returns 0 on success, -1 on error. */
static int _dt_io_open(_dt_io *io, const char *filename) {
    memset(io, 0, sizeof(*io));
    size_t len = strlen(filename);

    if (len >= 3 && strcmp(filename + len - 3, ".gz") == 0) {
        /* Compressed: use popen + getline */
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "zcat '%s'", filename);
        io->use_mmap = 0;
        io->is_pipe = 1;
        io->fp = popen(cmd, "r");
        return io->fp ? 0 : -1;
    }

    /* Try mmap for regular files */
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        close(fd);
        /* Fall back to FILE* for non-regular or empty files */
        io->use_mmap = 0;
        io->is_pipe = 0;
        io->fp = fopen(filename, "r");
        return io->fp ? 0 : -1;
    }

    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        io->use_mmap = 0;
        io->is_pipe = 0;
        io->fp = fopen(filename, "r");
        return io->fp ? 0 : -1;
    }

    madvise(base, (size_t)st.st_size, MADV_SEQUENTIAL);

    io->use_mmap = 1;
    io->mmap_base = (char *)base;
    io->mmap_pos = io->mmap_base;
    io->mmap_end = io->mmap_base + st.st_size;
    io->mmap_size = (size_t)st.st_size;
    io->fd = fd;
    return 0;
}

/* Get next line. Returns line pointer and sets *out_len to length
 * (including newline if present). Returns NULL at EOF.
 * For mmap: returns pointer directly into mapped memory (zero-copy).
 * For getline: returns internal buffer pointer. */
static char *_dt_io_next_line(_dt_io *io, ssize_t *out_len) {
    if (io->use_mmap) {
        if (io->mmap_pos >= io->mmap_end) return NULL;
        char *start = io->mmap_pos;
        char *p = start;
        while (p < io->mmap_end && *p != '\n') p++;
        if (p < io->mmap_end) p++; /* include the newline */
        io->mmap_pos = p;
        *out_len = p - start;
        return start;
    } else {
        ssize_t len = getline(&io->line_buf, &io->line_cap, io->fp);
        if (len < 0) return NULL;
        *out_len = len;
        return io->line_buf;
    }
}

/* Close and free all resources. */
static void _dt_io_close(_dt_io *io) {
    if (io->use_mmap) {
        if (io->mmap_base) munmap(io->mmap_base, io->mmap_size);
        if (io->fd >= 0) close(io->fd);
    } else {
        free(io->line_buf);
        if (io->fp) {
            if (io->is_pipe) pclose(io->fp);
            else fclose(io->fp);
        }
    }
    memset(io, 0, sizeof(*io));
}

/* ================================================================
 * Legacy file open/close (kept for legacy per-column readers)
 * ================================================================ */

static FILE *_dt_open(const char *filename, int *is_pipe) {
    size_t len = strlen(filename);
    if (len >= 3 && strcmp(filename + len - 3, ".gz") == 0) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "zcat '%s'", filename);
        *is_pipe = 1;
        return popen(cmd, "r");
    }
    *is_pipe = 0;
    return fopen(filename, "r");
}

static void _dt_close(FILE *fp, int is_pipe) {
    if (is_pipe) pclose(fp);
    else fclose(fp);
}

/* ================================================================
 * Internal: Field extraction from a line
 * ================================================================
 *
 * Parses fields from a line buffer, handling quoted fields.
 * Returns the number of fields extracted.
 * field_starts[i] and field_lens[i] describe each field's location
 * within the line buffer. Quotes are stripped from field boundaries.
 */

static int _dt_split_fields(char *line, ssize_t line_len, char delim,
                             int *field_starts, int *field_lens,
                             int max_fields) {
    int nf = 0;
    int i = 0;

    /* Strip trailing newline/CR */
    while (line_len > 0 && (line[line_len-1] == '\n' || line[line_len-1] == '\r'))
        line_len--;

    if (line_len == 0) return 0;

    /* Parse fields. We always have at least one field if line is non-empty. */
    while (nf < max_fields) {
        int start = i;
        int flen;

        if (i < line_len && line[i] == '"') {
            /* Quoted field: scan until closing quote */
            i++; /* skip opening quote */
            start = i;
            while (i < line_len) {
                if (line[i] == '"') {
                    if (i + 1 < line_len && line[i+1] == '"') {
                        i += 2; /* escaped quote */
                    } else {
                        break; /* closing quote */
                    }
                } else {
                    i++;
                }
            }
            flen = i - start;
            if (i < line_len && line[i] == '"') i++; /* skip closing quote */
        } else {
            /* Unquoted field: scan until delimiter or end */
            while (i < line_len && line[i] != delim)
                i++;
            flen = i - start;
            /* Strip whitespace from unquoted field */
            while (flen > 0 && (line[start] == ' ' || line[start] == '\t')) {
                start++; flen--;
            }
            while (flen > 0 && (line[start + flen - 1] == ' ' ||
                                line[start + flen - 1] == '\t')) {
                flen--;
            }
        }

        field_starts[nf] = start;
        field_lens[nf] = flen;
        nf++;

        /* If we hit a delimiter, skip it and continue; otherwise we're done */
        if (i < line_len && line[i] == delim) {
            i++;
        } else {
            break;
        }
    }
    return nf;
}

/* ================================================================
 * Internal: Type detection for a single field value
 * ================================================================
 * Returns DT_COL_INT, DT_COL_DOUBLE, or DT_COL_STRING.
 * Empty fields are ignored (return -1).
 */

static int _dt_detect_field_type(const char *field, int len) {
    if (len == 0) return -1; /* empty, skip */

    /* Check for common NA/null markers */
    if (len == 2 && (strncmp(field, "NA", 2) == 0 ||
                     strncmp(field, "na", 2) == 0))
        return -1; /* treat as missing */
    if (len == 3 && (strncasecmp(field, "nan", 3) == 0 ||
                     strncasecmp(field, "n/a", 3) == 0))
        return -1;
    if (len == 4 && strncasecmp(field, "null", 4) == 0)
        return -1;
    if (len == 0 || (len == 1 && field[0] == '.'))
        return -1;

    /* Try integer first */
    {
        char buf[64];
        int blen = len < 63 ? len : 63;
        memcpy(buf, field, blen);
        buf[blen] = '\0';

        char *end;
        errno = 0;
        long lval = strtol(buf, &end, 10);
        (void)lval;
        if (errno == 0 && end == buf + blen && blen > 0) {
            /* Check it's not too large for int32 */
            if (lval >= INT_MIN && lval <= INT_MAX)
                return DT_COL_INT;
            else
                return DT_COL_DOUBLE; /* large integer, use double */
        }
    }

    /* Try double */
    {
        char buf[64];
        int blen = len < 63 ? len : 63;
        memcpy(buf, field, blen);
        buf[blen] = '\0';

        char *end;
        errno = 0;
        double dval = strtod(buf, &end);
        (void)dval;
        if (errno == 0 && end == buf + blen && blen > 0)
            return DT_COL_DOUBLE;
    }

    return DT_COL_STRING;
}

/* ================================================================
 * Internal: Date format detection
 * ================================================================ */

#define DT_NUM_DATE_FMTS 11
#define NANOS_PER_SEC_CSV 1000000000LL

static const char *_dt_date_formats[DT_NUM_DATE_FMTS] = {
    "%Y-%m-%dT%H:%M:%S",    /* 0: ISO 8601 full */
    "%Y-%m-%d %H:%M:%S",    /* 1: ISO with space */
    "%Y-%m-%dT%H:%M",       /* 2: ISO no seconds */
    "%Y-%m-%d %H:%M",       /* 3: space, no seconds */
    "%Y-%m-%d",              /* 4: date only */
    "%Y/%m/%d %H:%M:%S",    /* 5: slash full */
    "%Y/%m/%d",              /* 6: slash date only */
    "%m/%d/%Y %H:%M:%S",    /* 7: US full */
    "%m/%d/%Y %H:%M",       /* 8: US no seconds */
    "%m/%d/%Y",              /* 9: US date only */
    "%d-%b-%Y"               /* 10: day-abbrev-year */
};

/* Try each date format on a field. Returns format index (0-10) on success,
 * DT_FMT_HINT (-2) if hint matched, -1 on failure.
 * Accepts trailing fractional seconds/timezone chars. */
static int _dt_detect_date_format(const char *field, int len,
                                   const char *hint_date_fmt) {
    char buf[128];
    int blen = len < 127 ? len : 127;
    memcpy(buf, field, blen);
    buf[blen] = '\0';

    /* Quick reject: must have at least one digit and one separator */
    if (blen < 8) return -1;  /* shortest date: YYYY-M-D = 8 chars minimum */

    struct tm tm;

    /* Try user-supplied hint format first */
    if (hint_date_fmt) {
        memset(&tm, 0, sizeof(tm));
        const char *rest = strptime(buf, hint_date_fmt, &tm);
        if (rest) {
            if (*rest == '\0' || *rest == '.' || *rest == 'Z' ||
                *rest == '+' || *rest == '-' || *rest == ' ')
                return DT_FMT_HINT;
        }
    }

    for (int i = 0; i < DT_NUM_DATE_FMTS; i++) {
        memset(&tm, 0, sizeof(tm));
        const char *rest = strptime(buf, _dt_date_formats[i], &tm);
        if (rest) {
            /* Accept if rest is empty, or starts with '.', 'Z', '+', '-', or space */
            if (*rest == '\0' || *rest == '.' || *rest == 'Z' ||
                *rest == '+' || *rest == '-' || *rest == ' ')
                return i;
        }
    }
    return -1;
}

/* Type promotion rules for combining two field types.
 * stamp+stamp=stamp, stamp+numeric=string, stamp+string=string,
 * int+double=double, anything+string=string */
static int _dt_promote_type(int existing, int new_type) {
    if (existing == new_type) return existing;
    /* stamp + anything non-stamp = string */
    if (existing == DT_COL_STAMP || new_type == DT_COL_STAMP)
        return DT_COL_STRING;
    /* int + double = double */
    if ((existing == DT_COL_INT && new_type == DT_COL_DOUBLE) ||
        (existing == DT_COL_DOUBLE && new_type == DT_COL_INT))
        return DT_COL_DOUBLE;
    /* anything + string = string */
    return DT_COL_STRING;
}

/* ================================================================
 * Phase 1: Scan CSV file (now uses _dt_io for mmap benefit)
 * ================================================================ */

int dt_csv_scan(const char *filename,
                int *out_nrows, int *out_ncols,
                char *out_delim,
                int *col_types, int max_cols,
                char *name_buf, int name_buf_size,
                int *name_offsets,
                int *date_fmt_indices,
                char hint_delim,
                const char *hint_date_fmt) {
    _dt_io io;
    if (_dt_io_open(&io, filename) < 0) return -1;

    ssize_t len;
    char *line;

    /* Read header line */
    line = _dt_io_next_line(&io, &len);
    if (!line) { _dt_io_close(&io); return -1; }

    /* For mmap lines, we need a mutable copy for _dt_split_fields
     * (it doesn't modify, but the pointer type requires char*).
     * Actually _dt_split_fields only reads, so const_cast is safe.
     * But for header we need to copy since we extract names. */
    char *hdr_copy = malloc((size_t)len + 1);
    if (!hdr_copy) { _dt_io_close(&io); return -1; }
    memcpy(hdr_copy, line, (size_t)len);
    hdr_copy[len] = '\0';

    /* Detect delimiter: use hint if provided, else count tabs vs commas */
    char delim;
    if (hint_delim != 0) {
        delim = hint_delim;
    } else {
        int n_tabs = 0, n_commas = 0;
        for (ssize_t i = 0; i < len; i++) {
            if (hdr_copy[i] == '\t') n_tabs++;
            else if (hdr_copy[i] == ',') n_commas++;
        }
        delim = (n_tabs >= n_commas && n_tabs > 0) ? '\t' : ',';
    }
    *out_delim = delim;

    /* Parse header fields (heap-allocated to avoid stack overflow) */
    int lim = (max_cols < DT_MAX_COLS) ? max_cols : DT_MAX_COLS;
    int *field_starts = malloc(lim * sizeof(int));
    int *field_lens   = malloc(lim * sizeof(int));
    if (!field_starts || !field_lens) {
        free(field_starts); free(field_lens);
        free(hdr_copy); _dt_io_close(&io); return -1;
    }
    int ncols = _dt_split_fields(hdr_copy, len, delim,
                                  field_starts, field_lens, lim);
    *out_ncols = ncols;

    /* Copy column names */
    int buf_pos = 0;
    for (int i = 0; i < ncols; i++) {
        name_offsets[i] = buf_pos;
        int flen = field_lens[i];
        if (buf_pos + flen + 1 > name_buf_size) flen = name_buf_size - buf_pos - 1;
        if (flen < 0) flen = 0;
        memcpy(name_buf + buf_pos, hdr_copy + field_starts[i], flen);
        name_buf[buf_pos + flen] = '\0';
        buf_pos += flen + 1;
    }

    free(field_starts);
    free(field_lens);
    free(hdr_copy);

    /* Initialize type tracking */
    if (ncols <= 0) { _dt_io_close(&io); return -1; }
    int *type_seen = calloc((size_t)ncols, sizeof(int)); /* 0=no data, 1=has data */
    for (int i = 0; i < ncols; i++) {
        col_types[i] = DT_COL_INT; /* start optimistic */
        date_fmt_indices[i] = -1;
    }

    /* Allocate field arrays for data row parsing */
    int *fs = malloc(ncols * sizeof(int));
    int *fl = malloc(ncols * sizeof(int));
    if (!fs || !fl) {
        free(fs); free(fl); free(type_seen);
        _dt_io_close(&io); return -1;
    }

    /* Read data rows, detect types from first N rows, count all rows */
    int nrows = 0;
    int sample_rows = 0;

    /* For mmap lines we need a mutable copy for field parsing during sampling */
    char *line_copy = NULL;
    size_t line_copy_cap = 0;

    while ((line = _dt_io_next_line(&io, &len)) != NULL) {
        /* Strip trailing newline/CR to check for blank */
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        nrows++;

        /* Type detection from sample rows */
        if (sample_rows < DT_TYPE_SAMPLE_ROWS) {
            /* We need a mutable copy for _dt_split_fields (it doesn't
             * write, but field pointers reference into this buffer
             * and _dt_detect_field_type copies to its own buf). */
            if ((size_t)len + 1 > line_copy_cap) {
                line_copy_cap = (size_t)len * 2 + 256;
                free(line_copy);
                line_copy = malloc(line_copy_cap);
            }
            if (line_copy) {
                memcpy(line_copy, line, (size_t)len);
                line_copy[len] = '\0';

                int nf = _dt_split_fields(line_copy, len, delim, fs, fl, ncols);
                for (int j = 0; j < nf && j < ncols; j++) {
                    int ft = _dt_detect_field_type(line_copy + fs[j], fl[j]);
                    if (ft < 0) continue; /* missing/empty, skip */

                    /* If numeric detection returned STRING, try date detection */
                    if (ft == DT_COL_STRING) {
                        int dfmt = _dt_detect_date_format(line_copy + fs[j], fl[j],
                                                           hint_date_fmt);
                        if (dfmt >= 0 || dfmt == DT_FMT_HINT) {
                            ft = DT_COL_STAMP;
                            if (date_fmt_indices[j] < 0)
                                date_fmt_indices[j] = dfmt;
                        }
                    }

                    if (!type_seen[j]) {
                        col_types[j] = ft;
                        type_seen[j] = 1;
                    } else {
                        col_types[j] = _dt_promote_type(col_types[j], ft);
                    }
                }
            }
            sample_rows++;
        }
    }

    /* Columns with no data seen: default to string */
    for (int i = 0; i < ncols; i++) {
        if (!type_seen[i])
            col_types[i] = DT_COL_STRING;
    }

    free(line_copy);
    free(fs);
    free(fl);
    free(type_seen);
    _dt_io_close(&io);

    *out_nrows = nrows;
    return 0;
}

/* ================================================================
 * Phase 2: Read numeric columns (legacy)
 * ================================================================ */

int dt_csv_read_numeric(const char *filename,
                        char delim,
                        const int *col_indices, int n_cols,
                        double *data, int nrows) {
    int is_pipe = 0;
    FILE *fp = _dt_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Skip header */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _dt_close(fp, is_pipe); return -1; }

    /* Build reverse map: for each original column, which output column? */
    /* Use -1 for columns we don't care about */
    int max_col = 0;
    for (int j = 0; j < n_cols; j++)
        if (col_indices[j] > max_col) max_col = col_indices[j];

    int *out_map = malloc((max_col + 1) * sizeof(int));
    if (!out_map) { free(line); _dt_close(fp, is_pipe); return -1; }
    for (int i = 0; i <= max_col; i++) out_map[i] = -1;
    for (int j = 0; j < n_cols; j++)
        out_map[col_indices[j]] = j;

    int row = 0;
    while (row < nrows && (len = getline(&line, &cap, fp)) > 0) {
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        /* Fast field extraction + numeric parsing */
        int col = 0;
        char *p = line;
        char *line_end = line + slen;

        while (p < line_end && col <= max_col) {
            char *field_start = p;

            /* Find end of field */
            if (*p == '"') {
                /* Quoted field */
                p++;
                while (p < line_end) {
                    if (*p == '"') {
                        if (p + 1 < line_end && *(p+1) == '"') p += 2;
                        else { p++; break; }
                    } else p++;
                }
                if (p < line_end && *p == delim) p++;
            } else {
                while (p < line_end && *p != delim) p++;
                if (p < line_end) p++; /* skip delimiter */
            }

            if (out_map[col] >= 0) {
                /* Parse this field as double */
                char *end;
                double val = strtod(field_start, &end);
                /* Handle NA/NaN/missing -> NaN */
                if (end == field_start || (*end != delim && *end != '\n' &&
                    *end != '\r' && *end != '\0' && *end != '"' &&
                    end < line_end)) {
                    val = NAN;
                }
                data[row * n_cols + out_map[col]] = val;
            }
            col++;
        }

        /* Fill remaining columns with NaN if line was short */
        for (int j = 0; j < n_cols; j++) {
            if (col_indices[j] >= col)
                data[row * n_cols + j] = NAN;
        }

        row++;
    }

    free(out_map);
    free(line);
    _dt_close(fp, is_pipe);
    return (row >= nrows) ? 0 : -1;
}

/* ================================================================
 * Phase 2b: Read string column with dictionary encoding (legacy)
 * ================================================================ */

/* Simple hash table for string interning (open addressing) */
#define DT_HASH_SIZE 8192
#define DT_HASH_MASK (DT_HASH_SIZE - 1)

typedef struct {
    char *key;
    int code;
} _dt_hash_entry;

static unsigned int _dt_hash(const char *s, int len) {
    unsigned int h = 5381;
    for (int i = 0; i < len; i++)
        h = ((h << 5) + h) ^ (unsigned char)s[i];
    return h;
}

static int _dt_hash_find_or_insert(_dt_hash_entry *table,
                                    const char *key, int key_len,
                                    int *next_code,
                                    char *unique_buf, int unique_buf_size,
                                    int *unique_offsets, int max_unique,
                                    int *buf_pos) {
    unsigned int h = _dt_hash(key, key_len) & DT_HASH_MASK;
    for (int probe = 0; probe < DT_HASH_SIZE; probe++) {
        int idx = (h + probe) & DT_HASH_MASK;
        if (!table[idx].key) {
            /* Empty slot: insert */
            if (*next_code >= max_unique) return -1;
            if (*buf_pos + key_len + 1 > unique_buf_size) return -1;

            /* Copy string into unique buffer */
            memcpy(unique_buf + *buf_pos, key, key_len);
            unique_buf[*buf_pos + key_len] = '\0';
            unique_offsets[*next_code] = *buf_pos;
            table[idx].key = unique_buf + *buf_pos;
            table[idx].code = *next_code;
            *buf_pos += key_len + 1;
            int code = *next_code;
            (*next_code)++;
            return code;
        }
        if (strlen(table[idx].key) == (size_t)key_len &&
            memcmp(table[idx].key, key, key_len) == 0) {
            return table[idx].code;
        }
    }
    return -1; /* table full */
}

int dt_csv_read_string_col(const char *filename,
                           char delim, int col_idx,
                           int *codes, int nrows,
                           char *unique_buf, int unique_buf_size,
                           int *unique_offsets, int max_unique,
                           int *out_n_unique) {
    int is_pipe = 0;
    FILE *fp = _dt_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Skip header */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _dt_close(fp, is_pipe); return -1; }

    /* Initialize hash table */
    _dt_hash_entry *htable = calloc(DT_HASH_SIZE, sizeof(_dt_hash_entry));
    if (!htable) { free(line); _dt_close(fp, is_pipe); return -1; }

    int next_code = 0;
    int buf_pos = 0;
    int row = 0;

    while (row < nrows && (len = getline(&line, &cap, fp)) > 0) {
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        /* Find the target column */
        int col = 0;
        char *p = line;
        char *line_end = line + slen;
        char *field_start = p;
        int field_len = 0;

        while (p <= line_end) {
            field_start = p;

            if (p < line_end && *p == '"') {
                /* Quoted field */
                p++;
                field_start = p;
                while (p < line_end) {
                    if (*p == '"') {
                        if (p + 1 < line_end && *(p+1) == '"') p += 2;
                        else break;
                    } else p++;
                }
                field_len = p - field_start;
                if (p < line_end && *p == '"') p++;
                if (p < line_end && *p == delim) p++;
            } else {
                while (p < line_end && *p != delim) p++;
                field_len = p - field_start;
                /* Strip whitespace */
                while (field_len > 0 && (*field_start == ' ' || *field_start == '\t')) {
                    field_start++; field_len--;
                }
                while (field_len > 0 && (field_start[field_len-1] == ' ' ||
                       field_start[field_len-1] == '\t')) {
                    field_len--;
                }
                if (p < line_end) p++;
            }

            if (col == col_idx) break;
            col++;
        }

        if (col == col_idx) {
            int code = _dt_hash_find_or_insert(htable, field_start, field_len,
                                                &next_code,
                                                unique_buf, unique_buf_size,
                                                unique_offsets, max_unique,
                                                &buf_pos);
            codes[row] = (code >= 0) ? code : 0;
        } else {
            codes[row] = 0; /* column not found in this row */
        }

        row++;
    }

    *out_n_unique = next_code;

    free(htable);
    free(line);
    _dt_close(fp, is_pipe);
    return 0;
}

/* ================================================================
 * Phase 2c: Read a timestamp column (legacy)
 * ================================================================ */

int dt_csv_read_stamp_col(const char *filename, char delim,
                          int col_idx, int64_t *stamps, int nrows,
                          const char *fmt) {
    int is_pipe = 0;
    FILE *fp = _dt_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Skip header */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _dt_close(fp, is_pipe); return -1; }

    int row = 0;

    while (row < nrows && (len = getline(&line, &cap, fp)) > 0) {
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        /* Find the target column */
        int col = 0;
        char *p = line;
        char *line_end = line + slen;
        char *field_start = p;
        int field_len = 0;

        while (p <= line_end) {
            field_start = p;

            if (p < line_end && *p == '"') {
                /* Quoted field */
                p++;
                field_start = p;
                while (p < line_end) {
                    if (*p == '"') {
                        if (p + 1 < line_end && *(p+1) == '"') p += 2;
                        else break;
                    } else p++;
                }
                field_len = p - field_start;
                if (p < line_end && *p == '"') p++;
                if (p < line_end && *p == delim) p++;
            } else {
                while (p < line_end && *p != delim) p++;
                field_len = p - field_start;
                /* Strip whitespace */
                while (field_len > 0 && (*field_start == ' ' || *field_start == '\t')) {
                    field_start++; field_len--;
                }
                while (field_len > 0 && (field_start[field_len-1] == ' ' ||
                       field_start[field_len-1] == '\t')) {
                    field_len--;
                }
                if (p < line_end) p++;
            }

            if (col == col_idx) break;
            col++;
        }

        if (col == col_idx && field_len > 0) {
            /* Null-terminate the field */
            char buf[128];
            int blen = field_len < 127 ? field_len : 127;
            memcpy(buf, field_start, blen);
            buf[blen] = '\0';

            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            const char *rest = strptime(buf, fmt, &tm);
            if (rest) {
                time_t secs = timegm(&tm);
                int64_t frac_nanos = 0;
                /* Parse fractional seconds */
                if (*rest == '.') {
                    rest++;
                    int64_t mult = 100000000LL;
                    while (*rest >= '0' && *rest <= '9' && mult > 0) {
                        frac_nanos += (*rest - '0') * mult;
                        mult /= 10;
                        rest++;
                    }
                }
                stamps[row] = (int64_t)secs * NANOS_PER_SEC_CSV + frac_nanos;
            } else {
                stamps[row] = INT64_MIN; /* NaT sentinel */
            }
        } else {
            stamps[row] = INT64_MIN; /* missing field */
        }

        row++;
    }

    free(line);
    _dt_close(fp, is_pipe);
    return 0;
}

/* ================================================================
 * Per-string-column hash table context (dynamic resizing)
 * ================================================================ */

typedef struct {
    unsigned int *ht_hashes;   /* hash values in table */
    int *ht_codes;             /* code values in table (-1 = empty) */
    char **ht_keys;            /* key pointers in table */
    int *ht_key_lens;          /* key lengths in table */
    int ht_size, ht_mask;     /* power-of-2 size and mask */
    char *unique_buf;          /* points into caller's concatenated buffer */
    int buf_pos, buf_size;
    int *unique_offsets;       /* points into caller's concatenated offsets */
    int max_unique, next_code;
    int ht_count;              /* number of occupied slots */
} _dt_str_ctx;

static int _dt_str_ctx_init(_dt_str_ctx *ctx, char *unique_buf, int buf_size,
                             int *unique_offsets, int max_unique) {
    ctx->ht_size = 8192;
    ctx->ht_mask = ctx->ht_size - 1;
    ctx->ht_hashes = malloc(ctx->ht_size * sizeof(unsigned int));
    ctx->ht_codes  = malloc(ctx->ht_size * sizeof(int));
    ctx->ht_keys   = malloc(ctx->ht_size * sizeof(char *));
    ctx->ht_key_lens = malloc(ctx->ht_size * sizeof(int));
    if (!ctx->ht_hashes || !ctx->ht_codes || !ctx->ht_keys || !ctx->ht_key_lens) {
        free(ctx->ht_hashes); free(ctx->ht_codes);
        free(ctx->ht_keys); free(ctx->ht_key_lens);
        return -1;
    }
    for (int i = 0; i < ctx->ht_size; i++) ctx->ht_codes[i] = -1;
    ctx->unique_buf = unique_buf;
    ctx->buf_pos = 0;
    ctx->buf_size = buf_size;
    ctx->unique_offsets = unique_offsets;
    ctx->max_unique = max_unique;
    ctx->next_code = 0;
    ctx->ht_count = 0;
    return 0;
}

static void _dt_str_ctx_free(_dt_str_ctx *ctx) {
    free(ctx->ht_hashes);
    free(ctx->ht_codes);
    free(ctx->ht_keys);
    free(ctx->ht_key_lens);
}

/* Rehash: double the table size and reinsert all entries */
static int _dt_str_ctx_rehash(_dt_str_ctx *ctx) {
    int new_size = ctx->ht_size * 2;
    int new_mask = new_size - 1;
    unsigned int *new_hashes = malloc(new_size * sizeof(unsigned int));
    int *new_codes  = malloc(new_size * sizeof(int));
    char **new_keys = malloc(new_size * sizeof(char *));
    int *new_klens  = malloc(new_size * sizeof(int));
    if (!new_hashes || !new_codes || !new_keys || !new_klens) {
        free(new_hashes); free(new_codes);
        free(new_keys); free(new_klens);
        return -1;
    }
    for (int i = 0; i < new_size; i++) new_codes[i] = -1;

    /* Reinsert all existing entries */
    for (int i = 0; i < ctx->ht_size; i++) {
        if (ctx->ht_codes[i] < 0) continue;
        unsigned int h = ctx->ht_hashes[i] & new_mask;
        for (int probe = 0; probe < new_size; probe++) {
            int idx = (h + probe) & new_mask;
            if (new_codes[idx] < 0) {
                new_hashes[idx] = ctx->ht_hashes[i];
                new_codes[idx]  = ctx->ht_codes[i];
                new_keys[idx]   = ctx->ht_keys[i];
                new_klens[idx]  = ctx->ht_key_lens[i];
                break;
            }
        }
    }

    free(ctx->ht_hashes); free(ctx->ht_codes);
    free(ctx->ht_keys); free(ctx->ht_key_lens);
    ctx->ht_hashes = new_hashes;
    ctx->ht_codes  = new_codes;
    ctx->ht_keys   = new_keys;
    ctx->ht_key_lens = new_klens;
    ctx->ht_size = new_size;
    ctx->ht_mask = new_mask;
    return 0;
}

/* Find or insert a string key. Returns the code, or -1 on error. */
static int _dt_str_ctx_intern(_dt_str_ctx *ctx, const char *key, int key_len) {
    unsigned int h = _dt_hash(key, key_len);
    unsigned int slot = h & ctx->ht_mask;

    for (int probe = 0; probe < ctx->ht_size; probe++) {
        int idx = (slot + probe) & ctx->ht_mask;
        if (ctx->ht_codes[idx] < 0) {
            /* Empty slot: insert new entry */
            if (ctx->next_code >= ctx->max_unique) return -1;
            if (ctx->buf_pos + key_len + 1 > ctx->buf_size) return -1;

            /* Copy string into unique buffer */
            memcpy(ctx->unique_buf + ctx->buf_pos, key, key_len);
            ctx->unique_buf[ctx->buf_pos + key_len] = '\0';
            ctx->unique_offsets[ctx->next_code] = ctx->buf_pos;

            ctx->ht_hashes[idx] = h;
            ctx->ht_codes[idx]  = ctx->next_code;
            ctx->ht_keys[idx]   = ctx->unique_buf + ctx->buf_pos;
            ctx->ht_key_lens[idx] = key_len;

            ctx->buf_pos += key_len + 1;
            int code = ctx->next_code;
            ctx->next_code++;
            ctx->ht_count++;

            /* Check load factor: rehash if > 70% */
            if (ctx->ht_count * 10 > ctx->ht_size * 7) {
                if (_dt_str_ctx_rehash(ctx) < 0) return -1;
            }
            return code;
        }
        /* Check if this slot matches */
        if (ctx->ht_hashes[idx] == h &&
            ctx->ht_key_lens[idx] == key_len &&
            memcmp(ctx->ht_keys[idx], key, key_len) == 0) {
            return ctx->ht_codes[idx];
        }
    }
    return -1; /* table full (shouldn't happen with rehashing) */
}

/* ================================================================
 * Timestamp field parser (microseconds, matching _td-parse-stamp-at)
 * ================================================================ */

static int64_t _dt_parse_stamp_field(const char *field, int flen, int fmt_idx,
                                      const char *hint_date_fmt) {
    if (flen <= 0) return INT64_MIN;
    if (fmt_idx == DT_FMT_HINT) {
        if (!hint_date_fmt) return INT64_MIN;
    } else if (fmt_idx < 0 || fmt_idx >= DT_NUM_DATE_FMTS) {
        return INT64_MIN;
    }

    /* Check for NA/null markers */
    if (flen == 2 && (strncmp(field, "NA", 2) == 0 ||
                      strncmp(field, "na", 2) == 0))
        return INT64_MIN;
    if (flen == 3 && strncasecmp(field, "nan", 3) == 0)
        return INT64_MIN;
    if (flen == 4 && strncasecmp(field, "null", 4) == 0)
        return INT64_MIN;

    char buf[128];
    int blen = flen < 127 ? flen : 127;
    memcpy(buf, field, blen);
    buf[blen] = '\0';

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    const char *fmt = (fmt_idx == DT_FMT_HINT) ? hint_date_fmt
                                                 : _dt_date_formats[fmt_idx];
    const char *rest = strptime(buf, fmt, &tm_val);
    if (!rest) return INT64_MIN;

    time_t secs = timegm(&tm_val);
    int64_t frac_micros = 0;

    /* Parse fractional seconds (.NNNNNN) */
    if (*rest == '.') {
        rest++;
        int64_t mult = 100000LL; /* 10^5 for 6-digit microseconds */
        while (*rest >= '0' && *rest <= '9' && mult > 0) {
            frac_micros += (*rest - '0') * mult;
            mult /= 10;
            rest++;
        }
    }

    return (int64_t)secs * MICROS_PER_SEC_CSV + frac_micros;
}

/* ================================================================
 * Phase 2 (new): Single-pass reader for ALL column types
 * ================================================================ */

int dt_csv_read_all(const char *filename, char delim,
                    int ncols, int nrows,
                    const int *col_types,
                    const int *col_out_indices,
                    int n_num_cols,  double *num_data,
                    int n_str_cols,  int *str_codes,
                    char *str_unique_bufs,  int str_buf_size_each,
                    int *str_unique_offsets, int str_max_unique_each,
                    int *str_n_uniques,
                    int n_stamp_cols, int64_t *stamp_data,
                    const int *stamp_fmt_indices,
                    const char *hint_date_fmt) {

    _dt_io io;
    if (_dt_io_open(&io, filename) < 0) return -1;

    ssize_t len;
    char *line;

    /* Skip header line */
    line = _dt_io_next_line(&io, &len);
    if (!line) { _dt_io_close(&io); return -1; }

    /* Allocate per-string-column hash table contexts */
    _dt_str_ctx *str_ctxs = NULL;
    if (n_str_cols > 0) {
        str_ctxs = malloc(n_str_cols * sizeof(_dt_str_ctx));
        if (!str_ctxs) { _dt_io_close(&io); return -1; }
        for (int s = 0; s < n_str_cols; s++) {
            int rc = _dt_str_ctx_init(&str_ctxs[s],
                                       str_unique_bufs + s * str_buf_size_each,
                                       str_buf_size_each,
                                       str_unique_offsets + s * str_max_unique_each,
                                       str_max_unique_each);
            if (rc < 0) {
                for (int k = 0; k < s; k++) _dt_str_ctx_free(&str_ctxs[k]);
                free(str_ctxs);
                _dt_io_close(&io);
                return -1;
            }
        }
    }

    /* Allocate field arrays for splitting */
    int *fs = malloc(ncols * sizeof(int));
    int *fl = malloc(ncols * sizeof(int));
    if (!fs || !fl) {
        free(fs); free(fl);
        if (str_ctxs) {
            for (int s = 0; s < n_str_cols; s++) _dt_str_ctx_free(&str_ctxs[s]);
            free(str_ctxs);
        }
        _dt_io_close(&io);
        return -1;
    }

    /* Line buffer for mutable copy (needed for _dt_split_fields) */
    char *line_copy = NULL;
    size_t line_copy_cap = 0;

    int row = 0;
    while (row < nrows && (line = _dt_io_next_line(&io, &len)) != NULL) {
        /* Strip trailing newline/CR */
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        /* Make a mutable copy for field splitting */
        if ((size_t)slen + 1 > line_copy_cap) {
            line_copy_cap = (size_t)slen * 2 + 256;
            free(line_copy);
            line_copy = malloc(line_copy_cap);
            if (!line_copy) break;
        }
        memcpy(line_copy, line, (size_t)slen);
        line_copy[slen] = '\0';

        int nf = _dt_split_fields(line_copy, slen, delim, fs, fl, ncols);

        for (int c = 0; c < nf && c < ncols; c++) {
            int oi = col_out_indices[c];
            int ct = col_types[c];
            char *fld = line_copy + fs[c];
            int flen = fl[c];

            if (ct == DT_COL_INT || ct == DT_COL_DOUBLE) {
                /* Numeric: parse as double */
                if (flen == 0) {
                    num_data[row * n_num_cols + oi] = NAN;
                } else {
                    char buf[64];
                    int blen = flen < 63 ? flen : 63;
                    memcpy(buf, fld, blen);
                    buf[blen] = '\0';
                    char *end;
                    double val = strtod(buf, &end);
                    if (end == buf) val = NAN;
                    num_data[row * n_num_cols + oi] = val;
                }
            } else if (ct == DT_COL_STRING) {
                /* String: dictionary-encode via hash table */
                int code = _dt_str_ctx_intern(&str_ctxs[oi], fld, flen);
                str_codes[row * n_str_cols + oi] = (code >= 0) ? code : 0;
            } else if (ct == DT_COL_STAMP) {
                /* Timestamp: parse to microseconds */
                stamp_data[row * n_stamp_cols + oi] =
                    _dt_parse_stamp_field(fld, flen, stamp_fmt_indices[oi],
                                          hint_date_fmt);
            }
        }

        /* Fill missing columns with defaults if line was short */
        for (int c = nf; c < ncols; c++) {
            int oi = col_out_indices[c];
            int ct = col_types[c];
            if (ct == DT_COL_INT || ct == DT_COL_DOUBLE) {
                num_data[row * n_num_cols + oi] = NAN;
            } else if (ct == DT_COL_STRING) {
                /* Intern empty string */
                int code = _dt_str_ctx_intern(&str_ctxs[oi], "", 0);
                str_codes[row * n_str_cols + oi] = (code >= 0) ? code : 0;
            } else if (ct == DT_COL_STAMP) {
                stamp_data[row * n_stamp_cols + oi] = INT64_MIN;
            }
        }

        row++;
    }

    /* Write out unique counts for string columns */
    if (str_ctxs) {
        for (int s = 0; s < n_str_cols; s++) {
            str_n_uniques[s] = str_ctxs[s].next_code;
            _dt_str_ctx_free(&str_ctxs[s]);
        }
        free(str_ctxs);
    }

    free(line_copy);
    free(fs);
    free(fl);
    _dt_io_close(&io);
    return (row >= nrows) ? 0 : -1;
}
