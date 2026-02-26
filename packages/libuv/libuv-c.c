/* libuv-c.c -- Thin C wrapper around libuv for Lush FFI
 *
 * Three mechanisms:
 *   1. Handle table: maps integer IDs to uv_handle_t* pointers
 *   2. Event queue: ring buffer consumed by Lush after each uv_run
 *   3. Static callbacks registered with libuv that push events
 */

#include "libuv-c.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ================================================================
 * Handle table
 * ================================================================ */

#define MAX_HANDLES 1024

static struct {
    uv_handle_t *ptr;
    int in_use;
} handle_table[MAX_HANDLES];

static int next_handle_scan = 0;

/* Allocate an ID and store the handle pointer */
static int handle_register(uv_handle_t *h)
{
    for (int i = 0; i < MAX_HANDLES; i++) {
        int idx = (next_handle_scan + i) % MAX_HANDLES;
        if (!handle_table[idx].in_use) {
            handle_table[idx].ptr = h;
            handle_table[idx].in_use = 1;
            next_handle_scan = (idx + 1) % MAX_HANDLES;
            /* Store ID in handle's data field for callbacks */
            h->data = (void *)(long)idx;
            return idx;
        }
    }
    return -1;  /* table full */
}

static uv_handle_t *handle_lookup(int id)
{
    if (id < 0 || id >= MAX_HANDLES || !handle_table[id].in_use)
        return NULL;
    return handle_table[id].ptr;
}

static void handle_remove(int id)
{
    if (id >= 0 && id < MAX_HANDLES) {
        handle_table[id].ptr = NULL;
        handle_table[id].in_use = 0;
    }
}

static int handle_id_from_ptr(uv_handle_t *h)
{
    return (int)(long)h->data;
}

/* ================================================================
 * Event queue (ring buffer)
 * ================================================================ */

#define EVENT_QUEUE_SIZE 256

static lush_uv_event_t event_queue[EVENT_QUEUE_SIZE];
static int eq_head = 0;   /* next write position */
static int eq_tail = 0;   /* next read position */
static int eq_count = 0;

static void event_push(int type, int handle_id, int status,
                        const char *data, int data_len)
{
    if (eq_count >= EVENT_QUEUE_SIZE) return;  /* drop if full */

    lush_uv_event_t *ev = &event_queue[eq_head];
    ev->type = type;
    ev->handle_id = handle_id;
    ev->status = status;

    if (data && data_len > 0) {
        if (data_len > LUSH_UV_EVENT_DATA_SIZE)
            data_len = LUSH_UV_EVENT_DATA_SIZE;
        memcpy(ev->data, data, data_len);
        ev->data_len = data_len;
    } else {
        ev->data_len = 0;
    }

    eq_head = (eq_head + 1) % EVENT_QUEUE_SIZE;
    eq_count++;
}

int lush_uv_next_event(int *type, int *handle_id, int *status,
                        int *data_len, char *data_buf)
{
    if (eq_count <= 0) {
        *type = LUSH_UV_NO_EVENT;
        return 0;
    }

    lush_uv_event_t *ev = &event_queue[eq_tail];
    *type = ev->type;
    *handle_id = ev->handle_id;
    *status = ev->status;
    *data_len = ev->data_len;
    if (ev->data_len > 0 && data_buf)
        memcpy(data_buf, ev->data, ev->data_len);

    eq_tail = (eq_tail + 1) % EVENT_QUEUE_SIZE;
    eq_count--;
    return 1;
}

int lush_uv_event_pending(void)
{
    return eq_count > 0 ? 1 : 0;
}

/* ================================================================
 * Callbacks (static functions registered with libuv)
 * ================================================================ */

/* Timer callback */
static void _timer_cb(uv_timer_t *handle)
{
    int id = handle_id_from_ptr((uv_handle_t *)handle);
    event_push(LUSH_UV_TIMER_EVENT, id, 0, NULL, 0);
}

/* Alloc callback for read operations */
static void _alloc_cb(uv_handle_t *handle, size_t suggested_size,
                       uv_buf_t *buf)
{
    (void)handle;
    buf->base = (char *)malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

/* Read callback for TCP streams */
static void _read_cb(uv_stream_t *stream, ssize_t nread,
                      const uv_buf_t *buf)
{
    int id = handle_id_from_ptr((uv_handle_t *)stream);

    if (nread > 0) {
        int len = (int)nread;
        event_push(LUSH_UV_READ_EVENT, id, 0, buf->base, len);
    } else if (nread < 0) {
        /* EOF or error */
        event_push(LUSH_UV_READ_EVENT, id, (int)nread, NULL, 0);
    }
    /* nread == 0 means EAGAIN, no event needed */

    if (buf->base) free(buf->base);
}

/* Write callback */
typedef struct {
    uv_write_t req;
    int handle_id;
    char *buf;
} write_req_t;

static void _write_cb(uv_write_t *req, int status)
{
    write_req_t *wr = (write_req_t *)req;
    event_push(LUSH_UV_WRITE_COMPLETE, wr->handle_id, status, NULL, 0);
    free(wr->buf);
    free(wr);
}

/* Connection callback (server got new connection) */
static void _connection_cb(uv_stream_t *server, int status)
{
    int id = handle_id_from_ptr((uv_handle_t *)server);
    event_push(LUSH_UV_CONNECTION_EVENT, id, status, NULL, 0);
}

/* Connect callback (client connected to server) */
typedef struct {
    uv_connect_t req;
    int handle_id;
} connect_req_t;

static void _connect_cb(uv_connect_t *req, int status)
{
    connect_req_t *cr = (connect_req_t *)req;
    event_push(LUSH_UV_CONNECT_EVENT, cr->handle_id, status, NULL, 0);
    free(cr);
}

/* Close callback */
static void _close_cb(uv_handle_t *handle)
{
    int id = handle_id_from_ptr(handle);
    event_push(LUSH_UV_CLOSE_EVENT, id, 0, NULL, 0);
    handle_remove(id);
    free(handle);
}

/* Signal callback */
static void _signal_cb(uv_signal_t *handle, int signum)
{
    int id = handle_id_from_ptr((uv_handle_t *)handle);
    event_push(LUSH_UV_SIGNAL_EVENT, id, signum, NULL, 0);
}

/* Poll callback */
static void _poll_cb(uv_poll_t *handle, int status, int events)
{
    int id = handle_id_from_ptr((uv_handle_t *)handle);
    /* Encode events in status field for simplicity */
    event_push(LUSH_UV_POLL_EVENT, id, status, (const char *)&events,
               sizeof(int));
}

/* ================================================================
 * Loop API
 * ================================================================ */

void *lush_uv_loop_new(void)
{
    uv_loop_t *loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    if (!loop) return NULL;
    if (uv_loop_init(loop) != 0) {
        free(loop);
        return NULL;
    }
    return (void *)loop;
}

int lush_uv_loop_run(void *loop, int mode)
{
    if (!loop) return -1;
    uv_run_mode m;
    switch (mode) {
        case 0: m = UV_RUN_DEFAULT; break;
        case 1: m = UV_RUN_ONCE; break;
        case 2: m = UV_RUN_NOWAIT; break;
        default: m = UV_RUN_DEFAULT; break;
    }
    return uv_run((uv_loop_t *)loop, m);
}

void lush_uv_loop_stop(void *loop)
{
    if (loop) uv_stop((uv_loop_t *)loop);
}

int lush_uv_loop_close(void *loop)
{
    if (!loop) return -1;
    int rc = uv_loop_close((uv_loop_t *)loop);
    if (rc == 0) free(loop);
    return rc;
}

int lush_uv_loop_alive(void *loop)
{
    if (!loop) return 0;
    return uv_loop_alive((uv_loop_t *)loop);
}

/* ================================================================
 * Handle management
 * ================================================================ */

void lush_uv_close_handle(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (h && !uv_is_closing(h)) {
        uv_close(h, _close_cb);
    }
}

int lush_uv_handle_type(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;
    return (int)h->type;
}

/* ================================================================
 * Timer API
 * ================================================================ */

int lush_uv_timer_start(void *loop, int timeout_ms, int repeat_ms)
{
    if (!loop) return -1;
    uv_timer_t *timer = (uv_timer_t *)malloc(sizeof(uv_timer_t));
    if (!timer) return -1;

    if (uv_timer_init((uv_loop_t *)loop, timer) != 0) {
        free(timer);
        return -1;
    }

    int id = handle_register((uv_handle_t *)timer);
    if (id < 0) {
        free(timer);
        return -1;
    }

    if (uv_timer_start(timer, _timer_cb, timeout_ms, repeat_ms) != 0) {
        handle_remove(id);
        free(timer);
        return -1;
    }

    return id;
}

int lush_uv_timer_stop(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;
    return uv_timer_stop((uv_timer_t *)h);
}

int lush_uv_timer_again(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;
    return uv_timer_again((uv_timer_t *)h);
}

/* ================================================================
 * TCP API
 * ================================================================ */

int lush_uv_tcp_new(void *loop)
{
    if (!loop) return -1;
    uv_tcp_t *tcp = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if (!tcp) return -1;

    if (uv_tcp_init((uv_loop_t *)loop, tcp) != 0) {
        free(tcp);
        return -1;
    }

    int id = handle_register((uv_handle_t *)tcp);
    if (id < 0) {
        free(tcp);
        return -1;
    }

    return id;
}

int lush_uv_tcp_bind(int handle_id, const char *ip, int port)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    struct sockaddr_in addr;
    if (uv_ip4_addr(ip, port, &addr) != 0) return -1;

    return uv_tcp_bind((uv_tcp_t *)h, (struct sockaddr *)&addr, 0);
}

int lush_uv_tcp_listen(int handle_id, int backlog)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    return uv_listen((uv_stream_t *)h, backlog, _connection_cb);
}

int lush_uv_tcp_accept(int server_id)
{
    uv_handle_t *server = handle_lookup(server_id);
    if (!server) return -1;

    /* Get loop from server handle */
    uv_loop_t *loop = server->loop;

    uv_tcp_t *client = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
    if (!client) return -1;

    if (uv_tcp_init(loop, client) != 0) {
        free(client);
        return -1;
    }

    int id = handle_register((uv_handle_t *)client);
    if (id < 0) {
        free(client);
        return -1;
    }

    if (uv_accept((uv_stream_t *)server, (uv_stream_t *)client) != 0) {
        handle_remove(id);
        free(client);
        return -1;
    }

    return id;
}

int lush_uv_tcp_connect(int handle_id, const char *ip, int port)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    struct sockaddr_in addr;
    if (uv_ip4_addr(ip, port, &addr) != 0) return -1;

    connect_req_t *cr = (connect_req_t *)malloc(sizeof(connect_req_t));
    if (!cr) return -1;
    cr->handle_id = handle_id;

    int rc = uv_tcp_connect(&cr->req, (uv_tcp_t *)h,
                             (struct sockaddr *)&addr, _connect_cb);
    if (rc != 0) {
        free(cr);
        return rc;
    }
    return 0;
}

int lush_uv_tcp_read_start(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    return uv_read_start((uv_stream_t *)h, _alloc_cb, _read_cb);
}

int lush_uv_tcp_read_stop(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    return uv_read_stop((uv_stream_t *)h);
}

int lush_uv_tcp_write(int handle_id, const char *data, int len)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h || !data || len <= 0) return -1;

    write_req_t *wr = (write_req_t *)malloc(sizeof(write_req_t));
    if (!wr) return -1;

    wr->handle_id = handle_id;
    wr->buf = (char *)malloc(len);
    if (!wr->buf) {
        free(wr);
        return -1;
    }
    memcpy(wr->buf, data, len);

    uv_buf_t buf = uv_buf_init(wr->buf, len);
    int rc = uv_write(&wr->req, (uv_stream_t *)h, &buf, 1, _write_cb);
    if (rc != 0) {
        free(wr->buf);
        free(wr);
        return rc;
    }
    return 0;
}

/* ================================================================
 * Poll API
 * ================================================================ */

int lush_uv_poll_start(void *loop, int fd, int events)
{
    if (!loop) return -1;

    uv_poll_t *poll = (uv_poll_t *)malloc(sizeof(uv_poll_t));
    if (!poll) return -1;

    if (uv_poll_init((uv_loop_t *)loop, poll, fd) != 0) {
        free(poll);
        return -1;
    }

    int id = handle_register((uv_handle_t *)poll);
    if (id < 0) {
        free(poll);
        return -1;
    }

    int uv_events = 0;
    if (events & 1) uv_events |= UV_READABLE;
    if (events & 2) uv_events |= UV_WRITABLE;

    if (uv_poll_start(poll, uv_events, _poll_cb) != 0) {
        handle_remove(id);
        free(poll);
        return -1;
    }

    return id;
}

int lush_uv_poll_stop(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    return uv_poll_stop((uv_poll_t *)h);
}

/* ================================================================
 * Signal API
 * ================================================================ */

int lush_uv_signal_start(void *loop, int signum)
{
    if (!loop) return -1;

    uv_signal_t *sig = (uv_signal_t *)malloc(sizeof(uv_signal_t));
    if (!sig) return -1;

    if (uv_signal_init((uv_loop_t *)loop, sig) != 0) {
        free(sig);
        return -1;
    }

    int id = handle_register((uv_handle_t *)sig);
    if (id < 0) {
        free(sig);
        return -1;
    }

    if (uv_signal_start(sig, _signal_cb, signum) != 0) {
        handle_remove(id);
        free(sig);
        return -1;
    }

    return id;
}

int lush_uv_signal_stop(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    return uv_signal_stop((uv_signal_t *)h);
}

/* ================================================================
 * Utilities
 * ================================================================ */

int lush_uv_tcp_getport(int handle_id)
{
    uv_handle_t *h = handle_lookup(handle_id);
    if (!h) return -1;

    struct sockaddr_storage addr;
    int namelen = sizeof(addr);
    if (uv_tcp_getsockname((uv_tcp_t *)h, (struct sockaddr *)&addr,
                            &namelen) != 0)
        return -1;

    if (addr.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&addr)->sin_port);
    if (addr.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
    return -1;
}

int lush_uv_kill_self(int signum)
{
    return kill(getpid(), signum);
}
