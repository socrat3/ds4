/* Server HTTP di ds4_media: espone l'API Immagini di OpenAI, cosi' Open WebUI (o un
 * client curl) genera immagini puntando a un indirizzo fisso, indipendente dal modello
 * di chat attivo. Un thread per connessione, Connection: close. Vedi ds4_media.h.
 *
 *   POST /v1/images/generations  {model,prompt,n,size,response_format,seed,steps,weights}
 *   GET  /v1/media/files/<nome>  serve un PNG della cartella di uscita (response_format=url)
 *   GET  /v1/models              elenca qwen-image-2.1
 *   GET  /health                 200 se ComfyUI risponde */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* strcasestr */
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
#include <unistd.h>

typedef struct { int fd; ds4_media *m; } serve_conn;

static void serve_send(int fd, const char *status, const char *ctype,
                       const void *body, size_t blen) {
    media_buf h = {0};
    char line[256];
    snprintf(line, sizeof(line),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", status, ctype, blen);
    media_buf_puts(&h, line);
    media_buf_append(&h, (const char *)body, blen);
    (void)!write(fd, h.ptr, h.len);
    free(h.ptr);
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

static char *serve_read_file(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    if (n < 0) { fclose(fp); return NULL; }
    char *buf = media_xmalloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    buf[rd] = '\0';
    if (len) *len = rd;
    return buf;
}

/* POST /v1/images/generations */
static void serve_generations(serve_conn *c, const char *body) {
    char err[256] = {0};
    char *prompt = media_json_str(body, "prompt");
    if (!prompt || !prompt[0]) { free(prompt); serve_error(c->fd, "400 Bad Request", "prompt mancante"); return; }
    char *size = media_json_str(body, "size");
    char *fmt = media_json_str(body, "response_format");
    char *weights = media_json_str(body, "weights");
    bool has_seed = false, has_steps = false, has_n = false;
    long seed = media_json_int(body, "seed", &has_seed);
    long steps = media_json_int(body, "steps", &has_steps);
    long n = media_json_int(body, "n", &has_n);
    if (!has_n || n < 1) n = 1;
    if (n > 4) n = 4;

    ds4_media_image_req req = {0};
    req.prompt = prompt;
    req.seed = has_seed ? seed : -1;
    req.steps = has_steps ? (int)steps : 0;
    req.weights = (weights && strcmp(weights, "bf16") == 0) ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8;
    if (size && !ds4_media_parse_size(size, &req.width, &req.height)) {
        free(prompt); free(size); free(fmt); free(weights);
        serve_error(c->fd, "400 Bad Request", "size non valida (usa WxH multipli di 32)");
        return;
    }

    /* costruzione della risposta OpenAI; n immagini = n generazioni con seed diverso */
    media_buf out = {0};
    media_buf_puts(&out, "{\"created\":0,\"data\":[");
    bool want_url = fmt && strcmp(fmt, "url") == 0;
    bool ok_any = false;
    for (long i = 0; i < n; i++) {
        ds4_media_image_req r = req;
        if (req.seed >= 0) r.seed = req.seed + i;
        ds4_media_result res = {0};
        if (!ds4_media_image(c->m, &r, &res, err, sizeof(err))) {
            ds4_media_result_free(&res);
            free(prompt); free(size); free(fmt); free(weights); free(out.ptr);
            serve_error(c->fd, "502 Bad Gateway", err);
            return;
        }
        for (int k = 0; k < res.n_files; k++) {
            if (ok_any) media_buf_puts(&out, ",");
            ok_any = true;
            if (want_url) {
                const char *base = strrchr(res.files[k], '/');
                base = base ? base + 1 : res.files[k];
                char *qb = media_json_quote(base);
                media_buf_puts(&out, "{\"url\":\"/v1/media/files/");
                media_buf_append(&out, base, strlen(base));
                media_buf_puts(&out, "\",\"_name\":");
                media_buf_puts(&out, qb);
                media_buf_puts(&out, "}");
                free(qb);
            } else {
                size_t flen = 0;
                char *bytes = serve_read_file(res.files[k], &flen);
                char *b64 = bytes ? media_base64_encode((unsigned char *)bytes, flen) : media_xstrdup("");
                free(bytes);
                media_buf_puts(&out, "{\"b64_json\":\"");
                media_buf_puts(&out, b64);
                media_buf_puts(&out, "\"}");
                free(b64);
            }
        }
        ds4_media_result_free(&res);
    }
    media_buf_puts(&out, "]}");
    if (ok_any) serve_send(c->fd, "200 OK", "application/json", out.ptr, out.len);
    else serve_error(c->fd, "502 Bad Gateway", "nessuna immagine prodotta");
    free(prompt); free(size); free(fmt); free(weights); free(out.ptr);
}

/* GET /v1/media/files/<nome> — solo un nome di file, niente attraversamento. */
static void serve_file(serve_conn *c, const char *name) {
    if (strstr(name, "..") || strchr(name, '/')) { serve_error(c->fd, "400 Bad Request", "nome non valido"); return; }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ds4_media_dir(c->m), name);
    size_t len = 0;
    char *bytes = serve_read_file(path, &len);
    if (!bytes) { serve_error(c->fd, "404 Not Found", "file non trovato"); return; }
    const char *ext = strrchr(name, '.');
    const char *ct = (ext && strcmp(ext, ".mp4") == 0) ? "video/mp4" : "image/png";
    serve_send(c->fd, "200 OK", ct, bytes, len);
    free(bytes);
}

static void *serve_client(void *arg) {
    serve_conn *c = arg;
    media_buf raw = {0};
    char tmp[8192];
    /* legge finche' trova la fine degli header; poi il body per Content-Length */
    size_t hdr_end = 0, content_len = 0;
    for (;;) {
        ssize_t r = read(c->fd, tmp, sizeof(tmp));
        if (r <= 0) break;
        media_buf_append(&raw, tmp, (size_t)r);
        if (!hdr_end) {
            char *e = strstr(raw.ptr, "\r\n\r\n");
            if (e) {
                hdr_end = (size_t)(e - raw.ptr) + 4;
                char *cl = strcasestr(raw.ptr, "Content-Length:");
                if (cl) content_len = (size_t)strtol(cl + 15, NULL, 10);
            }
        }
        if (hdr_end && raw.len >= hdr_end + content_len) break;
    }
    if (raw.ptr) {
        const char *body = raw.ptr + hdr_end;
        if (strncmp(raw.ptr, "OPTIONS", 7) == 0) {
            serve_send(c->fd, "204 No Content", "text/plain", "", 0);
        } else if (strncmp(raw.ptr, "POST /v1/images/generations", 27) == 0) {
            serve_generations(c, body);
        } else if (strncmp(raw.ptr, "GET /v1/media/files/", 20) == 0) {
            char name[512] = {0};
            sscanf(raw.ptr + 20, "%511[^ ?]", name);
            serve_file(c, name);
        } else if (strncmp(raw.ptr, "GET /v1/models", 14) == 0) {
            const char *b = "{\"object\":\"list\",\"data\":[{\"id\":\"qwen-image-2.1\",\"object\":\"model\",\"owned_by\":\"ds4-media\"}]}";
            serve_send(c->fd, "200 OK", "application/json", b, strlen(b));
        } else if (strncmp(raw.ptr, "GET /health", 11) == 0) {
            char err[128];
            bool ok = ds4_media_health(c->m, err, sizeof(err));
            serve_send(c->fd, ok ? "200 OK" : "502 Bad Gateway", "application/json",
                       ok ? "{\"ok\":true}" : "{\"ok\":false}", ok ? 11 : 12);
        } else {
            serve_error(c->fd, "404 Not Found", "rotta sconosciuta");
        }
    }
    free(raw.ptr);
    close(c->fd);
    free(c);
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
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0) {
        media_set_err(err, err_len, "bind :%d: %s", port, strerror(errno));
        close(srv);
        return false;
    }
    listen(srv, 16);
    fprintf(stderr, "ds4: media serve su http://127.0.0.1:%d/v1 (immagini)\n", port);
    while (!stop || !*stop) {
        struct pollfd pfd = {.fd = srv, .events = POLLIN};
        int rc = poll(&pfd, 1, 500);
        if (rc <= 0) continue;
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        serve_conn *c = media_xmalloc(sizeof(*c));
        c->fd = fd;
        c->m = m;
        pthread_t th;
        if (pthread_create(&th, NULL, serve_client, c) != 0) { close(fd); free(c); continue; }
        pthread_detach(th);
    }
    close(srv);
    return true;
}
