/* zmq-c.h -- Minimal ZMQ API declarations for Lush FFI
 *
 * Provides the ~15 ZMQ C API functions and ~20 constants needed by the
 * Lush ZMQ package.  This is a self-contained header that replaces
 * the need for libzmq dev headers (zmq.h).  The ZMQ C API is ABI-stable
 * across 4.x, so we can declare just the subset we use.
 *
 * The Lush wrapper layer (zmq-c.c) adds:
 *   - Handle table: maps integer IDs to void* (context or socket)
 *   - ROUTER identity management in C (Lush never sees identity frames)
 *   - Static poll item array with setter/getter functions
 */

#ifndef LUSH_ZMQ_C_H
#define LUSH_ZMQ_C_H

#include <stddef.h>  /* size_t */

/* ---- Socket type constants ---- */
#define ZMQ_PUB      1
#define ZMQ_SUB      2
#define ZMQ_DEALER   5
#define ZMQ_ROUTER   6

/* ---- Socket option constants ---- */
#define ZMQ_SUBSCRIBE     6
#define ZMQ_UNSUBSCRIBE   7
#define ZMQ_LINGER       17
#define ZMQ_RECONNECT_IVL 18
#define ZMQ_SNDHWM       23
#define ZMQ_RCVHWM       24
#define ZMQ_RCVMORE      13
#define ZMQ_IDENTITY      5

/* ---- Send/recv flag constants ---- */
#define ZMQ_DONTWAIT  1
#define ZMQ_SNDMORE   2

/* ---- Poll event constants ---- */
#define ZMQ_POLLIN    1
#define ZMQ_POLLOUT   2
#define ZMQ_POLLERR   4

/* ---- zmq_pollitem_t ---- */
typedef struct {
    void *socket;
    int   fd;
    short events;
    short revents;
} zmq_pollitem_t;

/* ---- ZMQ C API function declarations ---- */
/* These are declared here so zmq-c.c can call them.
 * At link time they resolve against libzmq.so.5. */
void *zmq_ctx_new(void);
int   zmq_ctx_term(void *context);
void *zmq_socket(void *context, int type);
int   zmq_close(void *socket);
int   zmq_bind(void *socket, const char *endpoint);
int   zmq_connect(void *socket, const char *endpoint);
int   zmq_setsockopt(void *socket, int option, const void *optval,
                     unsigned long optvallen);
int   zmq_getsockopt(void *socket, int option, void *optval,
                     unsigned long *optvallen);
int   zmq_send(void *socket, const void *buf, unsigned long len, int flags);
int   zmq_recv(void *socket, void *buf, unsigned long len, int flags);
int   zmq_poll(zmq_pollitem_t *items, int nitems, long timeout);
int   zmq_errno(void);
const char *zmq_strerror(int errnum);

/* ---- Zero-copy message API (zmq_msg_*) ---- */
typedef struct zmq_msg_t { unsigned char _[64]; } zmq_msg_t;
typedef void (zmq_free_fn)(void *data, void *hint);

int zmq_msg_init_data(zmq_msg_t *msg, void *data, size_t size,
                       zmq_free_fn *ffn, void *hint);
int zmq_msg_send(zmq_msg_t *msg, void *socket, int flags);
int zmq_msg_close(zmq_msg_t *msg);

/* ---- Lush wrapper API ---- */

/* Context */
int   lush_zmq_ctx_new(void);
int   lush_zmq_ctx_term(int ctx_id);

/* Socket lifecycle */
int   lush_zmq_socket(int ctx_id, int type);
int   lush_zmq_close(int id);
int   lush_zmq_bind(int id, const char *endpoint);
int   lush_zmq_connect(int id, const char *endpoint);

/* Socket options */
int   lush_zmq_subscribe(int id, const char *topic, int topiclen);
int   lush_zmq_unsubscribe(int id, const char *topic, int topiclen);
int   lush_zmq_set_int_opt(int id, int option, int val);
int   lush_zmq_set_identity(int id, const char *identity, int len);

/* I/O */
int   lush_zmq_send(int id, const unsigned char *buf, int len, int flags);
int   lush_zmq_recv(int id, unsigned char *buf, int maxlen, int flags);
int   lush_zmq_get_rcvmore(int id);

/* ROUTER identity helpers */
int   lush_zmq_router_recv(int id, unsigned char *buf, int maxlen);
int   lush_zmq_router_send(int id, const unsigned char *buf, int len);

/* Poll */
int   lush_zmq_poll_set_socket(int idx, int sock_id, int events);
int   lush_zmq_poll_set_fd(int idx, int fd, int events);
int   lush_zmq_poll_clear(int idx);
int   lush_zmq_poll_exec(int nitems, int timeout_ms);
int   lush_zmq_poll_revents(int idx);

/* Zero-copy send */
int   lush_zmq_send_zc(int id, unsigned char *buf, int len, int flags);
int   lush_zmq_router_send_zc(int id, unsigned char *buf, int len);
int   lush_zmq_zc_busy(void);

/* Error */
int   lush_zmq_errno(void);
const char *lush_zmq_strerror(int errnum);

#endif /* LUSH_ZMQ_C_H */
