/* Finto ComfyUI OSTILE per i test avversari di ds4_media (tests/test_media_adv.c).
 * A differenza di media_fake.h ogni risposta e' programmabile: prompt_id arbitrario,
 * corpo di /history grezzo, /view con Content-Length bugiardo o vuoto, risposte
 * chunked, websocket a byte grezzi (frame malformati, handshake strani, frame
 * incollati all'handshake, ping flood). Registra tutto quello che riceve.
 * Solo per i test: funzioni static in un header, incluso da un solo file. */
#ifndef MEDIA_ADV_FAKE_H
#define MEDIA_ADV_FAKE_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { AW_NONE = 0, AW_SCRIPT = 1, AW_SILENT = 2, AW_PINGFLOOD = 3 };
enum { AW_AFTER_HOLD = 0, AW_AFTER_CLOSE = 1 };

typedef struct {
    int port, srv;
    pthread_mutex_t mu;
    /* risposte programmabili */
    char pid[160];                 /* prompt_id in /prompt e nella history generata */
    char prompt_body[512];         /* se non vuoto: corpo grezzo di /prompt */
    int prompt_status;             /* default 200 */
    int prompt_delay_ms;           /* ritardo prima di rispondere a /prompt */
    char history[32768];           /* se non vuoto: corpo grezzo di /history */
    int hist_pending;              /* {} per N richieste; -1 per sempre */
    int hist_status;               /* default 200 */
    bool hist_chunked;             /* /history in Transfer-Encoding: chunked */
    int n_outputs;                 /* immagini nella history generata */
    int view_status;               /* default 200 */
    unsigned char view_body[300000];
    size_t view_len;               /* byte davvero inviati da /view */
    long view_cl;                  /* Content-Length dichiarato (-1 = view_len) */
    char stats[512];               /* corpo di /system_stats */
    char object_info[256];         /* corpo 200 di /object_info/QwenImage21Cache; vuoto = 404 */
    int upload_status;             /* default 200 */
    /* websocket */
    int ws_mode;                   /* AW_* */
    char ws_handshake[512];        /* vuoto = 101 standard */
    unsigned char ws_raw[65536];   /* byte da mandare dopo l'handshake (AW_SCRIPT) */
    size_t ws_raw_len;
    size_t ws_chunk;               /* 0 = tutto insieme, altrimenti pezzi di N byte */
    int ws_chunk_us;               /* pausa tra i pezzi */
    bool ws_glue;                  /* manda i byte insieme all'handshake, senza aspettare /prompt */
    int ws_after;                  /* AW_AFTER_* */
    volatile int ws_stop;          /* ferma il ping flood / l'attesa */
    /* registrazioni */
    char graph[65536];
    char upload_names[16][160];
    char last_delete[256], last_interrupt[256], last_view[2048];
    int uploads, prompts, history_gets, view_gets, view_malformed, queue_deletes, interrupts,
        ws_opens, got_pong, frees, stats_gets;
    volatile int posted;
} adv_fake;

static adv_fake *g_af;

static long af_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void af_write(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
        p += r;
        n -= (size_t)r;
    }
}

static void af_send_cl(int fd, int status, const char *ctype, const void *b, size_t n, long cl) {
    char h[256];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                      "Connection: close\r\n\r\n", status, ctype, cl < 0 ? (long)n : cl);
    af_write(fd, h, (size_t)hl);
    if (n) af_write(fd, b, n);
}
static void af_send(int fd, int status, const char *ctype, const void *b, size_t n) {
    af_send_cl(fd, status, ctype, b, n, -1);
}

/* Risposta chunked: il corpo in pezzi di 7 byte, con un'estensione di chunk. */
static void af_send_chunked(int fd, int status, const char *b, size_t n) {
    char h[256];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n", status);
    af_write(fd, h, (size_t)hl);
    for (size_t i = 0; i < n; i += 7) {
        size_t k = n - i < 7 ? n - i : 7;
        char c[32];
        int cl = snprintf(c, sizeof(c), "%zx;ext=1\r\n", k);
        af_write(fd, c, (size_t)cl);
        af_write(fd, b + i, k);
        af_write(fd, "\r\n", 2);
    }
    af_write(fd, "0\r\n\r\n", 5);
}

/* Legge header (+ corpo da Content-Length) entro 1,5 s. *complete = fine header vista. */
static char *af_read(int fd, size_t *total, bool *complete) {
    size_t cap = 1 << 16, len = 0, need = 0, hend = 0;
    char *b = malloc(cap + 1);
    long deadline = af_now_ms() + 1500;
    *complete = false;
    for (;;) {
        long left = deadline - af_now_ms();
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        if (len == cap) { cap *= 2; b = realloc(b, cap + 1); }
        ssize_t r = read(fd, b + len, cap - len);
        if (r <= 0) break;
        len += (size_t)r;
        b[len] = '\0';
        if (!hend) {
            char *e = strstr(b, "\r\n\r\n");
            if (!e) continue;
            hend = (size_t)(e - b) + 4;
            *complete = true;
            char *cl = strcasestr(b, "Content-Length:");
            need = hend + (cl && cl < e ? (size_t)strtol(cl + 15, NULL, 10) : 0);
        }
        if (len >= need) break;
    }
    b[len] = '\0';
    *total = len;
    return b;
}

static void af_png(unsigned char *p, int w, int h) {
    static const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    memcpy(p, sig, 16);
    p[16] = (unsigned char)(w >> 24); p[17] = (unsigned char)(w >> 16); p[18] = (unsigned char)(w >> 8); p[19] = (unsigned char)w;
    p[20] = (unsigned char)(h >> 24); p[21] = (unsigned char)(h >> 16); p[22] = (unsigned char)(h >> 8); p[23] = (unsigned char)h;
    for (int i = 24; i < 64; i++) p[i] = (unsigned char)(i * 7);
}

/* Aggiunge un frame websocket a buf. len_mode: 0 minimo, 2 forza 16 bit, 8 forza 64 bit.
 * masked: frame mascherato (vietato dal server, ma il client deve reggerlo). rsv: bit RSV. */
static void af_frame(unsigned char *buf, size_t *len, int op, bool fin, const void *d, size_t n,
                     bool masked, int len_mode, int rsv) {
    unsigned char *p = buf + *len;
    size_t hl = 2;
    p[0] = (unsigned char)((fin ? 0x80 : 0) | (rsv << 4) | op);
    if (len_mode == 8 || n > 65535) {
        p[1] = 127;
        for (int i = 0; i < 8; i++) p[2 + i] = (unsigned char)((unsigned long long)n >> (56 - 8 * i));
        hl = 10;
    } else if (len_mode == 2 || n >= 126) {
        p[1] = 126; p[2] = (unsigned char)(n >> 8); p[3] = (unsigned char)n; hl = 4;
    } else {
        p[1] = (unsigned char)n;
    }
    unsigned char key[4] = {0x12, 0x34, 0x56, 0x78};
    if (masked) { p[1] |= 0x80; memcpy(p + hl, key, 4); hl += 4; }
    for (size_t i = 0; i < n; i++) p[hl + i] = ((const unsigned char *)d)[i] ^ (masked ? key[i & 3] : 0);
    *len += hl + n;
}

/* Frame di testo con un evento JSON di ComfyUI per il pid del finto (o `pid` esplicito). */
static void af_event(adv_fake *f, const char *type, const char *pid, const char *extra) {
    char m[1024];
    snprintf(m, sizeof(m), "{\"type\":\"%s\",\"data\":{%s%s\"prompt_id\":\"%s\"}}", type,
             extra ? extra : "", extra && extra[0] ? "," : "", pid ? pid : f->pid);
    af_frame(f->ws_raw, &f->ws_raw_len, 1, true, m, strlen(m), false, 0, 0);
}
static void af_done_event(adv_fake *f) { af_event(f, "executing", NULL, "\"node\":null"); }

/* Attende sul socket ws leggendo cio' che il client manda (conta i pong mascherati)
 * finche' il client chiude, ws_stop, o max_ms. */
static void af_ws_hold(adv_fake *f, int fd, long max_ms) {
    long deadline = af_now_ms() + max_ms;
    unsigned char in[4096];
    while (!f->ws_stop && af_now_ms() < deadline) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 50) <= 0) continue;
        ssize_t r = read(fd, in, sizeof(in));
        if (r <= 0) return;
        for (ssize_t i = 0; i + 1 < r; i++)
            if ((in[i] & 0x0f) == 0xA && (in[i + 1] & 0x80)) { __sync_fetch_and_add(&f->got_pong, 1); i += 5; }
    }
}

static void af_websocket(adv_fake *f, int fd) {
    __sync_fetch_and_add(&f->ws_opens, 1);
    const char *hs = f->ws_handshake[0] ? f->ws_handshake
        : "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: x\r\n\r\n";
    if (f->ws_mode == AW_SCRIPT && f->ws_glue) {
        unsigned char all[70000];
        size_t hl = strlen(hs);
        memcpy(all, hs, hl);
        memcpy(all + hl, f->ws_raw, f->ws_raw_len);
        af_write(fd, all, hl + f->ws_raw_len);
    } else {
        af_write(fd, hs, strlen(hs));
    }
    if (f->ws_mode == AW_SILENT) { af_ws_hold(f, fd, 30000); return; }
    if (!f->ws_glue) for (int i = 0; i < 500 && !f->posted; i++) usleep(10000);
    if (f->ws_mode == AW_PINGFLOOD) {
        /* ping a raffica senza mai leggere i pong: scrittura non bloccante, finche' ws_stop */
        unsigned char ping[16384];
        for (size_t i = 0; i < sizeof(ping); i += 2) { ping[i] = 0x89; ping[i + 1] = 0x00; }
        long deadline = af_now_ms() + 15000;
        while (!f->ws_stop && af_now_ms() < deadline) {
            ssize_t r = send(fd, ping, sizeof(ping), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { usleep(1000); continue; }
            if (r <= 0) break;
        }
        return;
    }
    if (f->ws_mode == AW_SCRIPT && !f->ws_glue) {
        if (f->ws_chunk == 0) af_write(fd, f->ws_raw, f->ws_raw_len);
        else for (size_t i = 0; i < f->ws_raw_len; i += f->ws_chunk) {
            size_t k = f->ws_raw_len - i < f->ws_chunk ? f->ws_raw_len - i : f->ws_chunk;
            af_write(fd, f->ws_raw + i, k);
            if (f->ws_chunk_us) usleep((useconds_t)f->ws_chunk_us);
        }
    }
    if (f->ws_after == AW_AFTER_HOLD) af_ws_hold(f, fd, 30000);
}

static void af_history_body(adv_fake *f, char *b, size_t n) {
    int bl = snprintf(b, n, "{\"%s\":{\"prompt\":[1,\"x\",{}],\"outputs\":{\"8\":{\"images\":[", f->pid);
    for (int i = 0; i < f->n_outputs; i++)
        bl += snprintf(b + bl, n - (size_t)bl, "%s{\"filename\":\"out_%05d_.png\",\"subfolder\":\"ds4\",\"type\":\"output\"}",
                       i ? "," : "", i);
    snprintf(b + bl, n - (size_t)bl, "]}},\"status\":{\"status_str\":\"success\",\"completed\":true,\"messages\":[]}}}");
}

static void *af_conn(void *arg) {
    int fd = (int)(long)arg;
    adv_fake *f = g_af;
    size_t n = 0;
    bool complete = false;
    char *req = af_read(fd, &n, &complete);
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req + n;
    if (!complete) {
        if (!strncmp(req, "GET /view", 9)) __sync_fetch_and_add(&f->view_malformed, 1);
        af_send(fd, 400, "text/plain", "malformed", 9);
    } else if (!strncmp(req, "GET /ws?", 8)) {
        if (f->ws_mode == AW_NONE) af_send(fd, 404, "text/plain", "no", 2);
        else af_websocket(f, fd);
    } else if (!strncmp(req, "POST /prompt", 12)) {
        pthread_mutex_lock(&f->mu);
        snprintf(f->graph, sizeof(f->graph), "%s", body);
        f->prompts++;
        char b[1024];
        if (f->prompt_body[0]) snprintf(b, sizeof(b), "%s", f->prompt_body);
        else snprintf(b, sizeof(b), "{\"prompt_id\":\"%s\",\"number\":1,\"node_errors\":{}}", f->pid);
        int st = f->prompt_status, delay = f->prompt_delay_ms;
        pthread_mutex_unlock(&f->mu);
        if (delay) usleep((useconds_t)delay * 1000);
        f->posted = 1;
        af_send(fd, st, "application/json", b, strlen(b));
    } else if (!strncmp(req, "GET /history/", 13)) {
        pthread_mutex_lock(&f->mu);
        f->history_gets++;
        bool pending = f->hist_pending < 0 || (f->hist_pending > 0 && f->hist_pending--);
        char b[32768];
        if (f->history[0]) snprintf(b, sizeof(b), "%s", f->history);
        else af_history_body(f, b, sizeof(b));
        int st = f->hist_status;
        bool chunked = f->hist_chunked;
        pthread_mutex_unlock(&f->mu);
        if (pending) af_send(fd, 200, "application/json", "{}", 2);
        else if (chunked) af_send_chunked(fd, st, b, strlen(b));
        else af_send(fd, st, "application/json", b, strlen(b));
    } else if (!strncmp(req, "GET /view", 9)) {
        pthread_mutex_lock(&f->mu);
        f->view_gets++;
        sscanf(req + 4, "%2047[^ ]", f->last_view);
        pthread_mutex_unlock(&f->mu);
        af_send_cl(fd, f->view_status, "image/png", f->view_body, f->view_len, f->view_cl);
    } else if (!strncmp(req, "POST /upload/image", 18)) {
        char name[160] = {0};
        const char *fn = strstr(req, "filename=\"");
        if (fn) sscanf(fn + 10, "%159[^\"]", name);
        pthread_mutex_lock(&f->mu);
        if (f->uploads < 16) snprintf(f->upload_names[f->uploads], 160, "%s", name);
        f->uploads++;
        pthread_mutex_unlock(&f->mu);
        char b[256];
        snprintf(b, sizeof(b), "{\"name\":\"%s\",\"subfolder\":\"ds4\",\"type\":\"input\"}", name);
        af_send(fd, f->upload_status, "application/json", b, strlen(b));
    } else if (!strncmp(req, "POST /queue", 11)) {
        pthread_mutex_lock(&f->mu);
        f->queue_deletes++;
        snprintf(f->last_delete, sizeof(f->last_delete), "%s", body);
        pthread_mutex_unlock(&f->mu);
        af_send(fd, 200, "text/plain", "", 0);
    } else if (!strncmp(req, "POST /interrupt", 15)) {
        pthread_mutex_lock(&f->mu);
        f->interrupts++;
        snprintf(f->last_interrupt, sizeof(f->last_interrupt), "%s", body);
        pthread_mutex_unlock(&f->mu);
        af_send(fd, 200, "text/plain", "", 0);
    } else if (!strncmp(req, "GET /object_info/QwenImage21Cache", 33)) {
        if (f->object_info[0]) af_send(fd, 200, "application/json", f->object_info, strlen(f->object_info));
        else af_send(fd, 404, "text/plain", "no", 2);
    } else if (!strncmp(req, "GET /system_stats", 17)) {
        __sync_fetch_and_add(&f->stats_gets, 1);
        af_send(fd, 200, "application/json", f->stats, strlen(f->stats));
    } else if (!strncmp(req, "POST /free", 10)) {
        __sync_fetch_and_add(&f->frees, 1);
        af_send(fd, 200, "application/json", "{}", 2);
    } else {
        af_send(fd, 404, "text/plain", "no", 2);
    }
    free(req);
    close(fd);
    return NULL;
}

static void *af_accept(void *arg) {
    adv_fake *f = arg;
    for (;;) {
        int fd = accept(f->srv, NULL, NULL);
        if (fd < 0) break;
        pthread_t th;
        pthread_create(&th, NULL, af_conn, (void *)(long)fd);
        pthread_detach(th);
    }
    return NULL;
}

/* Stato iniziale (la porta resta): ws con l'evento di fine, una uscita PNG 1024x1024. */
static void af_reset(adv_fake *f) {
    pthread_mutex_lock(&f->mu);
    snprintf(f->pid, sizeof(f->pid), "advpid");
    f->prompt_body[0] = f->history[0] = f->ws_handshake[0] = f->object_info[0] = '\0';
    f->prompt_status = f->hist_status = f->view_status = f->upload_status = 200;
    f->prompt_delay_ms = 0;
    f->hist_pending = 0;
    f->hist_chunked = false;
    f->n_outputs = 1;
    af_png(f->view_body, 1024, 1024);
    f->view_len = 64;
    f->view_cl = -1;
    snprintf(f->stats, sizeof(f->stats), "{\"system\":{}}");
    f->ws_mode = AW_SCRIPT;
    f->ws_raw_len = 0;
    f->ws_chunk = 0;
    f->ws_chunk_us = 0;
    f->ws_glue = false;
    f->ws_after = AW_AFTER_HOLD;
    f->ws_stop = 0;
    af_done_event(f);
    f->graph[0] = f->last_delete[0] = f->last_interrupt[0] = f->last_view[0] = '\0';
    f->uploads = f->prompts = f->history_gets = f->view_gets = f->view_malformed = 0;
    f->queue_deletes = f->interrupts = f->ws_opens = f->got_pong = f->frees = f->stats_gets = 0;
    f->posted = 0;
    pthread_mutex_unlock(&f->mu);
}

static void af_start(adv_fake *f) {
    memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->mu, NULL);
    f->srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(f->srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(f->srv, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(f->srv, (struct sockaddr *)&a, &al);
    f->port = ntohs(a.sin_port);
    listen(f->srv, 128);
    g_af = f;
    af_reset(f);
    pthread_t th;
    pthread_create(&th, NULL, af_accept, f);
    pthread_detach(th);
}

/* ── client grezzo per il serve ───────────────────────────────────────────── */

/* Una porta libera sul loopback (bind a 0, poi chiusa). */
static int af_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int af_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

/* Manda `req` (n byte, binario) e legge tutta la risposta entro max_ms (malloc'd, *len). */
static char *af_raw(int port, const void *req, size_t n, int max_ms, size_t *len) {
    int fd = af_connect(port);
    *len = 0;
    if (fd < 0) return NULL;
    af_write(fd, req, n);
    size_t cap = 1 << 16, got = 0;
    char *b = malloc(cap + 1);
    long deadline = af_now_ms() + max_ms;
    for (;;) {
        long left = deadline - af_now_ms();
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
        if (got == cap) { cap *= 2; b = realloc(b, cap + 1); }
        ssize_t r = read(fd, b + got, cap - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    b[got] = '\0';
    close(fd);
    *len = got;
    return b;
}

static char *af_rawstr(int port, const char *req, size_t *len) { return af_raw(port, req, strlen(req), 15000, len); }

static int af_status(const char *resp) { return resp && strlen(resp) >= 12 ? (int)strtol(resp + 9, NULL, 10) : -1; }

#endif
