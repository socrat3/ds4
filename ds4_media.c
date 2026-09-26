/* ds4_media - regia della generazione di immagini via ComfyUI. Vedi ds4_media.h.
 * Il grafo testo-immagine e' quello del template ufficiale Qwen-Image 2.1 (t2i):
 *   UNETLoader -> (CLIPLoader, VAELoader) -> TextEncodeQwenImage21 -> KSampler
 *   -> VAEDecode -> SaveImage, 25 passi, cfg 1, euler/simple. La modifica aggiunge
 *   images.image_1..N a TextEncodeQwenImage21 (riferimenti caricati con /upload/image). */
#include "ds4_media.h"
#include "ds4_media_http.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Nomi dei pesi come stanno su disco in ComfyUI (Comfy-Org/Qwen-Image-2.1). */
#define QI_UNET_INT8 "qwen_image_2.1_int8_convrot.safetensors"
#define QI_UNET_BF16 "qwen_image_2.1_bf16.safetensors"
#define QI_CLIP_INT8 "qwen3vl_8b_int8_convrot.safetensors"
#define QI_CLIP_BF16 "qwen3vl_8b_bf16.safetensors"
#define QI_VAE       "qwen_image_2.1_vae_bf16.safetensors"

#define MEDIA_MAX_W 2752
#define MEDIA_MAX_H 1536
#define MEDIA_MARGIN_KIB (6L * 1024 * 1024)   /* margine di sicurezza: 6 GiB */

struct ds4_media {
    char *host, *media_dir, *comfy_output_dir;
    int port;
    ds4_media_weights weights;
    int idle_free_sec;
    bool no_gate;
    long avail_override_kib;
    ds4_media_log_fn log;
    void *log_privdata;
    ds4_media_cancel_fn cancel;
    void *cancel_privdata;
    time_t last_job;
};

static void media_log(ds4_media *m, const char *fmt, ...) {
    if (!m->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m->log(m->log_privdata, buf);
}

static bool media_cancelled(ds4_media *m) {
    return m->cancel && m->cancel(m->cancel_privdata);
}

/* ── ciclo di vita ────────────────────────────────────────────────────────── */

static char *media_default_dir(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = ".";
    media_buf b = {0};
    media_buf_puts(&b, home);
    media_buf_puts(&b, "/.ds4/media");
    return media_buf_take(&b);
}

static bool media_mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

ds4_media *ds4_media_create(const ds4_media_config *cfg) {
    ds4_media *m = media_xmalloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->host = media_xstrdup(cfg->host && cfg->host[0] ? cfg->host : "127.0.0.1");
    m->port = cfg->port > 0 ? cfg->port : 8188;
    m->media_dir = cfg->media_dir && cfg->media_dir[0] ? media_xstrdup(cfg->media_dir)
                                                       : media_default_dir();
    m->comfy_output_dir = cfg->comfy_output_dir && cfg->comfy_output_dir[0]
                              ? media_xstrdup(cfg->comfy_output_dir) : NULL;
    m->weights = cfg->weights;
    m->idle_free_sec = cfg->idle_free_sec;
    m->no_gate = cfg->no_gate;
    m->avail_override_kib = cfg->avail_override_kib;
    m->log = cfg->log;
    m->log_privdata = cfg->log_privdata;
    m->cancel = cfg->cancel;
    m->cancel_privdata = cfg->cancel_privdata;
    if (!media_mkdir_p(m->media_dir))
        media_log(m, "ds4: media attenzione: non creo %s", m->media_dir);
    return m;
}

void ds4_media_free(ds4_media *m) {
    if (!m) return;
    free(m->host);
    free(m->media_dir);
    free(m->comfy_output_dir);
    free(m);
}

void ds4_media_result_free(ds4_media_result *r) {
    if (!r) return;
    for (int i = 0; i < r->n_files; i++) free(r->files[i]);
    free(r->files);
    r->files = NULL;
    r->n_files = 0;
}

/* ── gate di memoria ──────────────────────────────────────────────────────── */

static long media_avail_kib(ds4_media *m) {
    if (m->avail_override_kib > 0) return m->avail_override_kib;
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[256];
    long kib = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) { kib = strtol(line + 13, NULL, 10); break; }
    }
    fclose(fp);
    return kib;
}

/* Fabbisogno stimato (GiB) da pesi e dimensioni, dai picchi misurati il 25/09. */
static long media_need_kib(ds4_media_weights w, int width, int height) {
    long gib;
    if (w == DS4_MEDIA_BF16) gib = ((long)width * height > 1024L * 1024) ? 37 : 32;
    else gib = 18;
    return gib * 1024 * 1024;
}

static bool media_gate(ds4_media *m, ds4_media_weights w, int width, int height,
                       char *err, size_t err_len) {
    if (m->no_gate) return true;
    long avail = media_avail_kib(m);
    if (avail < 0) return true;   /* non so leggere: non blocco */
    long need = media_need_kib(w, width, height) + MEDIA_MARGIN_KIB;
    if (need > avail) {
        media_set_err(err, err_len,
                      "servono ~%ld GiB liberi, disponibili %ld GiB: usa int8, riduci le dimensioni, "
                      "o libera memoria (spegni un modello)",
                      need / (1024 * 1024), avail / (1024 * 1024));
        return false;
    }
    return true;
}

/* ── costruzione del grafo ────────────────────────────────────────────────── */

/* Avvolge il prompt nella formula RGBA ufficiale per lo sfondo trasparente. */
static char *media_wrap_transparent(const char *prompt) {
    media_buf b = {0};
    media_buf_puts(&b, "This is an RGBA format image with transparency. ");
    media_buf_puts(&b, prompt);
    media_buf_puts(&b, ". The image has an alpha channel and a transparent background.");
    return media_buf_take(&b);
}

/* Costruisce il JSON {"prompt": <grafo>, "client_id": ...} per POST /prompt.
 * ref_names = nomi dei file di riferimento gia' caricati su ComfyUI (o NULL). */
static char *media_build_graph(ds4_media *m, const ds4_media_image_req *req,
                               ds4_media_weights w, long seed, int width, int height,
                               int steps, double cfg, char **ref_names, int n_refs,
                               const char *prefix) {
    (void)m;
    const char *unet = (w == DS4_MEDIA_BF16) ? QI_UNET_BF16 : QI_UNET_INT8;
    const char *clip = (w == DS4_MEDIA_BF16) ? QI_CLIP_BF16 : QI_CLIP_INT8;
    char *eff_prompt = req->transparent ? media_wrap_transparent(req->prompt)
                                        : media_xstrdup(req->prompt);
    char *qprompt = media_json_quote(eff_prompt);
    char *qneg = media_json_quote(req->negative ? req->negative : "");
    free(eff_prompt);

    media_buf g = {0};
    char line[1024];
    media_buf_puts(&g, "{\"prompt\":{");
    /* 1 UNETLoader, 2 CLIPLoader, 3 VAELoader */
    snprintf(line, sizeof(line),
             "\"1\":{\"class_type\":\"UNETLoader\",\"inputs\":{\"unet_name\":\"%s\",\"weight_dtype\":\"default\"}},", unet);
    media_buf_puts(&g, line);
    snprintf(line, sizeof(line),
             "\"2\":{\"class_type\":\"CLIPLoader\",\"inputs\":{\"clip_name\":\"%s\",\"type\":\"qwen_image\",\"device\":\"default\"}},", clip);
    media_buf_puts(&g, line);
    snprintf(line, sizeof(line),
             "\"3\":{\"class_type\":\"VAELoader\",\"inputs\":{\"vae_name\":\"%s\"}},", QI_VAE);
    media_buf_puts(&g, line);
    /* 4 TextEncodeQwenImage21 (con eventuali riferimenti) */
    media_buf_puts(&g, "\"4\":{\"class_type\":\"TextEncodeQwenImage21\",\"inputs\":{\"clip\":[\"2\",0],\"prompt\":");
    media_buf_puts(&g, qprompt);
    media_buf_puts(&g, ",\"negative_prompt\":");
    media_buf_puts(&g, qneg);
    media_buf_puts(&g, ",\"resolution\":1024");
    for (int i = 0; i < n_refs; i++) {
        snprintf(line, sizeof(line), ",\"images.image_%d\":[\"%d\",0]", i + 1, 100 + i);
        media_buf_puts(&g, line);
    }
    media_buf_puts(&g, ",\"vae\":[\"3\",0]}},");
    /* 5 EmptyLatentImage */
    snprintf(line, sizeof(line),
             "\"5\":{\"class_type\":\"EmptyLatentImage\",\"inputs\":{\"width\":%d,\"height\":%d,\"batch_size\":1}},",
             width, height);
    media_buf_puts(&g, line);
    /* 6 KSampler */
    snprintf(line, sizeof(line),
             "\"6\":{\"class_type\":\"KSampler\",\"inputs\":{\"model\":[\"1\",0],\"positive\":[\"4\",0],"
             "\"negative\":[\"4\",1],\"latent_image\":[\"5\",0],\"seed\":%ld,\"steps\":%d,\"cfg\":%.3f,"
             "\"sampler_name\":\"euler\",\"scheduler\":\"simple\",\"denoise\":1.0}},",
             seed, steps, cfg);
    media_buf_puts(&g, line);
    /* 7 VAEDecode, 8 SaveImage */
    media_buf_puts(&g, "\"7\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"6\",0],\"vae\":[\"3\",0]}},");
    snprintf(line, sizeof(line),
             "\"8\":{\"class_type\":\"SaveImage\",\"inputs\":{\"images\":[\"7\",0],\"filename_prefix\":\"%s\"}}", prefix);
    media_buf_puts(&g, line);
    /* nodi di caricamento dei riferimenti: 100+i LoadImage */
    for (int i = 0; i < n_refs; i++) {
        char *qn = media_json_quote(ref_names[i]);
        snprintf(line, sizeof(line), ",\"%d\":{\"class_type\":\"LoadImage\",\"inputs\":{\"image\":", 100 + i);
        media_buf_puts(&g, line);
        media_buf_puts(&g, qn);
        media_buf_puts(&g, "}}");
        free(qn);
    }
    media_buf_puts(&g, "},\"client_id\":\"ds4-media\"}");
    free(qprompt);
    free(qneg);
    return media_buf_take(&g);
}

/* ── invio, attesa, scarico ───────────────────────────────────────────────── */

static char *media_read_file(const char *path, size_t *len) {
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

/* Carica un file di riferimento su ComfyUI; restituisce il nome assegnato. */
static char *media_upload_ref(ds4_media *m, const char *path, char *err, size_t err_len) {
    size_t len = 0;
    char *bytes = media_read_file(path, &len);
    if (!bytes) { media_set_err(err, err_len, "non leggo il riferimento %s", path); return NULL; }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *fields[] = {"overwrite", "true", "subfolder", "ds4", "type", "input", NULL};
    media_http_response resp = {0};
    bool ok = media_http_post_image(m->host, m->port, "/upload/image", fields, "image",
                                    base, bytes, len, 30000, &resp, err, err_len);
    free(bytes);
    if (!ok) return NULL;
    if (resp.status != 200) {
        media_set_err(err, err_len, "/upload/image ha risposto %d", resp.status);
        media_http_response_free(&resp);
        return NULL;
    }
    char *name = media_json_str(resp.body, "name");
    char *sub = media_json_str(resp.body, "subfolder");
    media_http_response_free(&resp);
    if (!name) { free(sub); media_set_err(err, err_len, "/upload/image senza nome"); return NULL; }
    if (sub && sub[0]) {
        media_buf b = {0};
        media_buf_puts(&b, sub);
        media_buf_puts(&b, "/");
        media_buf_puts(&b, name);
        free(name);
        free(sub);
        return media_buf_take(&b);
    }
    free(sub);
    return name;
}

/* Aspetta la fine del lavoro; su successo mette in outputs il corpo di /history. */
static bool media_wait(ds4_media *m, const char *prompt_id, char **out_history,
                       int timeout_ms, char *err, size_t err_len) {
    int waited = 0;
    char path[128];
    snprintf(path, sizeof(path), "/history/%s", prompt_id);
    for (;;) {
        if (media_cancelled(m)) {
            char ipath[160];
            media_http_response ir = {0};
            char body[128];
            snprintf(body, sizeof(body), "{\"prompt_id\":\"%s\"}", prompt_id);
            snprintf(ipath, sizeof(ipath), "/interrupt");
            media_http_post(m->host, m->port, ipath, "application/json", body, strlen(body),
                            5000, &ir, err, err_len);
            media_http_response_free(&ir);
            media_set_err(err, err_len, "annullato");
            return false;
        }
        media_http_response resp = {0};
        if (!media_http_get(m->host, m->port, path, 10000, &resp, err, err_len)) return false;
        bool has = resp.body && strstr(resp.body, "\"status\"") &&
                   (strstr(resp.body, "\"success\"") || strstr(resp.body, "\"error\""));
        if (has) {
            if (strstr(resp.body, "\"status_str\": \"error\"") ||
                strstr(resp.body, "\"status_str\":\"error\"")) {
                char *msg = media_json_str(resp.body, "exception_message");
                media_set_err(err, err_len, "ComfyUI: %s", msg ? msg : "errore di esecuzione");
                free(msg);
                media_http_response_free(&resp);
                return false;
            }
            *out_history = resp.body;
            resp.body = NULL;
            media_http_response_free(&resp);
            return true;
        }
        media_http_response_free(&resp);
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);
        waited += 1000;
        if (waited >= timeout_ms) { media_set_err(err, err_len, "timeout: ComfyUI non ha finito in %d s", timeout_ms / 1000); return false; }
    }
}

/* Scarica i file elencati in outputs con GET /view e li scrive in media_dir. Ogni
 * immagine e' un oggetto {"filename":..,"subfolder":..,"type":..}: per ogni "filename"
 * isolo l'oggetto (dal '{' precedente al '}' seguente) e ne leggo i tre campi. */
static bool media_fetch_outputs(ds4_media *m, const char *history, const char *stem,
                                ds4_media_result *out, char *err, size_t err_len) {
    const char *p = history;
    char **list = NULL;
    int n = 0, cap = 0, idx = 0;
    const char *hit;
    while ((hit = strstr(p, "\"filename\"")) != NULL) {
        const char *obj_beg = hit;
        while (obj_beg > history && *obj_beg != '{') obj_beg--;
        const char *obj_end = strchr(hit, '}');
        p = obj_end ? obj_end + 1 : hit + 10;
        if (!obj_end) break;
        size_t span = (size_t)(obj_end - obj_beg + 1);
        char *chunk = media_xmalloc(span + 1);
        memcpy(chunk, obj_beg, span);
        chunk[span] = '\0';
        char *fn = media_json_str(chunk, "filename");
        char *sub = media_json_str(chunk, "subfolder");
        char *type = media_json_str(chunk, "type");
        free(chunk);
        if (fn && (!type || strcmp(type, "output") == 0)) {
            char *efn = media_url_encode(fn);
            char *esub = media_url_encode(sub ? sub : "");
            char vpath[1024];
            snprintf(vpath, sizeof(vpath), "/view?filename=%s&subfolder=%s&type=output", efn, esub);
            free(efn);
            free(esub);
            media_http_response vr = {0};
            if (media_http_get(m->host, m->port, vpath, 60000, &vr, err, err_len) && vr.status == 200) {
                const char *ext = strrchr(fn, '.');
                char dest[1024];
                snprintf(dest, sizeof(dest), "%s/%s-%03d%s", m->media_dir, stem, idx, ext ? ext : ".png");
                FILE *of = fopen(dest, "wb");
                if (of) {
                    fwrite(vr.body, 1, vr.body_len, of);
                    fclose(of);
                    if (n == cap) { cap = cap ? cap * 2 : 4; list = realloc(list, (size_t)cap * sizeof(char *)); }
                    list[n++] = media_xstrdup(dest);
                    idx++;
                    /* Togli la copia doppia che ComfyUI ha salvato (host locale): niente file sparsi. */
                    if (m->comfy_output_dir) {
                        char copy[1600];
                        if (sub && sub[0])
                            snprintf(copy, sizeof(copy), "%s/%s/%s", m->comfy_output_dir, sub, fn);
                        else
                            snprintf(copy, sizeof(copy), "%s/%s", m->comfy_output_dir, fn);
                        unlink(copy);
                    }
                }
            }
            media_http_response_free(&vr);
        }
        free(fn);
        free(sub);
        free(type);
    }
    out->files = list;
    out->n_files = n;
    if (n == 0) { media_set_err(err, err_len, "nessuna immagine prodotta da ComfyUI"); return false; }
    return true;
}

bool ds4_media_image(ds4_media *m, const ds4_media_image_req *req,
                     ds4_media_result *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    if (!req->prompt || !req->prompt[0]) { media_set_err(err, err_len, "prompt vuoto"); return false; }
    if (strlen(req->prompt) > 4000) { media_set_err(err, err_len, "prompt troppo lungo (max 4000)"); return false; }
    int width = req->width > 0 ? req->width : 1024;
    int height = req->height > 0 ? req->height : 1024;
    if (width % 32 || height % 32) { media_set_err(err, err_len, "dimensioni multiple di 32"); return false; }
    if (width > MEDIA_MAX_W || height > MEDIA_MAX_H) {
        media_set_err(err, err_len, "dimensioni oltre il massimo %dx%d", MEDIA_MAX_W, MEDIA_MAX_H);
        return false;
    }
    int steps = req->steps > 0 ? req->steps : 25;
    if (steps > 60) { media_set_err(err, err_len, "passi oltre 60"); return false; }
    if (req->n_refs > 10) { media_set_err(err, err_len, "massimo 10 riferimenti"); return false; }
    double cfg = req->cfg > 0 ? req->cfg : 1.0;
    ds4_media_weights w = req->weights;
    long seed = req->seed >= 0 ? req->seed : (long)(time(NULL) ^ (getpid() << 8)) & 0x7fffffff;

    if (!media_gate(m, w, width, height, err, err_len)) return false;

    /* carica i riferimenti */
    char *ref_names[10] = {0};
    for (int i = 0; i < req->n_refs; i++) {
        ref_names[i] = media_upload_ref(m, req->refs[i], err, err_len);
        if (!ref_names[i]) { for (int j = 0; j < i; j++) free(ref_names[j]); return false; }
    }

    char stem[96];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stem, sizeof(stem), "img-%Y%m%d-%H%M%S", &tmv);
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "ds4/%s-%ld", stem, seed);

    char *graph = media_build_graph(m, req, w, seed, width, height, steps, cfg,
                                    ref_names, req->n_refs, prefix);
    for (int i = 0; i < req->n_refs; i++) free(ref_names[i]);

    long t0 = (long)time(NULL);
    media_http_response resp = {0};
    bool ok = media_http_post(m->host, m->port, "/prompt", "application/json",
                              graph, strlen(graph), 30000, &resp, err, err_len);
    free(graph);
    if (!ok) return false;
    if (resp.status != 200) {
        char *ne = media_json_str(resp.body, "type");
        media_set_err(err, err_len, "ComfyUI ha rifiutato il grafo (%d)%s%s", resp.status,
                      ne ? ": " : "", ne ? ne : "");
        free(ne);
        media_http_response_free(&resp);
        return false;
    }
    char *pid = media_json_str(resp.body, "prompt_id");
    media_http_response_free(&resp);
    if (!pid) { media_set_err(err, err_len, "ComfyUI non ha dato un prompt_id"); return false; }
    snprintf(out->prompt_id, sizeof(out->prompt_id), "%s", pid);
    media_log(m, "ds4: media inviato a ComfyUI (id %s), attendo la generazione"
                 " (in coda dietro altri lavori GPU puo' richiedere piu' tempo)...", out->prompt_id);

    /* Timeout ampio: se la GPU e' occupata (H3, un'altra generazione) l'immagine resta in coda. */
    char *history = NULL;
    ok = media_wait(m, pid, &history, 900000, err, err_len);
    free(pid);
    if (!ok) return false;

    ok = media_fetch_outputs(m, history, stem, out, err, err_len);
    free(history);
    if (!ok) { ds4_media_result_free(out); return false; }

    out->width = width;
    out->height = height;
    out->seed = seed;
    out->ms = ((long)time(NULL) - t0) * 1000;
    m->last_job = time(NULL);
    media_log(m, "ds4: media %s %dx%d seed=%ld %ld ms -> %s",
              out->prompt_id, width, height, seed, out->ms,
              out->n_files ? out->files[0] : "(niente)");
    return true;
}

#include "ds4_media_h3.inc"

/* Sostituisce tutte le occorrenze di `needle` con `repl` (malloc'd). */
static char *media_replace(const char *s, const char *needle, const char *repl) {
    media_buf b = {0};
    size_t nl = strlen(needle);
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, needle);
        if (!hit) { media_buf_puts(&b, p); break; }
        media_buf_append(&b, p, (size_t)(hit - p));
        media_buf_puts(&b, repl);
        p = hit + nl;
    }
    return media_buf_take(&b);
}

/* Contenuto di una stringa JSON, senza le virgolette esterne (per i segnaposto tra ""). */
static char *media_json_inner(const char *s) {
    char *q = media_json_quote(s);   /* "...." */
    size_t n = strlen(q);
    if (n >= 2) { memmove(q, q + 1, n - 2); q[n - 2] = '\0'; }
    return q;
}

/* Fotogrammi dal tempo, sulla griglia 17k+5 di H3 (124~5s, 362~15s). */
static int media_h3_length(double seconds) {
    if (seconds <= 0) seconds = 5.0;
    long base = (long)(seconds * 24.0 + 0.5);
    long k = (base - 5 + 8) / 17;   /* arrotonda */
    long frames = 5 + k * 17;
    if (frames < 5) frames = 5;
    if (frames > 362) frames = 362;
    return (int)frames;
}

bool ds4_media_video(ds4_media *m, const ds4_media_video_req *req,
                     ds4_media_result *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    if (!req->prompt || !req->prompt[0]) { media_set_err(err, err_len, "prompt vuoto"); return false; }
    if (!req->ref || !req->ref[0]) { media_set_err(err, err_len, "serve un'immagine di riferimento (--rif): H3 parte dal primo fotogramma"); return false; }
    int width = req->width > 0 ? req->width : 864;
    int height = req->height > 0 ? req->height : 480;
    if (width % 32 || height % 32) { media_set_err(err, err_len, "dimensioni multiple di 32"); return false; }
    int steps = req->steps > 0 ? req->steps : 20;
    int length = media_h3_length(req->seconds);
    long seed = req->seed >= 0 ? req->seed : (long)(time(NULL) ^ (getpid() << 8)) & 0x7fffffff;

    /* Gate: H3 occupa la GPU per intero (~110 GiB in uso). Serve che nessun modello
     * grande sia residente; a macchina scarica MemAvailable e' ~111-118 GiB. Richiedo
     * >= 100 GiB liberi: passa a vuoto, blocca se un LLM/immagine e' ancora caricato. */
    if (!m->no_gate) {
        long avail = media_avail_kib(m);
        long need = 100L * 1024 * 1024;
        if (avail > 0 && need > avail) {
            media_set_err(err, err_len, "H3 vuole la GPU quasi intera, liberi solo %ld GiB: "
                          "spegni ogni modello/immagine prima del video", avail / (1024 * 1024));
            return false;
        }
    }

    char *refname = media_upload_ref(m, req->ref, err, err_len);
    if (!refname) return false;

    char stem[96];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(stem, sizeof(stem), "vid-%Y%m%d-%H%M%S", &tmv);
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "ds4/%s-%ld", stem, seed);

    char *qprompt = media_json_inner(req->prompt);
    char *qimage = media_json_inner(refname);
    free(refname);
    char nums[8][24];
    snprintf(nums[0], 24, "%d", width);
    snprintf(nums[1], 24, "%d", height);
    snprintf(nums[2], 24, "%d", length);
    snprintf(nums[3], 24, "%ld", seed);
    snprintf(nums[4], 24, "%d", steps);
    char *g = media_xstrdup(DS4_MEDIA_H3_TEMPLATE);
    const char *subs[][2] = {{"__PROMPT__", qprompt}, {"__IMAGE__", qimage}, {"__PREFIX__", prefix},
                             {"__WIDTH__", nums[0]}, {"__HEIGHT__", nums[1]}, {"__LENGTH__", nums[2]},
                             {"__SEED__", nums[3]}, {"__STEPS__", nums[4]}};
    for (int i = 0; i < 8; i++) { char *n = media_replace(g, subs[i][0], subs[i][1]); free(g); g = n; }
    free(qprompt);
    free(qimage);

    media_buf body = {0};
    media_buf_puts(&body, "{\"prompt\":");
    media_buf_puts(&body, g);
    media_buf_puts(&body, ",\"client_id\":\"ds4-media\"}");
    free(g);
    char *graph = media_buf_take(&body);

    media_log(m, "ds4: media video %dx%d %d fotogrammi (~%.1f s), attendo H3 (minuti)...",
              width, height, length, length / 24.0);
    long t0 = (long)time(NULL);
    media_http_response resp = {0};
    bool ok = media_http_post(m->host, m->port, "/prompt", "application/json",
                              graph, strlen(graph), 30000, &resp, err, err_len);
    free(graph);
    if (!ok) return false;
    if (resp.status != 200) {
        char *ne = media_json_str(resp.body, "type");
        media_set_err(err, err_len, "ComfyUI ha rifiutato il grafo H3 (%d)%s%s", resp.status,
                      ne ? ": " : "", ne ? ne : "");
        free(ne);
        media_http_response_free(&resp);
        return false;
    }
    char *pid = media_json_str(resp.body, "prompt_id");
    media_http_response_free(&resp);
    if (!pid) { media_set_err(err, err_len, "ComfyUI non ha dato un prompt_id"); return false; }
    snprintf(out->prompt_id, sizeof(out->prompt_id), "%s", pid);

    char *history = NULL;
    ok = media_wait(m, pid, &history, 1800000, err, err_len);   /* fino a 30 min */
    free(pid);
    if (!ok) return false;
    ok = media_fetch_outputs(m, history, stem, out, err, err_len);
    free(history);
    if (!ok) { ds4_media_result_free(out); return false; }
    out->width = width;
    out->height = height;
    out->seed = seed;
    out->ms = ((long)time(NULL) - t0) * 1000;
    m->last_job = time(NULL);
    media_log(m, "ds4: media video %s %ld ms -> %s", out->prompt_id, out->ms,
              out->n_files ? out->files[0] : "(niente)");
    return true;
}

bool ds4_media_health(ds4_media *m, char *err, size_t err_len) {
    media_http_response resp = {0};
    if (!media_http_get(m->host, m->port, "/system_stats", 5000, &resp, err, err_len)) return false;
    bool ok = resp.status == 200;
    if (!ok) media_set_err(err, err_len, "ComfyUI ha risposto %d a /system_stats", resp.status);
    media_http_response_free(&resp);
    return ok;
}

bool ds4_media_free_models(ds4_media *m, char *err, size_t err_len) {
    const char *body = "{\"unload_models\":true,\"free_memory\":true}";
    media_http_response resp = {0};
    if (!media_http_post(m->host, m->port, "/free", "application/json", body, strlen(body),
                         10000, &resp, err, err_len)) return false;
    bool ok = resp.status == 200;
    media_http_response_free(&resp);
    return ok;
}

long ds4_media_avail_gib(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[256];
    long kib = -1;
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, "MemAvailable:", 13) == 0) { kib = strtol(line + 13, NULL, 10); break; }
    fclose(fp);
    return kib < 0 ? -1 : kib / (1024 * 1024);
}

bool ds4_media_parse_size(const char *s, int *width, int *height) {
    if (!s) return false;
    char *x = NULL;
    long w = strtol(s, &x, 10);
    if (!x || (*x != 'x' && *x != 'X')) return false;
    long h = strtol(x + 1, NULL, 10);
    if (w <= 0 || h <= 0 || w % 32 || h % 32 || w > MEDIA_MAX_W || h > MEDIA_MAX_H) return false;
    *width = (int)w;
    *height = (int)h;
    return true;
}

/* Cartella di uscita, per il server che restituisce i file con response_format=url. */
const char *ds4_media_dir(const ds4_media *m) { return m->media_dir; }
