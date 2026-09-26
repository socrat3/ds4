/* Client WebSocket di ds4_media. Vedi ds4_media_ws.h.
 *
 * Solo quanto serve per ascoltare ComfyUI: handshake HTTP/1.1 con Upgrade, frame del
 * server (non mascherati per RFC, ma se lo fossero li smascheriamo), messaggi
 * frammentati, ping a cui rispondere con pong, close. Nessuna estensione negoziata,
 * quindi un bit RSV acceso e' un errore di protocollo.
 *
 * Non verifichiamo Sec-WebSocket-Accept: parliamo con il ComfyUI scelto dall'utente e
 * lo stato 101 basta a sapere che l'upgrade e' avvenuto. La verifica protegge da proxy
 * che rispondono dalla cache, che su questo percorso non esistono; costerebbe uno SHA-1
 * scritto a mano per nessun guadagno reale. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memmem */
#endif
#include "ds4_media_ws.h"
#include "ds4_media_http.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WS_MAX_HEADER (16 * 1024)
#define WS_HANDSHAKE_MS 3000

struct media_ws {
    int fd;
    media_buf in;      /* byte ricevuti; quelli prima di `off` sono gia' consumati */
    size_t off;
    media_buf frag;    /* messaggio frammentato in costruzione */
    int frag_op;       /* opcode del primo frammento; 0 = nessun messaggio aperto */
    uint32_t mask;     /* chiave di mascheramento dei nostri frame (non e' un segreto) */
};

static long ws_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Legge altri byte dal socket entro deadline. 1 = letti, 0 = scaduto (o segnale:
 * il chiamante riprova), -1 = connessione chiusa o errore. */
static int ws_fill(media_ws *ws, long deadline) {
    /* Butta via i byte consumati prima che il buffer cresca senza motivo. */
    if (ws->off == ws->in.len) {
        ws->in.len = ws->off = 0;
    } else if (ws->off > 65536) {
        memmove(ws->in.ptr, ws->in.ptr + ws->off, ws->in.len - ws->off);
        ws->in.len -= ws->off;
        ws->off = 0;
    }
    long left = deadline - ws_now_ms();
    if (left <= 0) return 0;
    struct pollfd pfd = {.fd = ws->fd, .events = POLLIN};
    int rc = poll(&pfd, 1, (int)left);
    if (rc == 0) return 0;
    if (rc < 0) return errno == EINTR ? 0 : -1;
    char tmp[16384];
    ssize_t n = read(ws->fd, tmp, sizeof(tmp));
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n == 0) return -1;
    media_buf_append(&ws->in, tmp, (size_t)n);
    return 1;
}

/* Manda un frame di controllo (pong, close): dal client i frame vanno sempre
 * mascherati, e i frame di controllo non superano 125 byte. Mai bloccante: un peer
 * che manda ping senza leggere riempirebbe il socket e ci fermerebbe in send(),
 * lontano da cancellazione e timeout. Se il frame non entra tutto, false: il
 * chiamante chiude il websocket e ripiega su /history. */
static bool ws_send_control(media_ws *ws, int opcode, const unsigned char *p, size_t n) {
    if (n > 125) n = 125;
    unsigned char f[2 + 4 + 125];
    ws->mask = ws->mask * 1103515245u + 12345u;
    unsigned char key[4] = {ws->mask >> 24, ws->mask >> 16, ws->mask >> 8, ws->mask};
    f[0] = (unsigned char)(0x80 | opcode);
    f[1] = (unsigned char)(0x80 | n);
    memcpy(f + 2, key, 4);
    for (size_t i = 0; i < n; i++) f[6 + i] = p[i] ^ key[i & 3];
    ssize_t w;
    do w = send(ws->fd, f, 6 + n, MSG_DONTWAIT | MSG_NOSIGNAL); while (w < 0 && errno == EINTR);
    return w == (ssize_t)(6 + n);
}

/* Estrae un frame dal buffer. 1 = frame pronto (consumato; payload valido fino alla
 * prossima lettura), 0 = servono altri byte, -1 = protocollo violato. */
static int ws_frame(media_ws *ws, int *op, bool *fin, unsigned char **payload, size_t *plen) {
    unsigned char *b = (unsigned char *)ws->in.ptr + ws->off;
    size_t avail = ws->in.len - ws->off;
    if (avail < 2) return 0;
    if (b[0] & 0x70) return -1;
    *fin = (b[0] & 0x80) != 0;
    *op = b[0] & 0x0f;
    bool masked = (b[1] & 0x80) != 0;
    uint64_t len = b[1] & 0x7f;
    size_t hl = 2;
    if (len == 126) {
        if (avail < 4) return 0;
        len = ((uint64_t)b[2] << 8) | b[3];
        hl = 4;
    } else if (len == 127) {
        if (avail < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i];
        hl = 10;
    }
    if (len > MEDIA_WS_MAX_MSG) return -1;
    if (*op >= 8 && (!*fin || len > 125)) return -1;
    size_t mk = hl;
    if (masked) hl += 4;
    if (avail < hl + len) return 0;
    unsigned char *p = b + hl;
    if (masked) for (uint64_t i = 0; i < len; i++) p[i] ^= b[mk + (i & 3)];
    *payload = p;
    *plen = (size_t)len;
    ws->off += hl + (size_t)len;
    return 1;
}

static void ws_deliver(const unsigned char *p, size_t n, int op, char **msg, size_t *len, bool *binary) {
    char *m = media_xmalloc(n + 1);
    memcpy(m, p, n);
    m[n] = '\0';
    *msg = m;
    *len = n;
    *binary = (op == 2);
}

int media_ws_read(media_ws *ws, int timeout_ms, char **msg, size_t *len, bool *binary) {
    long deadline = ws_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int op;
        bool fin;
        unsigned char *p;
        size_t n;
        int r = ws_frame(ws, &op, &fin, &p, &n);
        if (r < 0) return -1;
        if (r == 0) {
            int f = ws_fill(ws, deadline);
            if (f < 0) return -1;
            if (f == 0) return 0;
            continue;
        }
        switch (op) {
        case 0x9:                                            /* ping -> pong */
            if (!ws_send_control(ws, 0xA, p, n)) return -1;
            continue;
        case 0xA: continue;                                  /* pong non richiesto */
        case 0x8: ws_send_control(ws, 0x8, p, n < 2 ? n : 2); return -1;
        case 0x0:                                            /* continuazione */
            if (!ws->frag_op) return -1;
            if (ws->frag.len + n > MEDIA_WS_MAX_MSG) return -1;
            media_buf_append(&ws->frag, (const char *)p, n);
            if (!fin) continue;
            ws_deliver((unsigned char *)ws->frag.ptr, ws->frag.len, ws->frag_op, msg, len, binary);
            ws->frag.len = 0;
            ws->frag_op = 0;
            return 1;
        case 0x1:
        case 0x2:
            if (ws->frag_op) return -1;   /* nuovo messaggio dentro uno frammentato */
            if (fin) { ws_deliver(p, n, op, msg, len, binary); return 1; }
            ws->frag_op = op;
            ws->frag.len = 0;
            media_buf_append(&ws->frag, (const char *)p, n);
            continue;
        default:
            return -1;
        }
    }
}

media_ws *media_ws_open(const char *host, int port, const char *client_id,
                        char *err, size_t err_len) {
    int fd = media_tcp_connect(host, port, err, err_len);
    if (fd < 0) return NULL;

    /* La chiave dell'handshake deve essere casuale, ma non e' un segreto. */
    unsigned char key[16];
    long seed = ws_now_ms() ^ ((long)getpid() << 16);
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd < 0 || read(rfd, key, sizeof(key)) != (ssize_t)sizeof(key))
        for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005L + 1; key[i] = (unsigned char)(seed >> 33); }
    if (rfd >= 0) close(rfd);
    char *k64 = media_base64_encode(key, sizeof(key));
    char *cid = media_url_encode(client_id);
    char req[1024];
    int rl = snprintf(req, sizeof(req),
                      "GET /ws?clientId=%s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
                      "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                      cid, host, port, k64);
    free(k64);
    free(cid);
    if (rl < 0 || (size_t)rl >= sizeof(req) || media_write_all(fd, req, (size_t)rl) != 0) {
        media_set_err(err, err_len, "websocket: invio dell'handshake non riuscito");
        close(fd);
        return NULL;
    }

    media_ws *ws = media_xmalloc(sizeof(*ws));
    memset(ws, 0, sizeof(*ws));
    ws->fd = fd;
    ws->mask = (uint32_t)seed ^ key[0];
    long deadline = ws_now_ms() + WS_HANDSHAKE_MS;
    char *end = NULL;
    while (!end) {
        if (ws_fill(ws, deadline) <= 0 || ws->in.len > WS_MAX_HEADER) {
            media_set_err(err, err_len, "websocket: nessuna risposta all'handshake");
            media_ws_close(ws);
            return NULL;
        }
        end = memmem(ws->in.ptr, ws->in.len, "\r\n\r\n", 4);
    }
    if (ws->in.len < 12 || strncmp(ws->in.ptr, "HTTP/1.", 7) != 0 ||
        strtol(ws->in.ptr + 9, NULL, 10) != 101) {
        media_set_err(err, err_len, "websocket: ComfyUI non ha accettato l'upgrade");
        media_ws_close(ws);
        return NULL;
    }
    ws->off = (size_t)(end + 4 - ws->in.ptr);   /* i frame arrivati con l'header restano */
    return ws;
}

void media_ws_close(media_ws *ws) {
    if (!ws) return;
    close(ws->fd);
    free(ws->in.ptr);
    free(ws->frag.ptr);
    free(ws);
}
