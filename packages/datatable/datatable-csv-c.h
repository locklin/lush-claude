/* datatable-csv-c.h -- C-accelerated CSV parser for Lush DataTable
 *
 * Two-phase CSV reading:
 *   Phase 1: Scan file to determine dimensions, delimiter, column types
 *   Phase 2: Read numeric data directly into pre-allocated arrays
 *
 * Type detection: sample first N rows per column.
 *   - If all non-empty values parse as integer -> DT_COL_INT
 *   - Else if all parse as double -> DT_COL_DOUBLE
 *   - Else -> DT_COL_STRING
 */

#ifndef DATATABLE_CSV_C_H
#define DATATABLE_CSV_C_H

#include <stdlib.h>

/* Column type codes */
#define DT_COL_INT     0
#define DT_COL_DOUBLE  1
#define DT_COL_STRING  2

/* Maximum columns supported */
#define DT_MAX_COLS 65536

/* Maximum rows to sample for type detection */
#define DT_TYPE_SAMPLE_ROWS 200

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
                int *name_offsets);

/* ================================================================
 * Phase 2: Read numeric columns
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
 * Phase 2b: Read a string column with dictionary encoding
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

#endif /* DATATABLE_CSV_C_H */
