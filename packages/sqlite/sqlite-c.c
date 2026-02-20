/* sqlite-c.c -- Thin C wrappers around SQLite3 for Lush FFI
 *
 * Each function does one thing.  Pointers are passed as void* so that
 * Lush can treat them as gptr values without needing the sqlite3 typedefs.
 */

#include "sqlite3.h"
#include "sqlite-c.h"
#include <stdlib.h>
#include <string.h>

/* ---- connection ---- */

void *lush_sqlite3_open(const char *path)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        /* db may still be allocated even on error */
        if (db) sqlite3_close(db);
        return NULL;
    }
    return (void *)db;
}

int lush_sqlite3_close(void *db)
{
    if (!db) return SQLITE_OK;
    return sqlite3_close((sqlite3 *)db);
}

const char *lush_sqlite3_errmsg(void *db)
{
    if (!db) return "null database handle";
    return sqlite3_errmsg((sqlite3 *)db);
}

int lush_sqlite3_busy_timeout(void *db, int ms)
{
    if (!db) return SQLITE_MISUSE;
    return sqlite3_busy_timeout((sqlite3 *)db, ms);
}

/* ---- statements ---- */

void *lush_sqlite3_prepare(void *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!db) return NULL;
    rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return NULL;
    }
    return (void *)stmt;
}

int lush_sqlite3_step(void *stmt)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_step((sqlite3_stmt *)stmt);
}

int lush_sqlite3_finalize(void *stmt)
{
    if (!stmt) return SQLITE_OK;
    return sqlite3_finalize((sqlite3_stmt *)stmt);
}

int lush_sqlite3_reset(void *stmt)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_reset((sqlite3_stmt *)stmt);
}

/* ---- binding ---- */

int lush_sqlite3_bind_int(void *stmt, int idx, int val)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_int((sqlite3_stmt *)stmt, idx, val);
}

int lush_sqlite3_bind_double(void *stmt, int idx, double val)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_double((sqlite3_stmt *)stmt, idx, val);
}

int lush_sqlite3_bind_text(void *stmt, int idx, const char *val)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_text((sqlite3_stmt *)stmt, idx, val, -1,
                             SQLITE_TRANSIENT);
}

int lush_sqlite3_bind_null(void *stmt, int idx)
{
    if (!stmt) return SQLITE_MISUSE;
    return sqlite3_bind_null((sqlite3_stmt *)stmt, idx);
}

/* ---- column access ---- */

int lush_sqlite3_column_count(void *stmt)
{
    if (!stmt) return 0;
    return sqlite3_column_count((sqlite3_stmt *)stmt);
}

int lush_sqlite3_column_type(void *stmt, int col)
{
    if (!stmt) return 0;
    return sqlite3_column_type((sqlite3_stmt *)stmt, col);
}

int lush_sqlite3_column_int(void *stmt, int col)
{
    if (!stmt) return 0;
    return sqlite3_column_int((sqlite3_stmt *)stmt, col);
}

double lush_sqlite3_column_double(void *stmt, int col)
{
    if (!stmt) return 0.0;
    return sqlite3_column_double((sqlite3_stmt *)stmt, col);
}

const char *lush_sqlite3_column_text(void *stmt, int col)
{
    if (!stmt) return "";
    const char *t = (const char *)sqlite3_column_text((sqlite3_stmt *)stmt, col);
    return t ? t : "";
}

/* ---- misc ---- */

int lush_sqlite3_changes(void *db)
{
    if (!db) return 0;
    return sqlite3_changes((sqlite3 *)db);
}

int lush_sqlite3_last_insert_rowid(void *db)
{
    if (!db) return 0;
    /* sqlite3_last_insert_rowid returns int64, truncate to int for Lush */
    return (int)sqlite3_last_insert_rowid((sqlite3 *)db);
}

int lush_sqlite3_enable_wal(void *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!db) return SQLITE_MISUSE;
    rc = sqlite3_prepare_v2((sqlite3 *)db,
                            "PRAGMA journal_mode=WAL", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    /* step returns SQLITE_ROW for pragma results */
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) ? SQLITE_OK : rc;
}
