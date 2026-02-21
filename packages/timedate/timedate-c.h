/* timedate-c.h -- Nanosecond-precision UTC timestamps for Lush
 *
 * All timestamps are int64_t nanoseconds since Unix epoch (1970-01-01T00:00:00Z).
 * Uses timegm()/gmtime_r() for UTC conversion (available on Linux/glibc).
 * Parse failure sentinel: INT64_MIN.
 */

#ifndef TIMEDATE_C_H
#define TIMEDATE_C_H

#include <stdint.h>

/* Return current UTC time as nanoseconds since epoch. */
int64_t td_timestamp_now(void);

/* Parse a date/time string with strptime format into nanoseconds.
 * Handles trailing fractional seconds (.123456789) manually.
 * Returns INT64_MIN on parse failure. */
int64_t td_strptime_to_nanos(const char *str, const char *fmt);

/* Format nanoseconds into a string via strftime.
 * Returns 0 on success, -1 on failure. */
int td_nanos_to_strftime(int64_t nanos, const char *fmt, char *buf, int buf_size);

/* Decompose nanoseconds into components:
 * out7[0]=year, [1]=month(1-12), [2]=day(1-31),
 * [3]=hour, [4]=minute, [5]=second, [6]=nano_remainder */
void td_timestamp_components(int64_t nanos, int *out7);

/* Build timestamp from components.
 * Returns nanoseconds since epoch. */
int64_t td_make_timestamp(int y, int m, int d, int H, int M, int S, int nano);

#endif /* TIMEDATE_C_H */
