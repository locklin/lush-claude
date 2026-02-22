/* curl-c.h -- Thin C wrappers around libcurl for Lush FFI */

#ifndef LUSH_CURL_C_H
#define LUSH_CURL_C_H

/* Lifecycle */
void  lush_curl_global_init(void);

/* REST / HTTP
 * Returns malloc'd response body; sets *status_code and *resp_len.
 * Caller must free via lush_curl_free_response().
 * headers: newline-delimited "Key: Value\nKey2: Value2" or NULL.
 */
char *lush_curl_get(const char *url, const char *headers,
                    int timeout_sec, int *status_code, int *resp_len);
char *lush_curl_post(const char *url, const char *body,
                     const char *content_type, const char *headers,
                     int timeout_sec, int *status_code, int *resp_len);
char *lush_curl_request(const char *method, const char *url,
                        const char *body, const char *headers,
                        int timeout_sec, int *status_code, int *resp_len);
void  lush_curl_free_response(char *resp);

/* WebSocket */
void *lush_curl_ws_connect(const char *url, const char *headers);
int   lush_curl_ws_send_text(void *curl, const char *msg, int len);
int   lush_curl_ws_send_binary(void *curl, const unsigned char *data, int len);
char *lush_curl_ws_recv(void *curl, int timeout_ms,
                        int *out_len, int *out_flags);
int   lush_curl_ws_close(void *curl);
int   lush_curl_ws_poll(void *curl, int timeout_ms);

#endif /* LUSH_CURL_C_H */
