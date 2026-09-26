/* Client HTTP di ds4_media. Vedi ds4_media_http.h. */
#include "ds4_media_http.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── buffer e allocazione ─────────────────────────────────────────────────── */

void *media_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "ds4: media out of memory\n"); abort(); }
    return p;
}

char *media_xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = media_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

void media_buf_append(media_buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->ptr, cap);
        if (!p) { fprintf(stderr, "ds4: media out of memory\n"); abort(); }
        b->ptr = p;
        b->cap = cap;
    }
    memcpy(b->ptr + b->len, s, n);
    b->len += n;
    b->ptr[b->len] = '\0';
}

void media_buf_puts(media_buf *b, const char *s) { media_buf_append(b, s, strlen(s)); }

char *media_buf_take(media_buf *b) {
    char *p = b->ptr ? b->ptr : media_xstrdup("");
    b->ptr = NULL;
    b->len = b->cap = 0;
    return p;
}

void media_set_err(char *err, size_t err_len, const char *fmt, ...) {
    if (!err || !err_len) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

void media_http_response_free(media_http_response *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
    r->status = 0;
}

/* ── socket ───────────────────────────────────────────────────────────────── */

#define MEDIA_CONNECT_TIMEOUT_MS 3000

int media_tcp_connect(const char *host, int port, char *err, size_t err_len) {
    char service[32];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int gai = getaddrinfo(host, service, &hints, &res);
    if (gai != 0) {
        media_set_err(err, err_len, "getaddrinfo %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int fd = -1, last_errno = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) { if (flags >= 0) fcntl(fd, F_SETFL, flags); break; }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, MEDIA_CONNECT_TIMEOUT_MS) > 0) {
                int soerr = 0;
                socklen_t slen = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
                if (soerr == 0) { if (flags >= 0) fcntl(fd, F_SETFL, flags); break; }
                errno = soerr;
            } else {
                errno = ETIMEDOUT;
            }
        }
        last_errno = errno;   /* close() puo' sporcarlo */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        media_set_err(err, err_len, "ComfyUI non risponde su %s:%d (%s): accendi ComfyUI (spark-switch avvia 18)",
                      host, port, strerror(last_errno));
    return fd;
}

int media_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
#ifdef MSG_NOSIGNAL
        /* send() per non ricevere SIGPIPE da un peer chiuso; sui file vale write(). */
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == ENOTSOCK) n = write(fd, p, len);
#else
        ssize_t n = write(fd, p, len);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Annullamento del lavoro in corso su questo thread (vedi media_http_set_cancel). */
static __thread media_http_cancel_fn tl_cancel;
static __thread void *tl_cancel_pd;

void media_http_set_cancel(media_http_cancel_fn fn, void *privdata) {
    tl_cancel = fn;
    tl_cancel_pd = privdata;
}

static long media_http_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Legge l'intera risposta finche' il peer chiude. timeout_ms e' il silenzio massimo
 * tra due letture (un download lungo ma vivo non scade). L'attesa e' a fette di
 * 250 ms: tra una fetta e l'altra si guarda se il lavoro e' stato annullato, cosi'
 * un Ctrl+C non aspetta un ComfyUI lento a rispondere. */
static bool media_read_all(int fd, int timeout_ms, media_buf *out, char *err, size_t err_len) {
    char tmp[8192];
    long deadline = media_http_now_ms() + timeout_ms;
    for (;;) {
        if (tl_cancel && tl_cancel(tl_cancel_pd)) { media_set_err(err, err_len, "annullato"); return false; }
        long left = deadline - media_http_now_ms();
        if (left <= 0) { media_set_err(err, err_len, "timeout leggendo la risposta di ComfyUI"); return false; }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, left < 250 ? (int)left : 250);
        if (rc == 0) continue;
        if (rc < 0) { if (errno == EINTR) continue; media_set_err(err, err_len, "poll: %s", strerror(errno)); return false; }
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0) { if (errno == EINTR) continue; media_set_err(err, err_len, "read: %s", strerror(errno)); return false; }
        if (n == 0) break;
        media_buf_append(out, tmp, (size_t)n);
        deadline = media_http_now_ms() + timeout_ms;
    }
    return true;
}

/* Decodifica un corpo chunked in-place (RFC 7230); torna la nuova lunghezza. */
static size_t media_dechunk(char *body, size_t len) {
    char *out = body, *p = body, *end = body + len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        long sz = strtol(p, NULL, 16);
        p = nl + 1;
        if (sz <= 0) break;
        if (p + sz > end) sz = end - p;
        memmove(out, p, (size_t)sz);
        out += sz;
        p += sz;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    return (size_t)(out - body);
}

/* Spezza la risposta in stato + corpo, gestendo Content-Length/chunked. */
static bool media_parse_response(media_buf *raw, media_http_response *resp,
                                 char *err, size_t err_len) {
    if (!raw->ptr || raw->len < 12) { media_set_err(err, err_len, "risposta HTTP vuota"); return false; }
    resp->status = (int)strtol(raw->ptr + 9, NULL, 10);   /* "HTTP/1.1 XYZ" */
    /* fine header: cerca \r\n\r\n nei byte (binary-safe). */
    char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < raw->len; i++)
        if (raw->ptr[i] == '\r' && raw->ptr[i+1] == '\n' && raw->ptr[i+2] == '\r' && raw->ptr[i+3] == '\n') {
            hdr_end = raw->ptr + i + 4;
            break;
        }
    if (!hdr_end) { media_set_err(err, err_len, "risposta HTTP malformata"); return false; }
    bool chunked = false;
    long clen = -1;
    for (char *p = raw->ptr; p < hdr_end; p++) {
        if (p != raw->ptr && p[-1] != '\n') continue;
        if (strncasecmp(p, "Transfer-Encoding:", 18) == 0 &&
            strstr(p, "chunked") && strstr(p, "chunked") < strchr(p, '\n'))
            chunked = true;
        if (strncasecmp(p, "Content-Length:", 15) == 0) clen = strtol(p + 15, NULL, 10);
    }
    size_t body_len = raw->len - (size_t)(hdr_end - raw->ptr);
    /* Leggiamo fino alla chiusura: meno byte di quelli dichiarati = risposta troncata
     * (un file salvato a meta' sembrerebbe un successo). */
    if (!chunked && clen >= 0 && (size_t)clen != body_len) {
        media_set_err(err, err_len, "risposta troncata: %zu byte su %ld dichiarati", body_len, clen);
        return false;
    }
    char *body = media_xmalloc(body_len + 1);
    memcpy(body, hdr_end, body_len);
    if (chunked) body_len = media_dechunk(body, body_len);
    body[body_len] = '\0';
    resp->body = body;
    resp->body_len = body_len;
    return true;
}

static bool media_send(const char *host, int port, media_buf *req, int timeout_ms,
                       media_http_response *resp, char *err, size_t err_len) {
    int fd = media_tcp_connect(host, port, err, err_len);
    if (fd < 0) { free(req->ptr); return false; }
    int wr = media_write_all(fd, req->ptr, req->len);
    free(req->ptr);
    req->ptr = NULL; req->len = req->cap = 0;
    if (wr != 0) { media_set_err(err, err_len, "invio richiesta HTTP: %s", strerror(errno)); close(fd); return false; }
    media_buf raw = {0};
    bool ok = media_read_all(fd, timeout_ms, &raw, err, err_len);
    close(fd);
    if (!ok) { free(raw.ptr); return false; }
    ok = media_parse_response(&raw, resp, err, err_len);
    free(raw.ptr);
    return ok;
}

bool media_http_get(const char *host, int port, const char *path, int timeout_ms,
                    media_http_response *resp, char *err, size_t err_len) {
    /* Il path puo' venire da ComfyUI (nomi di file): niente buffer fissi da troncare. */
    media_buf req = {0};
    char tail[320];
    snprintf(tail, sizeof(tail), " HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", host, port);
    media_buf_puts(&req, "GET ");
    media_buf_puts(&req, path);
    media_buf_puts(&req, tail);
    return media_send(host, port, &req, timeout_ms, resp, err, err_len);
}

bool media_http_post(const char *host, int port, const char *path,
                     const char *content_type, const void *body, size_t body_len,
                     int timeout_ms, media_http_response *resp, char *err, size_t err_len) {
    media_buf req = {0};
    char tail[512];
    snprintf(tail, sizeof(tail), " HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
             "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n", host, port, content_type, body_len);
    media_buf_puts(&req, "POST ");
    media_buf_puts(&req, path);
    media_buf_puts(&req, tail);
    media_buf_append(&req, (const char *)body, body_len);
    return media_send(host, port, &req, timeout_ms, resp, err, err_len);
}

bool media_http_post_image(const char *host, int port, const char *path,
                           const char *const *fields, const char *file_field,
                           const char *file_name, const void *file_bytes, size_t file_len,
                           int timeout_ms, media_http_response *resp, char *err, size_t err_len) {
    const char *bound = "ds4mediaBoundary8f2a";
    media_buf body = {0};
    char h[512];
    for (int i = 0; fields && fields[i] && fields[i + 1]; i += 2) {
        snprintf(h, sizeof(h),
                 "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n", bound, fields[i]);
        media_buf_puts(&body, h);
        media_buf_puts(&body, fields[i + 1]);
        media_buf_puts(&body, "\r\n");
    }
    snprintf(h, sizeof(h),
             "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n", bound, file_field, file_name);
    media_buf_puts(&body, h);
    media_buf_append(&body, (const char *)file_bytes, file_len);
    media_buf_puts(&body, "\r\n");
    snprintf(h, sizeof(h), "--%s--\r\n", bound);
    media_buf_puts(&body, h);

    char ctype[128];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", bound);
    size_t blen = body.len;
    char *bytes = media_buf_take(&body);
    bool ok = media_http_post(host, port, path, ctype, bytes, blen, timeout_ms, resp, err, err_len);
    free(bytes);
    return ok;
}

/* ── mini JSON ────────────────────────────────────────────────────────────── */

/* Quattro cifre esadecimali in *out; false se ne manca una (si ferma al NUL). */
static bool media_hex4(const char *p, long *out) {
    long v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)p[i])) return false;
        char c = p[i];
        v = v * 16 + (isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
    }
    *out = v;
    return true;
}

/* Codifica un punto di codice in UTF-8 (1-4 byte). */
static void media_buf_put_utf8(media_buf *b, long cp) {
    unsigned char u[4];
    int n;
    if (cp < 0x80) { u[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = 0xC0 | (cp >> 6); u[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { u[0] = 0xE0 | (cp >> 12); u[1] = 0x80 | ((cp >> 6) & 0x3F); u[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { u[0] = 0xF0 | (cp >> 18); u[1] = 0x80 | ((cp >> 12) & 0x3F); u[2] = 0x80 | ((cp >> 6) & 0x3F); u[3] = 0x80 | (cp & 0x3F); n = 4; }
    media_buf_append(b, (const char *)u, (size_t)n);
}

/* Decodifica la stringa JSON che inizia in p (virgolette comprese). Le sequenze \uXXXX
 * diventano UTF-8, coppie surrogate incluse (cosi' i client Python con ensure_ascii,
 * come Open WebUI, non perdono accenti ed emoji); una \u malformata o troncata chiude
 * la stringa senza leggere oltre il terminatore. */
char *media_json_parse_string(const char *p, const char **end) {
    if (*p != '"') return NULL;
    p++;
    media_buf b = {0};
    while (*p && *p != '"') {
        if (*p != '\\' || !p[1]) {
            media_buf_append(&b, p, 1);
            p++;
            continue;
        }
        p++;
        char c = *p;
        if (c == 'u') {
            long cp;
            if (!media_hex4(p + 1, &cp)) break;   /* troncata: fermati qui */
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                long lo;
                if (media_hex4(p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   /* surrogato spaiato */
            media_buf_put_utf8(&b, cp);
            continue;
        }
        switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            default: break;   /* \" \\ \/ e altri: il carattere stesso */
        }
        media_buf_append(&b, &c, 1);
        p++;
    }
    if (end) *end = *p == '"' ? p + 1 : p;
    return media_buf_take(&b);
}

char *media_json_str(const char *json, const char *key) {
    if (!json) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != ':') continue;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '"') return media_json_parse_string(p, NULL);
    }
    return NULL;
}

long media_json_int(const char *json, const char *key, bool *found) {
    if (found) *found = false;
    if (!json) return 0;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p++ != ':') continue;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '-' || isdigit((unsigned char)*p)) {
            if (found) *found = true;
            return strtol(p, NULL, 10);
        }
    }
    return 0;
}

char *media_url_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    media_buf b = {0};
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            media_buf_append(&b, (const char *)&c, 1);
        } else {
            char e[3] = {'%', hex[c >> 4], hex[c & 15]};
            media_buf_append(&b, e, 3);
        }
    }
    return media_buf_take(&b);
}

char *media_base64_encode(const unsigned char *data, size_t len) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outlen = ((len + 2) / 3) * 4;
    char *out = media_xmalloc(outlen + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len) v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned)data[i + 2];
        out[j++] = tab[(v >> 18) & 63];
        out[j++] = tab[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? tab[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? tab[v & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

char *media_json_quote(const char *s) {
    media_buf b = {0};
    media_buf_puts(&b, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  media_buf_puts(&b, "\\\""); break;
            case '\\': media_buf_puts(&b, "\\\\"); break;
            case '\n': media_buf_puts(&b, "\\n"); break;
            case '\r': media_buf_puts(&b, "\\r"); break;
            case '\t': media_buf_puts(&b, "\\t"); break;
            default:
                if (*p < 0x20) {
                    char e[8];
                    snprintf(e, sizeof(e), "\\u%04x", *p);
                    media_buf_puts(&b, e);
                } else {
                    media_buf_append(&b, (const char *)p, 1);
                }
        }
    }
    media_buf_puts(&b, "\"");
    return media_buf_take(&b);
}
