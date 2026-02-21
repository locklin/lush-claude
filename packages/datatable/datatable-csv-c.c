/* datatable-csv-c.c -- C-accelerated CSV parser for Lush DataTable
 *
 * Fast CSV/TSV reading with per-column type inference.
 * Uses getline() for line reading, strtod()/strtol() for number parsing.
 * Supports comma and tab delimiters (auto-detected from header).
 * Handles quoted fields, CR/LF line endings.
 */

#include "datatable-csv-c.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* ================================================================
 * Internal: File open/close (supports .gz via pipe)
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
 * Phase 1: Scan CSV file
 * ================================================================ */

int dt_csv_scan(const char *filename,
                int *out_nrows, int *out_ncols,
                char *out_delim,
                int *col_types, int max_cols,
                char *name_buf, int name_buf_size,
                int *name_offsets) {
    int is_pipe = 0;
    FILE *fp = _dt_open(filename, &is_pipe);
    if (!fp) return -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    /* Read header line */
    len = getline(&line, &cap, fp);
    if (len < 0) { free(line); _dt_close(fp, is_pipe); return -1; }

    /* Detect delimiter: count tabs vs commas in header */
    int n_tabs = 0, n_commas = 0;
    for (ssize_t i = 0; i < len; i++) {
        if (line[i] == '\t') n_tabs++;
        else if (line[i] == ',') n_commas++;
    }
    char delim = (n_tabs >= n_commas && n_tabs > 0) ? '\t' : ',';
    *out_delim = delim;

    /* Parse header fields (heap-allocated to avoid stack overflow) */
    int lim = (max_cols < DT_MAX_COLS) ? max_cols : DT_MAX_COLS;
    int *field_starts = malloc(lim * sizeof(int));
    int *field_lens   = malloc(lim * sizeof(int));
    if (!field_starts || !field_lens) {
        free(field_starts); free(field_lens);
        free(line); _dt_close(fp, is_pipe); return -1;
    }
    int ncols = _dt_split_fields(line, len, delim,
                                  field_starts, field_lens, lim);
    *out_ncols = ncols;

    /* Copy column names */
    int buf_pos = 0;
    for (int i = 0; i < ncols; i++) {
        name_offsets[i] = buf_pos;
        int flen = field_lens[i];
        if (buf_pos + flen + 1 > name_buf_size) flen = name_buf_size - buf_pos - 1;
        if (flen < 0) flen = 0;
        memcpy(name_buf + buf_pos, line + field_starts[i], flen);
        name_buf[buf_pos + flen] = '\0';
        buf_pos += flen + 1;
    }

    free(field_starts);
    free(field_lens);

    /* Initialize type tracking */
    if (ncols <= 0) { free(line); _dt_close(fp, is_pipe); return -1; }
    int *type_seen = calloc((size_t)ncols, sizeof(int)); /* 0=no data, 1=has data */
    for (int i = 0; i < ncols; i++) col_types[i] = DT_COL_INT; /* start optimistic */

    /* Allocate field arrays for data row parsing */
    int *fs = malloc(ncols * sizeof(int));
    int *fl = malloc(ncols * sizeof(int));
    if (!fs || !fl) {
        free(fs); free(fl); free(type_seen);
        free(line); _dt_close(fp, is_pipe); return -1;
    }

    /* Read data rows, detect types from first N rows, count all rows */
    int nrows = 0;
    int sample_rows = 0;

    while ((len = getline(&line, &cap, fp)) > 0) {
        /* Skip blank lines */
        ssize_t slen = len;
        while (slen > 0 && (line[slen-1] == '\n' || line[slen-1] == '\r'))
            slen--;
        if (slen == 0) continue;

        nrows++;

        /* Type detection from sample rows */
        if (sample_rows < DT_TYPE_SAMPLE_ROWS) {
            int nf = _dt_split_fields(line, len, delim, fs, fl, ncols);
            for (int j = 0; j < nf && j < ncols; j++) {
                int ft = _dt_detect_field_type(line + fs[j], fl[j]);
                if (ft < 0) continue; /* missing/empty, skip */
                type_seen[j] = 1;
                /* Type promotion: int -> double -> string */
                if (ft > col_types[j])
                    col_types[j] = ft;
            }
            sample_rows++;
        }
    }

    /* Columns with no data seen: default to string */
    for (int i = 0; i < ncols; i++) {
        if (!type_seen[i])
            col_types[i] = DT_COL_STRING;
    }

    free(fs);
    free(fl);

    free(type_seen);
    free(line);
    _dt_close(fp, is_pipe);

    *out_nrows = nrows;
    return 0;
}

/* ================================================================
 * Phase 2: Read numeric columns
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
 * Phase 2b: Read string column with dictionary encoding
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
