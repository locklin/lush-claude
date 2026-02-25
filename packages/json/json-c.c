/* json-c.c -- Fast JSON parser for Lush (yyjson-backed implementation) */

#include "json-c.h"
#include "yyjson.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ================================================================
 * Internal: hand-rolled ISO 8601 timestamp parser
 * Parses "YYYY-MM-DDThh:mm:ss.fracZ" -> int64 microseconds since epoch.
 * No strptime overhead; parses digits directly.
 * ================================================================ */

static int _parse_2digits(const char *p) {
    return (p[0] - '0') * 10 + (p[1] - '0');
}

static int _parse_4digits(const char *p) {
    return (p[0] - '0') * 1000 + (p[1] - '0') * 100 +
           (p[2] - '0') * 10 + (p[3] - '0');
}

/* Returns microseconds since epoch, or INT64_MIN on failure. */
static long long _parse_coinbase_timestamp(const char *s) {
    /* Expected: "2024-01-15T10:30:00.123456Z" (minimum "YYYY-MM-DDThh:mm:ss") */
    if (!s || strlen(s) < 19) return (-9223372036854775807LL - 1LL);

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(struct tm));

    tm_val.tm_year = _parse_4digits(s) - 1900;   /* YYYY */
    tm_val.tm_mon  = _parse_2digits(s + 5) - 1;  /* MM */
    tm_val.tm_mday = _parse_2digits(s + 8);       /* DD */
    tm_val.tm_hour = _parse_2digits(s + 11);      /* hh */
    tm_val.tm_min  = _parse_2digits(s + 14);      /* mm */
    tm_val.tm_sec  = _parse_2digits(s + 17);      /* ss */

    time_t secs = timegm(&tm_val);
    long long frac_micros = 0;

    /* Handle fractional seconds after position 19 */
    const char *rest = s + 19;
    if (*rest == '.') {
        rest++;
        long long mult = 100000LL;  /* 10^5: first frac digit = 0.1s = 100000us */
        while (*rest >= '0' && *rest <= '9' && mult > 0) {
            frac_micros += (*rest - '0') * mult;
            mult /= 10;
            rest++;
        }
    }

    return (long long)secs * 1000000LL + frac_micros;
}


/* ================================================================
 * Internal: product_id -> integer code lookup
 * Linear scan through packed string table (small, typically 1-20).
 * Returns code (0-based index) or -1 if not found.
 * ================================================================ */

static int _lookup_product(const char *product_id,
                           const unsigned char *product_table,
                           const int *product_offsets,
                           int n_products) {
    if (!product_id || !product_table || !product_offsets || n_products <= 0)
        return -1;
    for (int i = 0; i < n_products; i++) {
        const char *entry = (const char *)(product_table + product_offsets[i]);
        if (strcmp(product_id, entry) == 0)
            return i;
    }
    return -1;
}


/* ================================================================
 * Tier 1: Message type detector
 * Fast substring search for "type":"..." pattern.
 * ================================================================ */

int lush_json_msg_type(const char *json, int len) {
    /* Search for "type":" pattern */
    const char *pattern = "\"type\":\"";
    int plen = 8;
    const char *p = json;
    const char *end = json + len - plen;

    while (p <= end) {
        if (memcmp(p, pattern, plen) == 0) {
            const char *val = p + plen;
            /* Check first character for fast dispatch */
            switch (val[0]) {
            case 's':
                if (val[1] == 'u') return LUSH_JSON_MSG_SUBSCRIPTIONS;  /* subscriptions */
                if (val[1] == 'n') return LUSH_JSON_MSG_SNAPSHOT;       /* snapshot */
                break;
            case 'l':
                if (val[1] == '2') return LUSH_JSON_MSG_L2UPDATE;       /* l2update */
                break;
            case 't':
                if (val[1] == 'i') return LUSH_JSON_MSG_TICKER;         /* ticker */
                break;
            case 'h':
                return LUSH_JSON_MSG_HEARTBEAT;                         /* heartbeat */
            case 'e':
                return LUSH_JSON_MSG_ERROR;                             /* error */
            }
            return LUSH_JSON_MSG_UNKNOWN;
        }
        p++;
    }
    return LUSH_JSON_MSG_UNKNOWN;
}


/* ================================================================
 * Tier 2: L2 update extractor
 * ================================================================ */

int lush_json_parse_l2update(
    const char *json, int len,
    int *side_codes, double *prices, double *quantities,
    long long *timestamps, int *product_codes,
    int offset,
    const unsigned char *product_table, const int *product_offsets,
    int n_products)
{
    char pool_buf[4096];
    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, pool_buf, sizeof(pool_buf));

    yyjson_doc *doc = yyjson_read_opts((char *)json, (size_t)len, 0, &alc, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) return -1;

    /* Extract timestamp */
    yyjson_val *time_val = yyjson_obj_get(root, "time");
    long long ts = (-9223372036854775807LL - 1LL);
    if (time_val && yyjson_is_str(time_val)) {
        ts = _parse_coinbase_timestamp(yyjson_get_str(time_val));
    }

    /* Extract product_id -> integer code */
    yyjson_val *pid_val = yyjson_obj_get(root, "product_id");
    int pcode = -1;
    if (pid_val && yyjson_is_str(pid_val)) {
        pcode = _lookup_product(yyjson_get_str(pid_val),
                                product_table, product_offsets, n_products);
    }

    /* Extract changes array */
    yyjson_val *changes = yyjson_obj_get(root, "changes");
    if (!changes || !yyjson_is_arr(changes)) return -1;

    int count = 0;
    size_t idx, max;
    yyjson_val *change;
    yyjson_arr_foreach(changes, idx, max, change) {
        if (!yyjson_is_arr(change) || yyjson_arr_size(change) < 3)
            continue;

        yyjson_val *side_val = yyjson_arr_get(change, 0);
        yyjson_val *price_val = yyjson_arr_get(change, 1);
        yyjson_val *qty_val = yyjson_arr_get(change, 2);

        int pos = offset + count;

        /* Side: check first char 'b' for buy, else sell */
        const char *side_str = yyjson_get_str(side_val);
        side_codes[pos] = (side_str && side_str[0] == 'b')
                          ? LUSH_JSON_SIDE_BUY : LUSH_JSON_SIDE_SELL;

        /* Price and quantity: strtod on yyjson's null-terminated strings */
        const char *price_str = yyjson_get_str(price_val);
        const char *qty_str = yyjson_get_str(qty_val);
        prices[pos] = price_str ? strtod(price_str, NULL) : 0.0;
        quantities[pos] = qty_str ? strtod(qty_str, NULL) : 0.0;

        /* Timestamp and product code: same for all rows */
        timestamps[pos] = ts;
        product_codes[pos] = pcode;

        count++;
    }

    return count;
}


/* ================================================================
 * Tier 2: Ticker extractor
 * ================================================================ */

int lush_json_parse_ticker(
    const char *json, int len,
    double *prices, double *best_bids, double *best_asks,
    int *side_codes, long long *timestamps, int *product_codes,
    int offset,
    const unsigned char *product_table, const int *product_offsets,
    int n_products)
{
    char pool_buf[4096];
    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, pool_buf, sizeof(pool_buf));

    yyjson_doc *doc = yyjson_read_opts((char *)json, (size_t)len, 0, &alc, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) return -1;

    /* Timestamp */
    yyjson_val *time_val = yyjson_obj_get(root, "time");
    long long ts = (-9223372036854775807LL - 1LL);
    if (time_val && yyjson_is_str(time_val)) {
        ts = _parse_coinbase_timestamp(yyjson_get_str(time_val));
    }

    /* Product code */
    yyjson_val *pid_val = yyjson_obj_get(root, "product_id");
    int pcode = -1;
    if (pid_val && yyjson_is_str(pid_val)) {
        pcode = _lookup_product(yyjson_get_str(pid_val),
                                product_table, product_offsets, n_products);
    }

    /* Price */
    yyjson_val *price_val = yyjson_obj_get(root, "price");
    double price = 0.0;
    if (price_val && yyjson_is_str(price_val))
        price = strtod(yyjson_get_str(price_val), NULL);
    else if (price_val && yyjson_is_num(price_val))
        price = yyjson_get_num(price_val);

    /* Best bid */
    yyjson_val *bid_val = yyjson_obj_get(root, "best_bid");
    double best_bid = 0.0;
    if (bid_val && yyjson_is_str(bid_val))
        best_bid = strtod(yyjson_get_str(bid_val), NULL);
    else if (bid_val && yyjson_is_num(bid_val))
        best_bid = yyjson_get_num(bid_val);

    /* Best ask */
    yyjson_val *ask_val = yyjson_obj_get(root, "best_ask");
    double best_ask = 0.0;
    if (ask_val && yyjson_is_str(ask_val))
        best_ask = strtod(yyjson_get_str(ask_val), NULL);
    else if (ask_val && yyjson_is_num(ask_val))
        best_ask = yyjson_get_num(ask_val);

    /* Side */
    yyjson_val *side_val = yyjson_obj_get(root, "side");
    int side = LUSH_JSON_SIDE_BUY;
    if (side_val && yyjson_is_str(side_val)) {
        const char *s = yyjson_get_str(side_val);
        side = (s && s[0] == 'b') ? LUSH_JSON_SIDE_BUY : LUSH_JSON_SIDE_SELL;
    }

    /* Write to arrays */
    prices[offset] = price;
    best_bids[offset] = best_bid;
    best_asks[offset] = best_ask;
    side_codes[offset] = side;
    timestamps[offset] = ts;
    product_codes[offset] = pcode;

    return 1;
}


/* ================================================================
 * Tier 3: General-purpose parser
 * Recursive DOM walk emitting type-tagged entries.
 * ================================================================ */

typedef struct {
    int *type_tags;
    double *num_vals;
    unsigned char *str_buf;
    int *str_offsets;
    int max_entries;
    int str_buf_size;
    int pos;          /* current entry index */
    int str_pos;      /* current position in str_buf */
    int error;        /* set to 1 if overflow */
} _parse_ctx;

static void _emit_string(_parse_ctx *ctx, int tag, const char *s, size_t slen) {
    if (ctx->pos >= ctx->max_entries || ctx->error) {
        ctx->error = 1;
        return;
    }
    /* Check string buffer space (need slen + 1 for null terminator) */
    if (ctx->str_pos + (int)slen + 1 > ctx->str_buf_size) {
        ctx->error = 1;
        return;
    }
    ctx->type_tags[ctx->pos] = tag;
    ctx->num_vals[ctx->pos] = 0.0;
    ctx->str_offsets[ctx->pos] = ctx->str_pos;
    memcpy(ctx->str_buf + ctx->str_pos, s, slen);
    ctx->str_buf[ctx->str_pos + slen] = '\0';
    ctx->str_pos += (int)slen + 1;
    ctx->pos++;
}

static void _emit_num(_parse_ctx *ctx, int tag, double val) {
    if (ctx->pos >= ctx->max_entries || ctx->error) {
        ctx->error = 1;
        return;
    }
    ctx->type_tags[ctx->pos] = tag;
    ctx->num_vals[ctx->pos] = val;
    ctx->str_offsets[ctx->pos] = -1;
    ctx->pos++;
}

static void _emit_marker(_parse_ctx *ctx, int tag) {
    if (ctx->pos >= ctx->max_entries || ctx->error) {
        ctx->error = 1;
        return;
    }
    ctx->type_tags[ctx->pos] = tag;
    ctx->num_vals[ctx->pos] = 0.0;
    ctx->str_offsets[ctx->pos] = -1;
    ctx->pos++;
}

static void _walk_val(_parse_ctx *ctx, yyjson_val *val);

static void _walk_val(_parse_ctx *ctx, yyjson_val *val) {
    if (!val || ctx->error) return;

    if (yyjson_is_null(val)) {
        _emit_marker(ctx, LUSH_JSON_TAG_NULL);
    } else if (yyjson_is_bool(val)) {
        _emit_num(ctx, LUSH_JSON_TAG_BOOL, yyjson_get_bool(val) ? 1.0 : 0.0);
    } else if (yyjson_is_int(val)) {
        _emit_num(ctx, LUSH_JSON_TAG_INT, (double)yyjson_get_sint(val));
    } else if (yyjson_is_real(val)) {
        _emit_num(ctx, LUSH_JSON_TAG_REAL, yyjson_get_real(val));
    } else if (yyjson_is_str(val)) {
        const char *s = yyjson_get_str(val);
        size_t slen = yyjson_get_len(val);
        _emit_string(ctx, LUSH_JSON_TAG_STRING, s, slen);
    } else if (yyjson_is_arr(val)) {
        _emit_marker(ctx, LUSH_JSON_TAG_ARRAY_START);
        size_t idx, max;
        yyjson_val *child;
        yyjson_arr_foreach(val, idx, max, child) {
            _walk_val(ctx, child);
        }
        _emit_marker(ctx, LUSH_JSON_TAG_ARRAY_END);
    } else if (yyjson_is_obj(val)) {
        _emit_marker(ctx, LUSH_JSON_TAG_OBJ_START);
        size_t idx, max;
        yyjson_val *key, *child;
        yyjson_obj_foreach(val, idx, max, key, child) {
            const char *ks = yyjson_get_str(key);
            size_t klen = yyjson_get_len(key);
            _emit_string(ctx, LUSH_JSON_TAG_KEY, ks, klen);
            _walk_val(ctx, child);
        }
        _emit_marker(ctx, LUSH_JSON_TAG_OBJ_END);
    }
}

int lush_json_parse(
    const char *json, int len,
    int *type_tags, double *num_vals,
    unsigned char *str_buf, int *str_offsets,
    int max_entries, int str_buf_size)
{
    char pool_buf[4096];
    yyjson_alc alc;
    yyjson_alc_pool_init(&alc, pool_buf, sizeof(pool_buf));

    yyjson_doc *doc = yyjson_read_opts((char *)json, (size_t)len, 0, &alc, NULL);
    if (!doc) return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) return -1;

    _parse_ctx ctx;
    ctx.type_tags = type_tags;
    ctx.num_vals = num_vals;
    ctx.str_buf = str_buf;
    ctx.str_offsets = str_offsets;
    ctx.max_entries = max_entries;
    ctx.str_buf_size = str_buf_size;
    ctx.pos = 0;
    ctx.str_pos = 0;
    ctx.error = 0;

    _walk_val(&ctx, root);

    if (ctx.error) return -1;
    return ctx.pos;
}
