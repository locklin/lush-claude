/* timedate-c.c -- Nanosecond-precision UTC timestamps for Lush
 *
 * Implements int64 nanosecond timestamps using POSIX time functions.
 * All conversions are UTC (timegm/gmtime_r).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "timedate-c.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>

#define NANOS_PER_SEC  1000000000LL
#define TD_PARSE_FAIL  INT64_MIN

/* ================================================================
 * td_timestamp_now -- current UTC time as nanoseconds
 * ================================================================ */

int64_t td_timestamp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * NANOS_PER_SEC + (int64_t)ts.tv_nsec;
}

/* ================================================================
 * td_strptime_to_nanos -- parse date/time string to nanoseconds
 * ================================================================
 *
 * Uses strptime for the main format, then manually parses any
 * trailing fractional seconds after a '.' character.
 */

int64_t td_strptime_to_nanos(const char *str, const char *fmt) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    const char *rest = strptime(str, fmt, &tm);
    if (!rest) return TD_PARSE_FAIL;

    /* Convert struct tm to epoch seconds (UTC) */
    time_t secs = timegm(&tm);
    if (secs == (time_t)-1) {
        /* timegm returns -1 on error, but also for 1969-12-31T23:59:59 UTC.
         * Accept the value if the year is reasonable. */
        if (tm.tm_year + 1900 < 1900 || tm.tm_year + 1900 > 2200)
            return TD_PARSE_FAIL;
    }

    /* Parse fractional seconds if present */
    int64_t frac_nanos = 0;
    if (*rest == '.') {
        rest++;
        int64_t mult = 100000000LL; /* 10^8 */
        while (*rest >= '0' && *rest <= '9' && mult > 0) {
            frac_nanos += (*rest - '0') * mult;
            mult /= 10;
            rest++;
        }
    }

    /* Skip trailing timezone chars (Z, +00:00 etc) -- accept them */
    /* We already parsed via UTC, so timezone suffix is informational only */

    return (int64_t)secs * NANOS_PER_SEC + frac_nanos;
}

/* ================================================================
 * td_nanos_to_strftime -- format nanoseconds to string
 * ================================================================ */

int td_nanos_to_strftime(int64_t nanos, const char *fmt, char *buf, int buf_size) {
    time_t secs = (time_t)(nanos / NANOS_PER_SEC);
    /* Handle negative nanoseconds (pre-epoch) */
    if (nanos < 0 && (nanos % NANOS_PER_SEC) != 0)
        secs--;

    struct tm tm;
    if (!gmtime_r(&secs, &tm)) {
        if (buf_size > 0) buf[0] = '\0';
        return -1;
    }

    size_t rc = strftime(buf, (size_t)buf_size, fmt, &tm);
    if (rc == 0) {
        if (buf_size > 0) buf[0] = '\0';
        return -1;
    }
    return 0;
}

/* ================================================================
 * td_timestamp_components -- decompose nanoseconds to parts
 * ================================================================ */

void td_timestamp_components(int64_t nanos, int *out7) {
    time_t secs = (time_t)(nanos / NANOS_PER_SEC);
    int64_t remainder = nanos % NANOS_PER_SEC;
    if (nanos < 0 && remainder != 0) {
        secs--;
        remainder += NANOS_PER_SEC;
    }

    struct tm tm;
    gmtime_r(&secs, &tm);

    out7[0] = tm.tm_year + 1900;
    out7[1] = tm.tm_mon + 1;
    out7[2] = tm.tm_mday;
    out7[3] = tm.tm_hour;
    out7[4] = tm.tm_min;
    out7[5] = tm.tm_sec;
    out7[6] = (int)remainder;
}

/* ================================================================
 * td_make_timestamp -- build nanoseconds from components
 * ================================================================ */

int64_t td_make_timestamp(int y, int m, int d, int H, int M, int S, int nano) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = H;
    tm.tm_min  = M;
    tm.tm_sec  = S;

    time_t secs = timegm(&tm);
    return (int64_t)secs * NANOS_PER_SEC + (int64_t)nano;
}
