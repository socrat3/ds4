/* Finto ComfyUI per i test di ds4_media, un thread per connessione: /prompt, /history,
 * /view (PNG con IHDR vero), /upload/image, /queue, /interrupt, /object_info,
 * /system_stats, /free e il websocket /ws con eventi, ping e messaggi frammentati.
 * Registra quello che riceve, cosi' i test controllano grafo, upload, cancellazioni.
 * Solo per i test: funzioni static in un header, incluso da un solo file. */
#ifndef MEDIA_FAKE_H
#define MEDIA_FAKE_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { WS_NONE = 0, WS_EVENTS = 1, WS_SILENT = 2, WS_CLOSE = 3 };

typedef struct {
    int port, srv;
    int ws_mode;               /* WS_* */
    int hist_pending;          /* /history vuoto per N richieste; -1 = per sempre */
    int n_outputs;             /* file nell'esito */
    int view_w, view_h;        /* IHDR del PNG servito da /view */
    long torch_bytes;          /* torch_vram_total in /system_stats; 0 = assente */
    bool cache_node;           /* /object_info/QwenImage21Cache risponde 200 */
    pthread_mutex_t mu;
    char graph[65536];         /* ultimo corpo di POST /prompt */
    char client_id[128];
    char upload_names[16][160];
    int uploads, queue_deletes, interrupts, prompts, history_gets, got_pong;
    char last_delete[256];
    int prompt_seq;
    volatile int posted;       /* un /prompt e' arrivato: il ws puo' mandare gli eventi */
} fake_comfy;

static fake_comfy *g_fk;

static void fk_send(int fd, const char *status, const char *ctype, const void *b, size_t n) {
    char h[256];
    int hl = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n", status, ctype, n);
    if (write(fd, h, (size_t)hl) < 0) return;
    if (n && write(fd, b, n) < 0) return;
}

/* Legge header e corpo (Content-Length). Restituisce il buffer (malloc'd) e *blen. */
static char *fk_read(int fd, size_t *total) {
    size_t cap = 1 << 16, len = 0, need = 0, hend = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 5000) <= 0) break;
        if (len == cap) { cap *= 2; b = realloc(b, cap + 1); }
        ssize_t r = read(fd, b + len, cap - len);
        if (r <= 0) break;
        len += (size_t)r;
        b[len] = '\0';
        if (!hend) {
            char *e = strstr(b, "\r\n\r\n");
            if (!e) continue;
            hend = (size_t)(e - b) + 4;
            char *cl = strcasestr(b, "Content-Length:");
            need = hend + (cl && cl < e ? (size_t)strtol(cl + 15, NULL, 10) : 0);
        }
        if (len >= need) break;
    }
    b[len] = '\0';
    *total = len;
    return b;
}

static void fk_png(unsigned char *p, int w, int h) {
    static const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    memcpy(p, sig, 16);
    p[16] = (unsigned char)(w >> 24); p[17] = (unsigned char)(w >> 16); p[18] = (unsigned char)(w >> 8); p[19] = (unsigned char)w;
    p[20] = (unsigned char)(h >> 24); p[21] = (unsigned char)(h >> 16); p[22] = (unsigned char)(h >> 8); p[23] = (unsigned char)h;
    for (int i = 24; i < 64; i++) p[i] = (unsigned char)(i * 7);
}

static void fk_ws_frame(int fd, int op, bool fin, const void *d, size_t n) {
    unsigned char h[10];
    size_t hl = 2;
    h[0] = (unsigned char)((fin ? 0x80 : 0) | op);
    if (n < 126) h[1] = (unsigned char)n;
    else { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hl = 4; }
    if (write(fd, h, hl) < 0 || (n && write(fd, d, n) < 0)) return;
}

static void fk_ws_text(int fd, const char *s) { fk_ws_frame(fd, 1, true, s, strlen(s)); }

static void fk_websocket(fake_comfy *f, int fd, const char *req) {
    const char *cid = strstr(req, "clientId=");
    pthread_mutex_lock(&f->mu);
    if (cid) sscanf(cid + 9, "%127[^ &]", f->client_id);
    pthread_mutex_unlock(&f->mu);
    const char *ok = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: x\r\n\r\n";
    if (write(fd, ok, strlen(ok)) < 0) return;
    if (f->ws_mode == WS_CLOSE) { unsigned char c[4] = {0x88, 2, 0x03, 0xe8}; if (write(fd, c, 4) < 0) {} return; }
    for (int i = 0; i < 500 && !f->posted; i++) usleep(10000);
    if (f->ws_mode == WS_SILENT) { usleep(20000000); return; }
    char pid[32], m[512];
    snprintf(pid, sizeof(pid), "fakepid%d", f->prompt_seq);
    snprintf(m, sizeof(m), "{\"type\":\"status\",\"data\":{\"status\":{}}}");
    fk_ws_text(fd, m);
    snprintf(m, sizeof(m), "{\"type\":\"progress\",\"data\":{\"value\":9,\"max\":9,\"prompt_id\":\"altro\"}}");
    fk_ws_text(fd, m);   /* di un altro lavoro: va ignorato */
    snprintf(m, sizeof(m), "{\"type\": \"execution_start\", \"data\": {\"prompt_id\": \"%s\"}}", pid);
    fk_ws_text(fd, m);
    fk_ws_frame(fd, 9, true, "hi", 2);   /* ping: il client deve rispondere pong */
    unsigned char bin[8] = {0, 0, 0, 1, 0, 0, 0, 2};
    fk_ws_frame(fd, 2, true, bin, sizeof(bin));   /* anteprima binaria */
    for (int s = 1; s <= 3; s++) {
        snprintf(m, sizeof(m), "{\"type\":\"progress\",\"data\":{\"value\":%d,\"max\":3,\"prompt_id\":\"%s\",\"node\":\"6\"}}", s, pid);
        if (s == 2) {   /* frammentato in tre pezzi */
            size_t n = strlen(m), a = n / 3;
            fk_ws_frame(fd, 1, false, m, a);
            fk_ws_frame(fd, 0, false, m + a, a);
            fk_ws_frame(fd, 0, true, m + 2 * a, n - 2 * a);
        } else {
            fk_ws_text(fd, m);
        }
    }
    /* il pong del client */
    struct pollfd p = {.fd = fd, .events = POLLIN};
    unsigned char in[64];
    if (poll(&p, 1, 2000) > 0 && read(fd, in, sizeof(in)) >= 6 && (in[0] & 0x0f) == 0xA && (in[1] & 0x80)) {
        pthread_mutex_lock(&f->mu);
        f->got_pong++;
        pthread_mutex_unlock(&f->mu);
    }
    snprintf(m, sizeof(m), "{\"type\":\"executing\",\"data\":{\"node\":null,\"prompt_id\":\"%s\"}}", pid);
    fk_ws_text(fd, m);
    usleep(200000);
}

static void *fk_conn(void *arg) {
    int fd = (int)(long)arg;
    fake_comfy *f = g_fk;
    size_t n = 0;
    char *req = fk_read(fd, &n);
    char *body = strstr(req, "\r\n\r\n");
    body = body ? body + 4 : req + n;
    if (!strncmp(req, "GET /ws?", 8)) {
        if (f->ws_mode == WS_NONE) fk_send(fd, "404 Not Found", "text/plain", "no", 2);
        else fk_websocket(f, fd, req);
    } else if (!strncmp(req, "POST /prompt", 12)) {
        pthread_mutex_lock(&f->mu);
        snprintf(f->graph, sizeof(f->graph), "%s", body);
        f->prompts++;
        f->prompt_seq++;
        char b[128];
        snprintf(b, sizeof(b), "{\"prompt_id\":\"fakepid%d\",\"number\":1,\"node_errors\":{}}", f->prompt_seq);
        pthread_mutex_unlock(&f->mu);
        f->posted = 1;
        fk_send(fd, "200 OK", "application/json", b, strlen(b));
    } else if (!strncmp(req, "GET /history/", 13)) {
        pthread_mutex_lock(&f->mu);
        f->history_gets++;
        bool pending = f->hist_pending < 0 || (f->hist_pending > 0 && f->hist_pending--);
        char b[4096];
        int bl = snprintf(b, sizeof(b), "{\"fakepid%d\":{\"prompt\":[1,\"x\",{\"8\":{\"inputs\":{\"filename_prefix\":\"ds4/x\"}}}],"
                          "\"outputs\":{\"8\":{\"images\":[", f->prompt_seq);
        for (int i = 0; i < f->n_outputs; i++)
            bl += snprintf(b + bl, sizeof(b) - (size_t)bl, "%s{\"filename\":\"out_%05d_.png\",\"subfolder\":\"ds4\",\"type\":\"output\"}",
                           i ? "," : "", i);
        snprintf(b + bl, sizeof(b) - (size_t)bl, "]}},\"status\":{\"status_str\":\"success\",\"completed\":true,\"messages\":[]}}}");
        pthread_mutex_unlock(&f->mu);
        if (pending) fk_send(fd, "200 OK", "application/json", "{}", 2);
        else fk_send(fd, "200 OK", "application/json", b, strlen(b));
    } else if (!strncmp(req, "GET /view", 9)) {
        unsigned char png[64];
        fk_png(png, f->view_w, f->view_h);
        fk_send(fd, "200 OK", "image/png", png, sizeof(png));
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
        fk_send(fd, "200 OK", "application/json", b, strlen(b));
    } else if (!strncmp(req, "POST /queue", 11)) {
        pthread_mutex_lock(&f->mu);
        f->queue_deletes++;
        snprintf(f->last_delete, sizeof(f->last_delete), "%s", body);
        pthread_mutex_unlock(&f->mu);
        fk_send(fd, "200 OK", "text/plain", "", 0);
    } else if (!strncmp(req, "POST /interrupt", 15)) {
        __sync_fetch_and_add(&f->interrupts, 1);
        fk_send(fd, "200 OK", "text/plain", "", 0);
    } else if (!strncmp(req, "GET /object_info/QwenImage21Cache", 33)) {
        if (f->cache_node) fk_send(fd, "200 OK", "application/json", "{\"QwenImage21Cache\":{}}", 23);
        else fk_send(fd, "404 Not Found", "text/plain", "no", 2);
    } else if (!strncmp(req, "GET /system_stats", 17)) {
        char b[256];
        if (f->torch_bytes > 0)
            snprintf(b, sizeof(b), "{\"system\":{},\"devices\":[{\"torch_vram_total\": %ld}]}", f->torch_bytes);
        else
            snprintf(b, sizeof(b), "{\"system\":{}}");
        fk_send(fd, "200 OK", "application/json", b, strlen(b));
    } else if (!strncmp(req, "POST /free", 10)) {
        fk_send(fd, "200 OK", "application/json", "{}", 2);
    } else {
        fk_send(fd, "404 Not Found", "text/plain", "no", 2);
    }
    free(req);
    close(fd);
    return NULL;
}

static void *fk_accept(void *arg) {
    fake_comfy *f = arg;
    for (;;) {
        int fd = accept(f->srv, NULL, NULL);
        if (fd < 0) break;
        pthread_t th;
        pthread_create(&th, NULL, fk_conn, (void *)(long)fd);
        pthread_detach(th);
    }
    return NULL;
}

static void fk_start(fake_comfy *f) {
    memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->mu, NULL);
    f->n_outputs = 1;
    f->view_w = f->view_h = 1024;
    f->srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(f->srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(f->srv, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(f->srv, (struct sockaddr *)&a, &al);
    f->port = ntohs(a.sin_port);
    listen(f->srv, 64);
    g_fk = f;
    pthread_t th;
    pthread_create(&th, NULL, fk_accept, f);
    pthread_detach(th);
}

/* Riporta il finto allo stato iniziale tra un test e l'altro (la porta resta). */
static void fk_reset(fake_comfy *f) {
    pthread_mutex_lock(&f->mu);
    f->ws_mode = WS_EVENTS;
    f->hist_pending = 0;
    f->n_outputs = 1;
    f->view_w = f->view_h = 1024;
    f->torch_bytes = 0;
    f->cache_node = false;
    f->graph[0] = f->client_id[0] = f->last_delete[0] = '\0';
    f->uploads = f->queue_deletes = f->interrupts = f->prompts = f->history_gets = f->got_pong = 0;
    f->posted = 0;
    pthread_mutex_unlock(&f->mu);
}

#endif
