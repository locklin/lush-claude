/* sqlite-c.h -- Thin C wrappers around SQLite3 for Lush FFI */

#ifndef LUSH_SQLITE_C_H
#define LUSH_SQLITE_C_H

void *lush_sqlite3_open(const char *path);
int   lush_sqlite3_close(void *db);
const char *lush_sqlite3_errmsg(void *db);
int   lush_sqlite3_busy_timeout(void *db, int ms);

void *lush_sqlite3_prepare(void *db, const char *sql);
int   lush_sqlite3_step(void *stmt);
int   lush_sqlite3_finalize(void *stmt);
int   lush_sqlite3_reset(void *stmt);

int   lush_sqlite3_bind_int(void *stmt, int idx, int val);
int   lush_sqlite3_bind_double(void *stmt, int idx, double val);
int   lush_sqlite3_bind_text(void *stmt, int idx, const char *val);
int   lush_sqlite3_bind_null(void *stmt, int idx);

int         lush_sqlite3_column_count(void *stmt);
int         lush_sqlite3_column_type(void *stmt, int col);
int         lush_sqlite3_column_int(void *stmt, int col);
double      lush_sqlite3_column_double(void *stmt, int col);
const char *lush_sqlite3_column_text(void *stmt, int col);

int   lush_sqlite3_changes(void *db);
int   lush_sqlite3_last_insert_rowid(void *db);
int   lush_sqlite3_enable_wal(void *db);

#endif /* LUSH_SQLITE_C_H */
