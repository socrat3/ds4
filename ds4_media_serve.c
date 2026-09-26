/* Server HTTP di ds4_media: espone l'API Immagini di OpenAI, cosi' Open WebUI (o un
 * client curl) genera immagini puntando a un indirizzo fisso, indipendente dal modello
 * di chat attivo. Un thread per connessione, Connection: close. Vedi ds4_media.h.
 *
 *   POST /v1/images/generations  {model,prompt,negative_prompt,n,size,response_format,seed,steps,weights}
 *   GET  /v1/media/files/<nome>  serve un file della cartella di uscita (response_format=url)
 *   GET  /v1/models              elenca qwen-image-2.1
 *   GET  /health                 200 se ComfyUI risponde
 *
 * Se il client chiude la connessione mentre aspetta, il lavoro viene tolto dalla coda
 * di ComfyUI: nessuno lo leggerebbe. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* strcasestr, POLLRDHUP */
#endif
#include "ds4_media.h"
#include "ds4_media_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SERVE_MAX_HEADER (64 * 1024)
#define SERVE_MAX_BODY (1024 * 1024)
#define SERVE_READ_MS 30000
#define SERVE_MAX_CONN 64          /* oltre: 503 subito, un client lento non esaurisce i thread */

typedef struct { int fd; int port; ds4_media *m; } serve_conn;

/* Connessioni in corso: allo stop si aspetta che finiscano (il chiamante poi libera
 * ds4_media, che i thread stanno usando). La cancellazione della config, se c'e',
 * fa terminare presto i lavori lunghi. */
static int serve_active;

static void serve_send(int fd, const char *status, const char *ctype,
                       const void *body, size_t blen) {
    char h[256];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", status, ctype, blen);
    if (media_write_all(fd, h, (size_t)n) == 0 && blen) media_write_all(fd, body, blen);
}

static void serve_error(int fd, const char *status, const char *msg) {
    media_buf b = {0};
    char *q = media_json_quote(msg);
    media_buf_puts(&b, "{\"error\":{\"message\":");
    media_buf_puts(&b, q);
    media_buf_puts(&b, ",\"type\":\"invalid_request_error\"}}");
    free(q);
    serve_send(fd, status, "application/json", b.ptr, b.len);
    free(b.ptr);
}

/* Il client ha chiuso? POLLRDHUP vede la chiusura anche se ha lasciato byte da leggere. */
static bool serve_client_gone(void *privdata) {
    serve_conn *c = privdata;
    struct pollfd pfd = {.fd = c->fd, .events = POLLIN | POLLRDHUP};
    if (poll(&pfd, 1, 0) <= 0) return false;
    if (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR)) return true;
    char b;
    return (pfd.revents & POLLIN) && recv(c->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

/* Base dell'URL assoluto dall'header Host (solo caratteri da host:porta), altrimenti
 * l'indirizzo di ascolto. */
static void serve_base_url(serve_conn *c, const char *headers, char *out, size_t n) {
    const char *h = strcasestr(headers, "\r\nHost:");
    char host[256] = {0};
    if (h) {
        h += 7;
        while (*h == ' ' || *h == '\t') h++;
        size_t k = 0;
        while (h[k] && h[k] != '\r' && k < sizeof(host) - 1) {
            char ch = h[k];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '-' || ch == ':' || ch == '[' || ch == ']')) { k = 0; break; }
            host[k] = ch;
            k++;
        }
        host[k] = '\0';
    }
    if (host[0]) snprintf(out, n, "http://%s", host);
    else snprintf(out, n, "http://127.0.0.1:%d", c->port);
}

/* Risposta b64_json in streaming: la lunghezza si conosce dalle dimensioni dei file,
 * quindi si manda l'header e poi un file alla volta, codificato a blocchi. In memoria
 * c'e' sempre un solo blocco, non tutte le immagini. */
static void serve_b64_stream(int fd, char **files, int n_files, long created) {
    FILE *fp[DS4_MEDIA_MAX_N * 4] = {0};
    long long total = 0;
    char head[64];
    int hl = snprintf(head, sizeof(head), "{\"created\":%ld,\"data\":[", created);
    total += hl;
    int nf = n_files < (int)(sizeof(fp) / sizeof(fp[0])) ? n_files : (int)(sizeof(fp) / sizeof(fp[0]));
    for (int i = 0; i < nf; i++) {
        struct stat st;
        fp[i] = fopen(files[i], "rb");
        if (!fp[i] || fstat(fileno(fp[i]), &st) != 0) {
            for (int k = 0; k <= i; k++) if (fp[k]) fclose(fp[k]);
            serve_error(fd, "500 Internal Server Error", "immagine generata ma non leggibile");
            return;
        }
        total += (i ? 1 : 0) + 13 + ((long long)st.st_size + 2) / 3 * 4 + 2;   /* ,{"b64_json":"...."} */
    }
    total += 2;   /* ]} */
    char h[256];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "Content-Length: %lld\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", total);
    bool ok = media_write_all(fd, h, (size_t)n) == 0 && media_write_all(fd, head, (size_t)hl) == 0;
    unsigned char raw[3 * 16384];
    for (int i = 0; i < nf; i++) {
        if (ok) ok = media_write_all(fd, i ? ",{\"b64_json\":\"" : "{\"b64_json\":\"", i ? 14 : 13) == 0;
        size_t rd;
        /* blocchi multipli di 3: il base64 dei pezzi concatenati e' quello del file */
        while (ok && (rd = fread(raw, 1, sizeof(raw), fp[i])) > 0) {
            char *b64 = media_base64_encode(raw, rd);
            ok = media_write_all(fd, b64, strlen(b64)) == 0;
            free(b64);
        }
        if (ok) ok = media_write_all(fd, "\"}", 2) == 0;
        fclose(fp[i]);
    }
    if (ok) media_write_all(fd, "]}", 2);
}

/* POST /v1/images/generations */
static void serve_generations(serve_conn *c, const char *headers, const char *body) {
    char err[256] = {0};
    char *prompt = media_json_str(body, "prompt");
    if (!prompt || !prompt[0]) { free(prompt); serve_error(c->fd, "400 Bad Request", "prompt mancante"); return; }
    char *negative = media_json_str(body, "negative_prompt");
    char *size = media_json_str(body, "size");
    char *fmt = media_json_str(body, "response_format");
    char *weights = media_json_str(body, "weights");
    bool has_seed = false, has_steps = false, has_n = false;
    long seed = media_json_int(body, "seed", &has_seed);
    long steps = media_json_int(body, "steps", &has_steps);
    long n = media_json_int(body, "n", &has_n);
    if (!has_n || n < 1) n = 1;
    if (n > DS4_MEDIA_MAX_N) n = DS4_MEDIA_MAX_N;

    ds4_media_image_req req = {0};
    req.prompt = prompt;
    req.negative = negative;
    req.seed = has_seed && seed >= 0 ? seed : -1;
    req.steps = has_steps ? (steps < 0 || steps > 1000 ? 1000 : (int)steps) : 0;   /* fuori limite: lo rifiuta il modulo */
    req.n = (int)n;
    req.weights = weights ? (strcmp(weights, "bf16") == 0 ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8)
                          : ds4_media_default_weights(c->m);
    req.cancel = serve_client_gone;
    req.cancel_privdata = c;
    bool bad_size = size && strcmp(size, "auto") != 0 && !ds4_media_parse_size(size, &req.width, &req.height);
    bool bad_fmt = fmt && strcmp(fmt, "url") != 0 && strcmp(fmt, "b64_json") != 0;
    if (bad_size || bad_fmt) {
        serve_error(c->fd, "400 Bad Request", bad_size
                    ? "size non valida: WxH multipli di 32, lato max 2752, area max 2752x1536"
                    : "response_format: url o b64_json");
    } else {
        ds4_media_result res = {0};
        if (!ds4_media_image(c->m, &req, &res, err, sizeof(err))) {
            serve_error(c->fd, res.invalid ? "400 Bad Request" : "502 Bad Gateway", err);
        } else if (fmt && strcmp(fmt, "url") == 0) {
            char base[300];
            serve_base_url(c, headers, base, sizeof(base));
            media_buf out = {0};
            char line[64];
            snprintf(line, sizeof(line), "{\"created\":%ld,\"data\":[", (long)time(NULL));
            media_buf_puts(&out, line);
            for (int k = 0; k < res.n_files; k++) {
                const char *fname = strrchr(res.files[k], '/');
                fname = fname ? fname + 1 : res.files[k];
                media_buf u = {0};
                media_buf_puts(&u, base);
                media_buf_puts(&u, "/v1/media/files/");
                media_buf_puts(&u, fname);
                char *qu = media_json_quote(u.ptr);
                free(u.ptr);
                media_buf_puts(&out, k ? ",{\"url\":" : "{\"url\":");
                media_buf_puts(&out, qu);
                media_buf_puts(&out, "}");
                free(qu);
            }
            media_buf_puts(&out, "]}");
            serve_send(c->fd, "200 OK", "application/json", out.ptr, out.len);
            free(out.ptr);
        } else {
            serve_b64_stream(c->fd, res.files, res.n_files, (long)time(NULL));
        }
        ds4_media_result_free(&res);
    }
    free(prompt); free(negative); free(size); free(fmt); free(weights);
}

/* GET /v1/media/files/<nome>: solo un nome di file della cartella, niente percorsi. */
static void serve_file(serve_conn *c, const char *name) {
    if (!name[0] || name[0] == '.' || strstr(name, "..") || strchr(name, '/')) {
        serve_error(c->fd, "400 Bad Request", "nome non valido");
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ds4_media_dir(c->m), name);
    FILE *fp = fopen(path, "rb");
    struct stat st;
    if (!fp || fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fp) fclose(fp);
        serve_error(c->fd, "404 Not Found", "file non trovato");
        return;
    }
    const char *ext = strrchr(name, '.');
    const char *ct = (ext && strcmp(ext, ".mp4") == 0) ? "video/mp4" : "image/png";
    char h[256];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", ct, (long long)st.st_size);
    bool ok = media_write_all(c->fd, h, (size_t)n) == 0;
    char buf[65536];
    size_t rd;
    while (ok && (rd = fread(buf, 1, sizeof(buf), fp)) > 0) ok = media_write_all(c->fd, buf, rd) == 0;
    fclose(fp);
}

/* Legge header e corpo con un timeout per ogni attesa e limiti di dimensione: un
 * client lento o ostile non tiene un thread per sempre ne' riempie la memoria.
 * Restituisce 0, oppure lo stato HTTP d'errore da mandare (-1 = chiudi e basta). */
static int serve_read_request(int fd, media_buf *raw, size_t *hdr_end) {
    size_t content_len = 0;
    char tmp[8192];
    *hdr_end = 0;
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, SERVE_READ_MS);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return *hdr_end ? 400 : -1;
        media_buf_append(raw, tmp, (size_t)r);
        if (!*hdr_end) {
            char *e = strstr(raw->ptr, "\r\n\r\n");
            if (!e) {
                if (raw->len > SERVE_MAX_HEADER) return 431;
                continue;
            }
            *hdr_end = (size_t)(e - raw->ptr) + 4;
            if (*hdr_end > SERVE_MAX_HEADER) return 431;
            char saved = raw->ptr[*hdr_end];
            raw->ptr[*hdr_end] = '\0';   /* cerca Content-Length solo negli header */
            char *cl = strcasestr(raw->ptr, "\r\nContent-Length:");
            raw->ptr[*hdr_end] = saved;
            if (cl) {
                char *end;
                long v = strtol(cl + 17, &end, 10);
                if (v < 0 || end == cl + 17) return 400;
                if (v > SERVE_MAX_BODY) return 413;
                content_len = (size_t)v;
            }
        }
        if (*hdr_end && raw->len >= *hdr_end + content_len) {
            raw->len = *hdr_end + content_len;   /* byte oltre il corpo dichiarato: ignorati */
            raw->ptr[raw->len] = '\0';
            return 0;
        }
    }
}

static void *serve_client(void *arg) {
    serve_conn *c = arg;
    media_buf raw = {0};
    size_t hdr_end = 0;
    int st = serve_read_request(c->fd, &raw, &hdr_end);
    if (st == 413) serve_error(c->fd, "413 Payload Too Large", "corpo oltre 1 MiB");
    else if (st == 431) serve_error(c->fd, "431 Request Header Fields Too Large", "header oltre 64 KiB");
    else if (st == 400) serve_error(c->fd, "400 Bad Request", "richiesta incompleta");
    else if (st == 0) {
        const char *body = raw.ptr + hdr_end;
        char *headers = media_xmalloc(hdr_end + 1);
        memcpy(headers, raw.ptr, hdr_end);
        headers[hdr_end] = '\0';
        if (strncmp(raw.ptr, "OPTIONS ", 8) == 0) {
            serve_send(c->fd, "204 No Content", "text/plain", "", 0);
        } else if (strncmp(raw.ptr, "POST /v1/images/generations ", 28) == 0) {
            serve_generations(c, headers, body);
        } else if (strncmp(raw.ptr, "GET /v1/media/files/", 20) == 0) {
            char name[512] = {0};
            sscanf(raw.ptr + 20, "%511[^ ?]", name);
            serve_file(c, name);
        } else if (strncmp(raw.ptr, "GET /v1/models ", 15) == 0) {
            const char *b = "{\"object\":\"list\",\"data\":[{\"id\":\"qwen-image-2.1\",\"object\":\"model\",\"owned_by\":\"ds4-media\"}]}";
            serve_send(c->fd, "200 OK", "application/json", b, strlen(b));
        } else if (strncmp(raw.ptr, "GET /health ", 12) == 0) {
            char err[128];
            bool ok = ds4_media_health(c->m, err, sizeof(err));
            serve_send(c->fd, ok ? "200 OK" : "502 Bad Gateway", "application/json",
                       ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
        } else {
            serve_error(c->fd, "404 Not Found", "rotta sconosciuta");
        }
        free(headers);
    }
    free(raw.ptr);
    close(c->fd);
    free(c);
    __sync_fetch_and_sub(&serve_active, 1);
    return NULL;
}

bool ds4_media_serve(ds4_media *m, int port, volatile int *stop, char *err, size_t err_len) {
    signal(SIGPIPE, SIG_IGN);
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { media_set_err(err, err_len, "socket: %s", strerror(errno)); return false; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 16) != 0) {
        media_set_err(err, err_len, "bind :%d: %s", port, strerror(errno));
        close(srv);
        return false;
    }
    fprintf(stderr, "ds4: media serve su http://127.0.0.1:%d/v1 (immagini)\n", port);
    while (!stop || !*stop) {
        struct pollfd pfd = {.fd = srv, .events = POLLIN};
        int rc = poll(&pfd, 1, 500);
        ds4_media_idle_tick(m);   /* --idle-free: /free dopo N s senza lavori */
        if (rc <= 0) continue;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        if (__sync_fetch_and_add(&serve_active, 0) >= SERVE_MAX_CONN) {
            serve_error(fd, "503 Service Unavailable", "troppe connessioni");
            close(fd);
            continue;
        }
        serve_conn *c = media_xmalloc(sizeof(*c));
        c->fd = fd;
        c->port = port;
        c->m = m;
        pthread_t th;
        __sync_fetch_and_add(&serve_active, 1);
        if (pthread_create(&th, NULL, serve_client, c) != 0) {
            __sync_fetch_and_sub(&serve_active, 1);
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(th);
    }
    close(srv);
    for (int i = 0; i < 600 && __sync_fetch_and_add(&serve_active, 0) > 0; i++) {   /* fino a 60 s */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&ts, NULL);
    }
    if (serve_active > 0) {
        media_set_err(err, err_len, "chiuso con %d connessioni ancora attive: non liberare ds4_media", serve_active);
        return false;
    }
    return true;
}
