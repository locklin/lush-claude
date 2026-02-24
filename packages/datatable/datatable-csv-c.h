/* datatable-csv-c.h -- C-accelerated CSV parser for Lush DataTable
 *
 * Two-phase CSV reading:
 *   Phase 1: Scan file to determine dimensions, delimiter, column types
 *   Phase 2: Read all columns in a single pass (numeric, string, stamp)
 *
 * Type detection: sample first N rows per column.
 *   - If all non-empty values parse as integer -> DT_COL_INT
 *   - Else if all parse as double -> DT_COL_DOUBLE
 *   - Else -> DT_COL_STRING
 *
 * I/O: mmap for regular files, getline fallback for .gz pipes.
 */

#ifndef DATATABLE_CSV_C_H
#define DATATABLE_CSV_C_H

#include <stdlib.h>
#include <stdint.h>

/* Column type codes */
#define DT_COL_INT     0
#define DT_COL_DOUBLE  1
#define DT_COL_STRING  2
#define DT_COL_STAMP   3

/* Maximum columns supported */
#define DT_MAX_COLS 65536

/* Maximum rows to sample for type detection */
#define DT_TYPE_SAMPLE_ROWS 200

/* Microseconds per second (stamp resolution) */
#define MICROS_PER_SEC_CSV 1000000LL

/* date_fmt_indices value meaning "use hint string" */
#define DT_FMT_HINT (-2)

/* ================================================================
 * Phase 1: Scan CSV file
 * ================================================================
 *
 * Reads the file to determine:
 *   - Number of data rows (excluding header)
 *   - Number of columns
 *   - Delimiter character (tab or comma)
 *   - Column types (int, double, or string)
 *   - Column names from header
 *
 * col_types must point to an array of at least max_cols ints.
 * name_buf is filled with null-terminated column names, concatenated.
 * name_offsets[i] is the offset into name_buf where column i's name starts.
 *
 * Returns 0 on success, -1 on error.
 */
int dt_csv_scan(const char *filename,
                int *out_nrows, int *out_ncols,
                char *out_delim,
                int *col_types, int max_cols,
                char *name_buf, int name_buf_size,
                int *name_offsets,
                int *date_fmt_indices,
                char hint_delim,
                const char *hint_date_fmt);

/* ================================================================
 * Phase 2: Read numeric columns (legacy, kept for compatibility)
 * ================================================================
 *
 * Reads selected columns from the CSV into a pre-allocated double matrix.
 * col_indices[j] = original column index for the j-th output column.
 * data layout: data[row * n_cols + j] for row in [0,nrows), j in [0,n_cols).
 *
 * Returns 0 on success, -1 on error.
 */
int dt_csv_read_numeric(const char *filename,
                        char delim,
                        const int *col_indices, int n_cols,
                        double *data, int nrows);

/* ================================================================
 * Phase 2b: Read a string column with dictionary encoding (legacy)
 * ================================================================
 *
 * Reads one column from the CSV, building a dictionary of unique values.
 * codes[row] = integer code for that row's value.
 * unique_buf is filled with null-terminated unique strings, concatenated.
 * unique_offsets[i] = offset into unique_buf where unique string i starts.
 * *out_n_unique = number of unique strings found.
 *
 * Returns 0 on success, -1 on error.
 */
int dt_csv_read_string_col(const char *filename,
                           char delim, int col_idx,
                           int *codes, int nrows,
                           char *unique_buf, int unique_buf_size,
                           int *unique_offsets, int max_unique,
                           int *out_n_unique);

/* ================================================================
 * Phase 2c: Read a timestamp column (legacy)
 * ================================================================
 *
 * Reads one column from the CSV, parsing each field as a timestamp
 * using the given strptime format string.  Handles trailing fractional
 * seconds after the decimal point.
 * stamps[row] = int64 nanoseconds since epoch, or INT64_MIN on failure.
 *
 * Returns 0 on success, -1 on error.
 */
int dt_csv_read_stamp_col(const char *filename, char delim,
                          int col_idx, int64_t *stamps, int nrows,
                          const char *fmt);

/* ================================================================
 * Phase 2 (new): Read ALL columns in a single pass
 * ================================================================
 *
 * Reads numeric, string, and stamp columns in one pass through the file.
 * Uses mmap for regular files, getline for .gz pipes.
 *
 * col_types[c]:       DT_COL_INT/DOUBLE/STRING/STAMP for each CSV column
 * col_out_indices[c]: output index within the column's type group
 *
 * Numeric output:  num_data[row * n_num_cols + col_out_indices[c]]
 * String output:   str_codes[row * n_str_cols + col_out_indices[c]]
 *                  Per-string-column dictionary in concatenated buffers
 * Stamp output:    stamp_data[row * n_stamp_cols + col_out_indices[c]]
 *
 * Returns 0 on success, -1 on error.
 */
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
                    const char *hint_date_fmt);

#endif /* DATATABLE_CSV_C_H */
