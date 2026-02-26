/* libuv-c.h -- Thin C wrapper around libuv for Lush FFI
 *
 * Provides:
 *   - Handle table: maps integer IDs to uv_handle_t* pointers
 *   - Event queue: ring buffer of typed events for Lush consumption
 *   - Wrapper functions for loop, timer, tcp, poll, signal
 *   - Static callbacks that push events into the queue
 *
 * Lush code references handles by integer ID (not raw pointers).
 * After each uv_run cycle, Lush drains events via lush_uv_next_event().
 */

#ifndef LUSH_LIBUV_C_H
#define LUSH_LIBUV_C_H

/* ---- Event types ---- */
#define LUSH_UV_NO_EVENT         0
#define LUSH_UV_TIMER_EVENT      1
#define LUSH_UV_READ_EVENT       2
#define LUSH_UV_WRITE_COMPLETE   3
#define LUSH_UV_CONNECT_EVENT    4
#define LUSH_UV_CLOSE_EVENT      5
#define LUSH_UV_SIGNAL_EVENT     6
#define LUSH_UV_POLL_EVENT       7
#define LUSH_UV_CONNECTION_EVENT 8

/* ---- Event structure ---- */
/* Each event carries: type, handle_id, status, and up to 8KB of data */
#define LUSH_UV_EVENT_DATA_SIZE 8192

typedef struct {
    int type;          /* LUSH_UV_*_EVENT */
    int handle_id;     /* handle table ID */
    int status;        /* 0 = success, <0 = error */
    int data_len;      /* bytes used in data[] */
    char data[LUSH_UV_EVENT_DATA_SIZE];
} lush_uv_event_t;

/* ---- Loop ---- */
void *lush_uv_loop_new(void);
int   lush_uv_loop_run(void *loop, int mode);
                        /* mode: 0=default, 1=once, 2=nowait */
void  lush_uv_loop_stop(void *loop);
int   lush_uv_loop_close(void *loop);
int   lush_uv_loop_alive(void *loop);

/* ---- Event queue ---- */
/* Returns 1 if an event was copied into *ev, 0 if queue empty */
int   lush_uv_next_event(int *type, int *handle_id, int *status,
                          int *data_len, char *data_buf);
int   lush_uv_event_pending(void);

/* ---- Handle management ---- */
void  lush_uv_close_handle(int handle_id);
int   lush_uv_handle_type(int handle_id);
                           /* returns UV_TIMER, UV_TCP, etc. or -1 */

/* ---- Timer ---- */
int   lush_uv_timer_start(void *loop, int timeout_ms, int repeat_ms);
                           /* returns handle_id */
int   lush_uv_timer_stop(int handle_id);
int   lush_uv_timer_again(int handle_id);

/* ---- TCP ---- */
int   lush_uv_tcp_new(void *loop);
                       /* returns handle_id */
int   lush_uv_tcp_bind(int handle_id, const char *ip, int port);
int   lush_uv_tcp_listen(int handle_id, int backlog);
int   lush_uv_tcp_accept(int server_id);
                          /* returns new handle_id for client */
int   lush_uv_tcp_connect(int handle_id, const char *ip, int port);
int   lush_uv_tcp_read_start(int handle_id);
int   lush_uv_tcp_read_stop(int handle_id);
int   lush_uv_tcp_write(int handle_id, const char *data, int len);

/* ---- Poll ---- */
int   lush_uv_poll_start(void *loop, int fd, int events);
                          /* events: 1=readable, 2=writable, 3=both */
                          /* returns handle_id */
int   lush_uv_poll_stop(int handle_id);

/* ---- Signal ---- */
int   lush_uv_signal_start(void *loop, int signum);
                            /* returns handle_id */
int   lush_uv_signal_stop(int handle_id);

/* ---- Utilities ---- */
int   lush_uv_tcp_getport(int handle_id);
                            /* returns bound port number, or -1 */
int   lush_uv_kill_self(int signum);
                            /* send signal to own process */

#endif /* LUSH_LIBUV_C_H */
