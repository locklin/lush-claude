/* json-c.h -- Fast JSON parser for Lush (C functions, backed by yyjson) */

#ifndef LUSH_JSON_C_H
#define LUSH_JSON_C_H

#include <stdint.h>

/* Side codes for order book */
#define LUSH_JSON_SIDE_BUY  0
#define LUSH_JSON_SIDE_SELL 1

/* Message type codes */
#define LUSH_JSON_MSG_SUBSCRIPTIONS 0
#define LUSH_JSON_MSG_L2UPDATE      1
#define LUSH_JSON_MSG_TICKER        2
#define LUSH_JSON_MSG_SNAPSHOT      3
#define LUSH_JSON_MSG_HEARTBEAT     4
#define LUSH_JSON_MSG_ERROR         5
#define LUSH_JSON_MSG_UNKNOWN      -1

/* General parser type tags */
#define LUSH_JSON_TAG_NULL        0
#define LUSH_JSON_TAG_BOOL        1
#define LUSH_JSON_TAG_INT         2
#define LUSH_JSON_TAG_REAL        3
#define LUSH_JSON_TAG_STRING      4
#define LUSH_JSON_TAG_ARRAY_START 5
#define LUSH_JSON_TAG_ARRAY_END   6
#define LUSH_JSON_TAG_OBJ_START   7
#define LUSH_JSON_TAG_OBJ_END     8
#define LUSH_JSON_TAG_KEY         9

/* ----------------------------------------------------------------
 * Tier 1: Message type detector (fast dispatch without full parse)
 * ---------------------------------------------------------------- */

/* Returns message type code: 0=subscriptions, 1=l2update, 2=ticker,
 * 3=snapshot, 4=heartbeat, 5=error, -1=unknown.
 */
int lush_json_msg_type(const char *json, int len);

/* ----------------------------------------------------------------
 * Tier 2: Schema-specific extractors (hot path, zero-malloc)
 * ---------------------------------------------------------------- */

/* Parse a Coinbase l2update message into parallel column arrays.
 * Writes at offset position in each array.
 * product_table / product_offsets / n_products: string lookup table.
 * Returns number of rows written (number of changes), or -1 on error.
 */
int lush_json_parse_l2update(
    const char *json, int len,
    int *side_codes, double *prices, double *quantities,
    long long *timestamps, int *product_codes,
    int offset,
    const unsigned char *product_table, const int *product_offsets,
    int n_products);

/* Parse a Coinbase ticker message into column arrays.
 * Writes a single row at the given offset.
 * volumes receives last_size (volume of last trade).
 * Returns 1 on success, -1 on error.
 */
int lush_json_parse_ticker(
    const char *json, int len,
    double *prices, double *best_bids, double *best_asks,
    int *side_codes, long long *timestamps, int *product_codes,
    double *volumes,
    int offset,
    const unsigned char *product_table, const int *product_offsets,
    int n_products);

/* ----------------------------------------------------------------
 * Tier 3: General-purpose parser
 * ----------------------------------------------------------------
 * Recursive DOM walk emitting type-tagged entries into pre-allocated
 * arrays.  The Lush layer reconstructs nested alist/list structures.
 *
 * type_tags[i]: one of LUSH_JSON_TAG_*
 * num_vals[i]:  numeric value (int64/double union, interpret by tag)
 * str_buf:      concatenated strings (null-terminated)
 * str_offsets[i]: offset into str_buf for string/key at entry i
 *
 * Returns number of entries written, or -1 on error.
 */
int lush_json_parse(
    const char *json, int len,
    int *type_tags, double *num_vals,
    unsigned char *str_buf, int *str_offsets,
    int max_entries, int str_buf_size);

#endif /* LUSH_JSON_C_H */
