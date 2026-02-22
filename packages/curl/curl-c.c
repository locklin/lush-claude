/* curl-c.c -- Thin C wrappers around libcurl for Lush FFI
 *
 * Provides REST (GET/POST/PUT/DELETE/PATCH) and WebSocket functions.
 * Pointers are passed as void* so Lush can treat them as gptr values.
 */

#include "curl-c.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

/* ---- internal: growable buffer for response data ---- */

typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_buf_t;

static size_t _write_callback(char *ptr, size_t size, size_t nmemb,
                               void *userdata)
{
    resp_buf_t *rb = (resp_buf_t *)userdata;
    int nbytes = (int)(size * nmemb);
    if (rb->len + nbytes >= rb->cap) {
        int newcap = (rb->cap + nbytes) * 2;
        char *tmp = (char *)realloc(rb->buf, newcap);
        if (!tmp) return 0;
        rb->buf = tmp;
        rb->cap = newcap;
    }
    memcpy(rb->buf + rb->len, ptr, nbytes);
    rb->len += nbytes;
    rb->buf[rb->len] = '\0';
    return (size_t)nbytes;
}

static void _init_resp_buf(resp_buf_t *rb)
{
    rb->cap = 4096;
    rb->buf = (char *)malloc(rb->cap);
    rb->len = 0;
    if (rb->buf) rb->buf[0] = '\0';
}

/* ---- internal: parse newline-delimited header string ---- */

static struct curl_slist *_parse_headers(const char *headers)
{
    struct curl_slist *slist = NULL;
    const char *p, *nl;
    char line[2048];

    if (!headers || !headers[0]) return NULL;

    p = headers;
    while (*p) {
        nl = strchr(p, '\n');
        if (nl) {
            int len = (int)(nl - p);
            if (len > 0 && len < (int)sizeof(line)) {
                memcpy(line, p, len);
                /* strip trailing \r */
                if (len > 0 && line[len-1] == '\r') len--;
                line[len] = '\0';
                if (len > 0)
                    slist = curl_slist_append(slist, line);
            }
            p = nl + 1;
        } else {
            /* last line without trailing newline */
            if (strlen(p) > 0 && strlen(p) < sizeof(line))
                slist = curl_slist_append(slist, p);
            break;
        }
    }
    return slist;
}

/* ---- lifecycle ---- */

static int _global_initialized = 0;

void lush_curl_global_init(void)
{
    if (!_global_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        _global_initialized = 1;
    }
}

/* ---- REST / HTTP ---- */

char *lush_curl_request(const char *method, const char *url,
                        const char *body, const char *headers,
                        int timeout_sec, int *status_code, int *resp_len)
{
    CURL *curl;
    CURLcode res;
    resp_buf_t rb;
    struct curl_slist *hdr_list = NULL;
    long http_code = 0;

    lush_curl_global_init();

    *status_code = 0;
    *resp_len = 0;

    curl = curl_easy_init();
    if (!curl) return NULL;

    _init_resp_buf(&rb);
    if (!rb.buf) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);

    /* Set HTTP method */
    if (method && strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    /* Set request body */
    if (body && body[0]) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    /* Set headers */
    hdr_list = _parse_headers(headers);
    if (hdr_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    }

    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        *status_code = (int)http_code;
        *resp_len = rb.len;
    } else {
        /* On error, put error message in buffer */
        free(rb.buf);
        const char *errmsg = curl_easy_strerror(res);
        rb.buf = (char *)malloc(strlen(errmsg) + 1);
        if (rb.buf) strcpy(rb.buf, errmsg);
        *status_code = -1;
        *resp_len = rb.buf ? (int)strlen(rb.buf) : 0;
    }

    if (hdr_list) curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);

    return rb.buf;
}

char *lush_curl_get(const char *url, const char *headers,
                    int timeout_sec, int *status_code, int *resp_len)
{
    return lush_curl_request("GET", url, NULL, headers,
                             timeout_sec, status_code, resp_len);
}

char *lush_curl_post(const char *url, const char *body,
                     const char *content_type, const char *headers,
                     int timeout_sec, int *status_code, int *resp_len)
{
    /* Prepend Content-Type to headers */
    char combined[8192];
    const char *ct = content_type;
    if (!ct || !ct[0]) ct = "application/json";

    if (headers && headers[0]) {
        snprintf(combined, sizeof(combined),
                 "Content-Type: %s\n%s", ct, headers);
    } else {
        snprintf(combined, sizeof(combined),
                 "Content-Type: %s", ct);
    }

    return lush_curl_request("POST", url, body, combined,
                             timeout_sec, status_code, resp_len);
}

void lush_curl_free_response(char *resp)
{
    if (resp) free(resp);
}

/* ---- WebSocket ---- */

void *lush_curl_ws_connect(const char *url, const char *headers)
{
    CURL *curl;
    CURLcode res;
    struct curl_slist *hdr_list = NULL;

    lush_curl_global_init();

    curl = curl_easy_init();
    if (!curl) return NULL;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);

    hdr_list = _parse_headers(headers);
    if (hdr_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    }

    res = curl_easy_perform(curl);

    if (hdr_list) curl_slist_free_all(hdr_list);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        return NULL;
    }

    return (void *)curl;
}

int lush_curl_ws_send_text(void *handle, const char *msg, int len)
{
    CURL *curl = (CURL *)handle;
    size_t sent = 0;
    CURLcode res;

    if (!curl || !msg) return -1;

    res = curl_ws_send(curl, msg, (size_t)len, &sent, 0, CURLWS_TEXT);
    return (res == CURLE_OK) ? (int)sent : -1;
}

int lush_curl_ws_send_binary(void *handle, const unsigned char *data, int len)
{
    CURL *curl = (CURL *)handle;
    size_t sent = 0;
    CURLcode res;

    if (!curl || !data) return -1;

    res = curl_ws_send(curl, data, (size_t)len, &sent, 0, CURLWS_BINARY);
    return (res == CURLE_OK) ? (int)sent : -1;
}

int lush_curl_ws_poll(void *handle, int timeout_ms)
{
    CURL *curl = (CURL *)handle;
    curl_socket_t sockfd;
    CURLcode res;

    if (!curl) return -1;

    res = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
    if (res != CURLE_OK || sockfd == CURL_SOCKET_BAD) return -1;

#ifdef _WIN32
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select((int)sockfd + 1, &readfds, NULL, NULL, &tv) > 0 ? 1 : 0;
#else
    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, timeout_ms);
    return (ret > 0 && (pfd.revents & POLLIN)) ? 1 : 0;
#endif
}

char *lush_curl_ws_recv(void *handle, int timeout_ms,
                        int *out_len, int *out_flags)
{
    CURL *curl = (CURL *)handle;
    char tmpbuf[65536];
    size_t nread = 0;
    const struct curl_ws_frame *meta;
    CURLcode res;

    *out_len = 0;
    *out_flags = 0;

    if (!curl) return NULL;

    /* Poll for data availability */
    if (timeout_ms > 0) {
        int ready = lush_curl_ws_poll(handle, timeout_ms);
        if (ready <= 0) return NULL;
    }

    res = curl_ws_recv(curl, tmpbuf, sizeof(tmpbuf), &nread, &meta);

    if (res != CURLE_OK || nread == 0) return NULL;

    /* Copy data to malloc'd buffer for Lush */
    char *result = (char *)malloc(nread + 1);
    if (!result) return NULL;
    memcpy(result, tmpbuf, nread);
    result[nread] = '\0';

    *out_len = (int)nread;
    *out_flags = (int)(meta ? meta->flags : 0);

    return result;
}

int lush_curl_ws_close(void *handle)
{
    CURL *curl = (CURL *)handle;
    size_t sent = 0;

    if (!curl) return -1;

    /* Send close frame */
    curl_ws_send(curl, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(curl);

    return 0;
}
