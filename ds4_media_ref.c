/* ds4_media_ref - i riferimenti delle modifiche: dimensioni lette dalle intestazioni
 * (PNG, JPEG, WebP) e upload su ComfyUI con un nome unico. Vedi ds4_media_int.h. */
#include "ds4_media_int.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEDIA_REF_MAX_BYTES (64L * 1024 * 1024)

static char *media_read_file(const char *path, long max, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    char *buf = media_xmalloc((size_t)max + 1);
    size_t rd = fread(buf, 1, (size_t)max + 1, fp);
    fclose(fp);
    *len = rd;
    return buf;
}

/* ── intestazioni delle immagini ──────────────────────────────────────────── */

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const unsigned char *p) { return (unsigned long)be16(p) << 16 | be16(p + 2); }
static unsigned le16(const unsigned char *p) { return (unsigned)p[1] << 8 | p[0]; }
static unsigned long le24(const unsigned char *p) { return (unsigned long)p[2] << 16 | le16(p); }

/* JPEG: si scorrono i segmenti fino al primo SOF (C0-CF tranne DHT C4, JPG C8, DAC CC). */
static bool media_jpeg_dims(const unsigned char *b, size_t n, unsigned long *w, unsigned long *h) {
    size_t i = 2;
    while (i < n) {
        if (b[i] != 0xFF) return false;
        while (i < n && b[i] == 0xFF) i++;
        if (i >= n) return false;
        unsigned marker = b[i++];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;   /* senza lunghezza */
        if (marker == 0xD9 || marker == 0xDA) return false;   /* fine, o dati prima di un SOF */
        if (i + 2 > n) return false;
        unsigned seg = be16(b + i);
        if (seg < 2) return false;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 7 > n) return false;
            *h = be16(b + i + 3);
            *w = be16(b + i + 5);
            return true;
        }
        i += seg;
    }
    return false;
}

bool media_image_dims(const unsigned char *b, size_t n, int *w, int *h) {
    unsigned long W = 0, H = 0;
    if (n >= 24 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(b + 12, "IHDR", 4) == 0) {
        W = be32(b + 16);
        H = be32(b + 20);
    } else if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        if (!media_jpeg_dims(b, n, &W, &H)) return false;
    } else if (n >= 30 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) {
        if (memcmp(b + 12, "VP8 ", 4) == 0 && b[23] == 0x9d && b[24] == 0x01 && b[25] == 0x2a) {
            W = le16(b + 26) & 0x3fff;
            H = le16(b + 28) & 0x3fff;
        } else if (memcmp(b + 12, "VP8L", 4) == 0 && b[20] == 0x2f) {
            W = 1 + (b[21] | (unsigned long)(b[22] & 0x3f) << 8);
            H = 1 + (b[22] >> 6 | (unsigned long)b[23] << 2 | (unsigned long)(b[24] & 0x0f) << 10);
        } else if (memcmp(b + 12, "VP8X", 4) == 0) {
            W = 1 + le24(b + 24);
            H = 1 + le24(b + 27);
        } else {
            return false;
        }
    } else {
        return false;
    }
    /* Oltre 2^20 per lato non e' una foto: intestazione rotta o ostile. */
    if (W == 0 || H == 0 || W > (1ul << 20) || H > (1ul << 20)) return false;
    *w = (int)W;
    *h = (int)H;
    return true;
}

bool media_file_dims(const char *path, int *w, int *h) {
    size_t len = 0;
    char *b = media_read_file(path, 1024L * 1024, &len);
    if (!b) return false;
    bool ok = media_image_dims((unsigned char *)b, len, w, h);
    free(b);
    return ok;
}

/* ── riferimenti ──────────────────────────────────────────────────────────── */

/* Il nome su ComfyUI lo decide il modulo: con il solo basename due riferimenti
 * omonimi (a/foto.png, b/foto.png) o due processi si sovrascrivevano a vicenda nella
 * cartella condivisa. pid + indice + hash del contenuto lo rende unico, e un nome
 * nostro non ha virgolette o '/' da citare nel multipart. */
char *media_upload_ref(ds4_media *m, const char *path, int idx, char *err, size_t err_len) {
    size_t len = 0;
    char *bytes = media_read_file(path, MEDIA_REF_MAX_BYTES, &len);
    if (!bytes) { media_set_err(err, err_len, "non leggo il riferimento %s: %s", path, strerror(errno)); return NULL; }
    if (len > (size_t)MEDIA_REF_MAX_BYTES) {
        free(bytes);
        media_set_err(err, err_len, "riferimento %s oltre %ld MiB", path, MEDIA_REF_MAX_BYTES >> 20);
        return NULL;
    }
    if (len == 0) { free(bytes); media_set_err(err, err_len, "riferimento %s vuoto", path); return NULL; }
    uint64_t hash = 1469598103934665603ULL;   /* FNV-1a 64 */
    for (size_t i = 0; i < len; i++) hash = (hash ^ (unsigned char)bytes[i]) * 1099511628211ULL;

    /* estensione: solo lettere e cifre, al massimo 5, altrimenti png */
    char ext[8] = "png";
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base : path, '.');
    if (dot && dot[1]) {
        size_t k = 0;
        for (const char *p = dot + 1; *p && k < 6; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) { k = 6; break; }
            ext[k++] = (char)(*p | 0x20);
        }
        if (k == 0 || k > 5) snprintf(ext, sizeof(ext), "png");
        else ext[k] = '\0';
    }
    char name[96];
    snprintf(name, sizeof(name), "ref-%d-%d-%016llx.%s", (int)getpid(), idx, (unsigned long long)hash, ext);

    const char *fields[] = {"overwrite", "true", "subfolder", "ds4", "type", "input", NULL};
    media_http_response resp = {0};
    bool ok = media_http_post_image(m->host, m->port, "/upload/image", fields, "image",
                                    name, bytes, len, 30000, &resp, err, err_len);
    free(bytes);
    if (!ok) return NULL;
    if (resp.status != 200) {
        media_set_err(err, err_len, "/upload/image ha risposto %d", resp.status);
        media_http_response_free(&resp);
        return NULL;
    }
    char *got = media_json_str(resp.body, "name");
    char *sub = media_json_str(resp.body, "subfolder");
    media_http_response_free(&resp);
    if (!got || !got[0]) { free(got); free(sub); media_set_err(err, err_len, "/upload/image senza nome"); return NULL; }
    media_buf b = {0};
    if (sub && sub[0]) { media_buf_puts(&b, sub); media_buf_puts(&b, "/"); }
    media_buf_puts(&b, got);
    free(got);
    free(sub);
    return media_buf_take(&b);
}
