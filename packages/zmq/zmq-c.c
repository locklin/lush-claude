/* zmq-c.c -- Thin C wrapper around libzmq for Lush FFI
 *
 * Three mechanisms:
 *   1. Handle table: maps integer IDs to void* (context or socket)
 *   2. ROUTER identity storage: per-socket identity frame buffer
 *   3. Static poll item array for zmq_poll
 *
 * Follows the same pattern as libuv-c.c.
 */

#include "zmq-c.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Handle table
 * ================================================================ */

#define MAX_ZMQ_HANDLES 64

/* Handle types for debugging */
#define HTYPE_NONE    0
#define HTYPE_CTX     1
#define HTYPE_SOCKET  2

static struct {
    void *ptr;
    int   in_use;
    int   htype;      /* HTYPE_CTX or HTYPE_SOCKET */
    int   ctx_id;     /* for sockets: which context owns this */
    /* ROUTER identity storage */
    unsigned char identity[256];
    int identity_len;
} handle_table[MAX_ZMQ_HANDLES];

static int next_handle_scan = 0;

/* Allocate an ID and store the pointer */
static int handle_register(void *p, int htype, int ctx_id)
{
    for (int i = 0; i < MAX_ZMQ_HANDLES; i++) {
        int idx = (next_handle_scan + i) % MAX_ZMQ_HANDLES;
        if (!handle_table[idx].in_use) {
            handle_table[idx].ptr = p;
            handle_table[idx].in_use = 1;
            handle_table[idx].htype = htype;
            handle_table[idx].ctx_id = ctx_id;
            handle_table[idx].identity_len = 0;
            next_handle_scan = (idx + 1) % MAX_ZMQ_HANDLES;
            return idx;
        }
    }
    return -1;  /* table full */
}

static void *handle_lookup(int id)
{
    if (id < 0 || id >= MAX_ZMQ_HANDLES || !handle_table[id].in_use)
        return NULL;
    return handle_table[id].ptr;
}

static void handle_remove(int id)
{
    if (id >= 0 && id < MAX_ZMQ_HANDLES) {
        handle_table[id].ptr = NULL;
        handle_table[id].in_use = 0;
        handle_table[id].htype = HTYPE_NONE;
        handle_table[id].identity_len = 0;
    }
}


/* ================================================================
 * Context API
 * ================================================================ */

int lush_zmq_ctx_new(void)
{
    void *ctx = zmq_ctx_new();
    if (!ctx) return -1;
    return handle_register(ctx, HTYPE_CTX, -1);
}

int lush_zmq_ctx_term(int ctx_id)
{
    void *ctx = handle_lookup(ctx_id);
    if (!ctx) return -1;
    int rc = zmq_ctx_term(ctx);
    handle_remove(ctx_id);
    return rc;
}


/* ================================================================
 * Socket API
 * ================================================================ */

int lush_zmq_socket(int ctx_id, int type)
{
    void *ctx = handle_lookup(ctx_id);
    if (!ctx) return -1;
    void *sock = zmq_socket(ctx, type);
    if (!sock) return -1;
    return handle_register(sock, HTYPE_SOCKET, ctx_id);
}

int lush_zmq_close(int id)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    int rc = zmq_close(sock);
    handle_remove(id);
    return rc;
}

int lush_zmq_bind(int id, const char *endpoint)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_bind(sock, endpoint);
}

int lush_zmq_connect(int id, const char *endpoint)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_connect(sock, endpoint);
}


/* ================================================================
 * Socket options
 * ================================================================ */

int lush_zmq_subscribe(int id, const char *topic, int topiclen)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_setsockopt(sock, ZMQ_SUBSCRIBE, topic, topiclen);
}

int lush_zmq_unsubscribe(int id, const char *topic, int topiclen)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_setsockopt(sock, ZMQ_UNSUBSCRIBE, topic, topiclen);
}

int lush_zmq_set_int_opt(int id, int option, int val)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_setsockopt(sock, option, &val, sizeof(val));
}

int lush_zmq_set_identity(int id, const char *identity, int len)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_setsockopt(sock, ZMQ_IDENTITY, identity, len);
}


/* ================================================================
 * I/O
 * ================================================================ */

int lush_zmq_send(int id, const unsigned char *buf, int len, int flags)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_send(sock, buf, len, flags);
}

int lush_zmq_recv(int id, unsigned char *buf, int maxlen, int flags)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    return zmq_recv(sock, buf, maxlen, flags);
}

int lush_zmq_get_rcvmore(int id)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    int more = 0;
    unsigned long moresz = sizeof(more);
    zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &moresz);
    return more;
}


/* ================================================================
 * ROUTER identity helpers
 *
 * When receiving on a ROUTER socket, ZMQ prepends identity frame(s)
 * and an empty delimiter before the payload.  These helpers store
 * the identity in C so Lush only sees the payload.
 *
 * lush_zmq_router_recv:
 *   1. Receive identity frame → store in handle_table[id].identity
 *   2. Receive empty delimiter → discard
 *   3. Receive payload → copy to buf, return length
 *
 * lush_zmq_router_send:
 *   1. Send stored identity frame (SNDMORE)
 *   2. Send empty delimiter (SNDMORE)
 *   3. Send payload
 * ================================================================ */

int lush_zmq_router_recv(int id, unsigned char *buf, int maxlen)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;

    /* 1. Receive identity frame */
    int id_len = zmq_recv(sock, handle_table[id].identity, 256, 0);
    if (id_len < 0) return -1;
    if (id_len > 256) id_len = 256;
    handle_table[id].identity_len = id_len;

    /* 2. Receive empty delimiter */
    unsigned char delim[16];
    int d_len = zmq_recv(sock, delim, sizeof(delim), 0);
    if (d_len < 0) return -1;

    /* 3. Receive payload */
    int n = zmq_recv(sock, buf, maxlen, 0);
    return n;
}

int lush_zmq_router_send(int id, const unsigned char *buf, int len)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    if (handle_table[id].identity_len <= 0) return -1;

    /* 1. Send identity frame */
    int rc = zmq_send(sock, handle_table[id].identity,
                      handle_table[id].identity_len, ZMQ_SNDMORE);
    if (rc < 0) return -1;

    /* 2. Send empty delimiter */
    rc = zmq_send(sock, "", 0, ZMQ_SNDMORE);
    if (rc < 0) return -1;

    /* 3. Send payload */
    return zmq_send(sock, buf, len, 0);
}


/* ================================================================
 * Zero-copy send
 *
 * Uses zmq_msg_init_data() to send directly from the caller's buffer
 * without copying.  ZMQ calls zc_free_callback when done with the data.
 * Only one zero-copy send can be in-flight at a time (single-threaded).
 * ================================================================ */

static volatile int zc_in_flight = 0;

static void zc_free_callback(void *data, void *hint)
{
    (void)data;
    (void)hint;
    zc_in_flight = 0;
}

int lush_zmq_send_zc(int id, unsigned char *buf, int len, int flags)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;

    zmq_msg_t msg;
    int rc = zmq_msg_init_data(&msg, buf, len, zc_free_callback, NULL);
    if (rc != 0) return -1;

    zc_in_flight = 1;
    rc = zmq_msg_send(&msg, sock, flags);
    if (rc < 0) {
        zc_in_flight = 0;
        zmq_msg_close(&msg);
        return -1;
    }
    return rc;
}

int lush_zmq_router_send_zc(int id, unsigned char *buf, int len)
{
    void *sock = handle_lookup(id);
    if (!sock) return -1;
    if (handle_table[id].identity_len <= 0) return -1;

    /* 1. Send identity frame (small, regular copy) */
    int rc = zmq_send(sock, handle_table[id].identity,
                      handle_table[id].identity_len, ZMQ_SNDMORE);
    if (rc < 0) return -1;

    /* 2. Send empty delimiter (small, regular copy) */
    rc = zmq_send(sock, "", 0, ZMQ_SNDMORE);
    if (rc < 0) return -1;

    /* 3. Send payload via zero-copy */
    return lush_zmq_send_zc(id, buf, len, 0);
}

int lush_zmq_zc_busy(void)
{
    return zc_in_flight;
}


/* ================================================================
 * Poll
 * ================================================================ */

#define MAX_POLL_ITEMS 16

static zmq_pollitem_t poll_items[MAX_POLL_ITEMS];

int lush_zmq_poll_set_socket(int idx, int sock_id, int events)
{
    if (idx < 0 || idx >= MAX_POLL_ITEMS) return -1;
    void *sock = handle_lookup(sock_id);
    if (!sock) return -1;
    poll_items[idx].socket = sock;
    poll_items[idx].fd = 0;
    poll_items[idx].events = (short)events;
    poll_items[idx].revents = 0;
    return 0;
}

int lush_zmq_poll_set_fd(int idx, int fd, int events)
{
    if (idx < 0 || idx >= MAX_POLL_ITEMS) return -1;
    poll_items[idx].socket = NULL;
    poll_items[idx].fd = fd;
    poll_items[idx].events = (short)events;
    poll_items[idx].revents = 0;
    return 0;
}

int lush_zmq_poll_clear(int idx)
{
    if (idx < 0 || idx >= MAX_POLL_ITEMS) return -1;
    memset(&poll_items[idx], 0, sizeof(zmq_pollitem_t));
    return 0;
}

int lush_zmq_poll_exec(int nitems, int timeout_ms)
{
    if (nitems < 0 || nitems > MAX_POLL_ITEMS) return -1;
    return zmq_poll(poll_items, nitems, timeout_ms);
}

int lush_zmq_poll_revents(int idx)
{
    if (idx < 0 || idx >= MAX_POLL_ITEMS) return 0;
    return (int)poll_items[idx].revents;
}


/* ================================================================
 * Error
 * ================================================================ */

int lush_zmq_errno(void)
{
    return zmq_errno();
}

const char *lush_zmq_strerror(int errnum)
{
    return zmq_strerror(errnum);
}
