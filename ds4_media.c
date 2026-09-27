/* ds4_media - regia della generazione di immagini via ComfyUI. Vedi ds4_media.h.
 * Il grafo testo-immagine e' quello del template ufficiale Qwen-Image 2.1 (t2i):
 *   UNETLoader -> (CLIPLoader, VAELoader) -> TextEncodeQwenImage21 -> KSampler
 *   -> VAEDecode -> SaveImage, 25 passi, cfg 1, euler/simple. La modifica aggiunge
 *   images.image_1..N a TextEncodeQwenImage21 (riferimenti caricati con /upload/image)
 *   e campiona sul latente che il nodo stesso dimensiona sul primo riferimento: con un
 *   latente di altre dimensioni la modifica esce spostata (tooltip in nodes_qwen.py).
 *
 * Qui stanno ciclo di vita, memoria, grafo e lavoro delle immagini; il viaggio verso
 * ComfyUI e' in ds4_media_job.c, il video in ds4_media_video.c. */
#include "ds4_media_int.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Nomi dei pesi come stanno su disco in ComfyUI (Comfy-Org/Qwen-Image-2.1). */
#define QI_UNET_INT8 "qwen_image_2.1_int8_convrot.safetensors"
#define QI_UNET_BF16 "qwen_image_2.1_bf16.safetensors"
#define QI_CLIP_INT8 "qwen3vl_8b_int8_convrot.safetensors"
#define QI_CLIP_BF16 "qwen3vl_8b_bf16.safetensors"
#define QI_VAE       "qwen_image_2.1_vae_bf16.safetensors"

#define GIB_KIB (1024L * 1024)
#define MEDIA_MARGIN_KIB (6 * GIB_KIB)   /* margine di sicurezza: 6 GiB */
#define MEDIA_EDIT_MIN_RES 512           /* sotto, Qwen-Image rende male: si ingrandisce */

/* ── ciclo di vita ────────────────────────────────────────────────────────── */

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

const char *ds4_media_home(void) {
    static char buf[1024];
    const char *e = getenv("DS4_MEDIA_HOME"), *h = getenv("HOME");
    if (e && e[0]) snprintf(buf, sizeof(buf), "%s", e);
    else snprintf(buf, sizeof(buf), "%s/ds4-media", h && h[0] ? h : ".");
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = '\0';
    return buf;
}

ds4_media *ds4_media_create(const ds4_media_config *cfg) {
    ds4_media *m = media_xmalloc(sizeof(*m));
    memset(m, 0, sizeof(*m));
    m->host = media_xstrdup(cfg->host && cfg->host[0] ? cfg->host : "127.0.0.1");
    m->port = cfg->port > 0 ? cfg->port : 8188;
    if (cfg->media_dir && cfg->media_dir[0]) {
        m->media_dir = media_xstrdup(cfg->media_dir);
    } else {
        media_buf b = {0};
        media_buf_puts(&b, ds4_media_home());
        media_buf_puts(&b, "/immagini");
        m->media_dir = media_buf_take(&b);
    }
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
    m->progress = cfg->progress;
    m->progress_privdata = cfg->progress_privdata;
    m->cache_node = -1;
    pthread_mutex_init(&m->lock, NULL);
    if (!media_mkdir_p(m->media_dir))
        media_log(m, "ds4: media attenzione: non creo %s", m->media_dir);
    return m;
}

void ds4_media_free(ds4_media *m) {
    if (!m) return;
    free(m->host);
    free(m->media_dir);
    free(m->comfy_output_dir);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

ds4_media_weights ds4_media_default_weights(const ds4_media *m) { return m->weights; }

void ds4_media_result_free(ds4_media_result *r) {
    if (!r) return;
    for (int i = 0; i < r->n_files; i++) free(r->files[i]);
    free(r->files);
    r->files = NULL;
    r->n_files = 0;
}

/* ── memoria ──────────────────────────────────────────────────────────────── */

static long media_meminfo_kib(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char line[256];
    long kib = -1;
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, "MemAvailable:", 13) == 0) { kib = strtol(line + 13, NULL, 10); break; }
    fclose(fp);
    return kib;
}

long ds4_media_avail_gib(void) {
    long kib = media_meminfo_kib();
    return kib < 0 ? -1 : kib / GIB_KIB;
}

/* La memoria che ComfyUI gia' tiene (i pesi dell'ultimo lavoro, la cache di torch) non
 * compare in MemAvailable, ma ComfyUI la riusa: contarla evita di rifiutare il secondo
 * lavoro quando il primo ha lasciato i pesi caricati. Si prende il massimo fra
 * torch_vram_total e la GPU del processo (ds4_media_mem.c). */
long media_comfy_avail_kib(ds4_media *m) {
    long avail = m->avail_override_kib > 0 ? m->avail_override_kib : media_meminfo_kib();
    if (avail < 0) return -1;
    media_http_response r = {0};
    char e[64];
    long tenuta = 0;
    if (media_http_get(m->host, m->port, "/system_stats", 5000, &r, e, sizeof(e)) && r.status == 200) {
        bool found = false;
        long bytes = media_json_int(r.body, "torch_vram_total", &found);
        if (found && bytes > 0) tenuta = bytes / 1024;
    }
    media_http_response_free(&r);
    /* torch_vram_total sottostima con cudaMallocAsync: conta la GPU vera del processo */
    if (!strcmp(m->host, "127.0.0.1") || !strcmp(m->host, "localhost")) {
        long gpu = media_gpu_used_kib(media_pid_on_port(m->port));
        if (gpu > tenuta) tenuta = gpu;
    }
    return avail + tenuta;
}

long ds4_media_comfy_avail_gib(ds4_media *m) {
    long kib = media_comfy_avail_kib(m);
    return kib < 0 ? -1 : kib / GIB_KIB;
}

/* Fabbisogno stimato: i picchi misurati il 25/09 per un'immagine, piu' ~2 GiB per
 * megapixel per ogni immagine in piu' nello stesso lavoro (stima, da misurare). */
static long media_need_kib(ds4_media_weights w, int width, int height, int n) {
    long px = (long)width * height;
    long gib;
    if (w == DS4_MEDIA_BF16) gib = px > 1024L * 1024 ? 37 : 32;
    else gib = 18;
    long mp = (px + 1024L * 1024 - 1) / (1024L * 1024);
    if (n > 1) gib += 2 * mp * (n - 1);
    return gib * GIB_KIB + MEDIA_MARGIN_KIB;
}

long ds4_media_image_need_gib(ds4_media_weights w, int width, int height, int n) {
    return (media_need_kib(w, width, height, n) + GIB_KIB - 1) / GIB_KIB;
}

bool media_mem_gate(ds4_media *m, long need_kib, const char *hint, char *err, size_t err_len) {
    if (m->no_gate) return true;
    long avail = media_comfy_avail_kib(m);
    if (avail < 0) return true;   /* non so leggere: non blocco */
    if (need_kib <= avail) return true;
    media_set_err(err, err_len, "servono ~%ld GiB per ComfyUI, disponibili %ld GiB (liberi piu' "
                  "quelli che ComfyUI tiene gia'): %s", (need_kib + GIB_KIB - 1) / GIB_KIB,
                  avail / GIB_KIB, hint);
    return false;
}

/* ── dimensioni ───────────────────────────────────────────────────────────── */

static bool media_size_ok(int w, int h) {
    return w > 0 && h > 0 && w % 32 == 0 && h % 32 == 0 &&
           w <= DS4_MEDIA_MAX_SIDE && h <= DS4_MEDIA_MAX_SIDE && (long)w * h <= DS4_MEDIA_MAX_AREA;
}

bool ds4_media_parse_size(const char *s, int *width, int *height) {
    /* strtol accetterebbe spazi e segni: si vogliono solo cifre, 'x', cifre. */
    if (!s || !isdigit((unsigned char)s[0])) return false;
    char *x = NULL, *end = NULL;
    errno = 0;
    long w = strtol(s, &x, 10);
    if (x == s || (*x != 'x' && *x != 'X') || !isdigit((unsigned char)x[1])) return false;
    long h = strtol(x + 1, &end, 10);
    if (end == x + 1 || *end || errno || w > DS4_MEDIA_MAX_SIDE || h > DS4_MEDIA_MAX_SIDE) return false;
    if (!media_size_ok((int)w, (int)h)) return false;
    *width = (int)w;
    *height = (int)h;
    return true;
}

/* Replica il ridimensionamento di TextEncodeQwenImage21: il riferimento va a circa
 * res x res pixel con le sue proporzioni, lati multipli di 32. Python arrotonda alla
 * pari (round), come nearbyint nel modo di arrotondamento di default. */
static void media_ref_scaled(int rw, int rh, int res, int *w, int *h) {
    double ratio = (double)rw / rh;
    *w = (int)nearbyint(sqrt((double)res * res * ratio) / 32.0) * 32;
    *h = (int)nearbyint(sqrt((double)res * res / ratio) / 32.0) * 32;
    if (*w < 32) *w = 32;
    if (*h < 32) *h = 32;
}

/* Per una modifica senza dimensioni esplicite: la risoluzione che conserva l'area del
 * primo riferimento (almeno MEDIA_EDIT_MIN_RES), ridotta finche' l'uscita rientra nei
 * limiti. Riferimento illeggibile: 1024, il default del nodo. 0 se nessuna risoluzione
 * rientra (proporzioni oltre ~86:1: un lato supererebbe 2752 anche a 32 sull'altro). */
static int media_edit_resolution(int rw, int rh, int *ow, int *oh) {
    if (rw <= 0 || rh <= 0) { rw = rh = 1024; }
    int res = (int)nearbyint(sqrt((double)rw * rh) / 32.0) * 32;
    if (res < MEDIA_EDIT_MIN_RES) res = MEDIA_EDIT_MIN_RES;
    for (;;) {
        media_ref_scaled(rw, rh, res, ow, oh);
        if (media_size_ok(*ow, *oh)) return res;
        if (res <= 32) return 0;
        res -= 32;
    }
}

/* ── grafo ────────────────────────────────────────────────────────────────── */

typedef struct {
    ds4_media_weights weights;
    long seed;
    int width, height;         /* dimensioni attese dell'uscita */
    int n, steps;
    double cfg;
    int resolution;            /* dei riferimenti in TextEncodeQwenImage21 */
    bool ref_latent;           /* campiona sul latente del nodo 4 (modifica senza size) */
    bool cache;                /* QwenImage21Cache tra UNET e KSampler */
    char **refs;
    int n_refs;
    char prefix[160];
} media_plan;

/* Avvolge il prompt nella formula RGBA ufficiale per lo sfondo trasparente. */
static char *media_wrap_transparent(const char *prompt) {
    media_buf b = {0};
    media_buf_puts(&b, "This is an RGBA format image with transparency. ");
    media_buf_puts(&b, prompt);
    media_buf_puts(&b, ". The image has an alpha channel and a transparent background.");
    return media_buf_take(&b);
}

/* L'oggetto JSON dei nodi. Id: 1-3 caricatori, 4 encoder, 5 latente, 6 KSampler,
 * 7 VAEDecode, 8 SaveImage, 9 cache dei riferimenti, 100+i LoadImage. */
static char *media_build_graph(const ds4_media_image_req *req, const media_plan *p) {
    const char *unet = p->weights == DS4_MEDIA_BF16 ? QI_UNET_BF16 : QI_UNET_INT8;
    const char *clip = p->weights == DS4_MEDIA_BF16 ? QI_CLIP_BF16 : QI_CLIP_INT8;
    char *eff = req->transparent ? media_wrap_transparent(req->prompt) : media_xstrdup(req->prompt);
    char *qprompt = media_json_quote(eff);
    char *qneg = media_json_quote(req->negative ? req->negative : "");
    char *qprefix = media_json_quote(p->prefix);
    free(eff);

    media_buf g = {0};
    char line[512];
    snprintf(line, sizeof(line),
             "{\"1\":{\"class_type\":\"UNETLoader\",\"inputs\":{\"unet_name\":\"%s\",\"weight_dtype\":\"default\"}},"
             "\"2\":{\"class_type\":\"CLIPLoader\",\"inputs\":{\"clip_name\":\"%s\",\"type\":\"qwen_image\",\"device\":\"default\"}},"
             "\"3\":{\"class_type\":\"VAELoader\",\"inputs\":{\"vae_name\":\"%s\"}},", unet, clip, QI_VAE);
    media_buf_puts(&g, line);
    media_buf_puts(&g, "\"4\":{\"class_type\":\"TextEncodeQwenImage21\",\"inputs\":{\"clip\":[\"2\",0],\"prompt\":");
    media_buf_puts(&g, qprompt);
    media_buf_puts(&g, ",\"negative_prompt\":");
    media_buf_puts(&g, qneg);
    snprintf(line, sizeof(line), ",\"resolution\":%d", p->resolution);
    media_buf_puts(&g, line);
    for (int i = 0; i < p->n_refs; i++) {
        snprintf(line, sizeof(line), ",\"images.image_%d\":[\"%d\",0]", i + 1, 100 + i);
        media_buf_puts(&g, line);
    }
    media_buf_puts(&g, ",\"vae\":[\"3\",0]}},");
    if (!p->ref_latent)
        snprintf(line, sizeof(line), "\"5\":{\"class_type\":\"EmptyLatentImage\",\"inputs\":"
                 "{\"width\":%d,\"height\":%d,\"batch_size\":%d}},", p->width, p->height, p->n);
    else if (p->n > 1)
        snprintf(line, sizeof(line), "\"5\":{\"class_type\":\"RepeatLatentBatch\",\"inputs\":"
                 "{\"samples\":[\"4\",2],\"amount\":%d}},", p->n);
    else
        line[0] = '\0';
    media_buf_puts(&g, line);
    const char *latent = (p->ref_latent && p->n == 1) ? "[\"4\",2]" : "[\"5\",0]";
    if (p->cache)
        media_buf_puts(&g, "\"9\":{\"class_type\":\"QwenImage21Cache\",\"inputs\":"
                           "{\"model\":[\"1\",0],\"device\":\"auto\",\"dtype\":\"int8\"}},");
    snprintf(line, sizeof(line),
             "\"6\":{\"class_type\":\"KSampler\",\"inputs\":{\"model\":[\"%s\",0],\"positive\":[\"4\",0],"
             "\"negative\":[\"4\",1],\"latent_image\":%s,\"seed\":%ld,\"steps\":%d,\"cfg\":%.3f,"
             "\"sampler_name\":\"euler\",\"scheduler\":\"simple\",\"denoise\":1.0}},",
             p->cache ? "9" : "1", latent, p->seed, p->steps, p->cfg);
    media_buf_puts(&g, line);
    media_buf_puts(&g, "\"7\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"6\",0],\"vae\":[\"3\",0]}},");
    media_buf_puts(&g, "\"8\":{\"class_type\":\"SaveImage\",\"inputs\":{\"images\":[\"7\",0],\"filename_prefix\":");
    media_buf_puts(&g, qprefix);
    media_buf_puts(&g, "}}");
    for (int i = 0; i < p->n_refs; i++) {
        char *qn = media_json_quote(p->refs[i]);
        snprintf(line, sizeof(line), ",\"%d\":{\"class_type\":\"LoadImage\",\"inputs\":{\"image\":", 100 + i);
        media_buf_puts(&g, line);
        media_buf_puts(&g, qn);
        media_buf_puts(&g, "}}");
        free(qn);
    }
    media_buf_puts(&g, "}");
    free(qprompt);
    free(qneg);
    free(qprefix);
    return media_buf_take(&g);
}

/* QwenImage21Cache e' sperimentale: lo si usa solo se il ComfyUI installato lo ha.
 * Una sola domanda per istanza; se ComfyUI non risponde si riprova la volta dopo. */
static bool media_has_cache_node(ds4_media *m) {
    if (m->cache_node >= 0) return m->cache_node == 1;
    media_http_response r = {0};
    char e[64];
    if (!media_http_get(m->host, m->port, "/object_info/QwenImage21Cache", 5000, &r, e, sizeof(e))) return false;
    m->cache_node = (r.status == 200 && r.body && strstr(r.body, "\"QwenImage21Cache\"")) ? 1 : 0;
    media_http_response_free(&r);
    return m->cache_node == 1;
}

/* ── il lavoro ────────────────────────────────────────────────────────────── */

/* Valida la richiesta e ne ricava il piano (dimensioni, cfg, risoluzione dei
 * riferimenti). false = errore del chiamante, con err. Non parla con ComfyUI. */
static bool media_image_plan(ds4_media *m, const ds4_media_image_req *req, media_plan *p,
                             char *err, size_t err_len) {
    if (!req->prompt || !req->prompt[0]) { media_set_err(err, err_len, "prompt vuoto"); return false; }
    if (strlen(req->prompt) > 4000) { media_set_err(err, err_len, "prompt troppo lungo (max 4000)"); return false; }
    if (req->negative && strlen(req->negative) > 4000) { media_set_err(err, err_len, "prompt negativo troppo lungo (max 4000)"); return false; }
    if (req->n_refs < 0 || req->n_refs > 10) { media_set_err(err, err_len, "da 0 a 10 riferimenti"); return false; }
    for (int i = 0; i < req->n_refs; i++)
        if (!req->refs || !req->refs[i] || !req->refs[i][0]) { media_set_err(err, err_len, "riferimento %d mancante", i + 1); return false; }
    if (req->n < 0 || req->n > DS4_MEDIA_MAX_N) { media_set_err(err, err_len, "da 1 a %d immagini per lavoro", DS4_MEDIA_MAX_N); return false; }
    if (req->steps < 0 || req->steps > 60) { media_set_err(err, err_len, "passi da 1 a 60"); return false; }
    if (!(req->cfg <= 20.0)) { media_set_err(err, err_len, "cfg oltre 20"); return false; }
    if (req->width < 0 || req->height < 0) { media_set_err(err, err_len, "dimensioni negative"); return false; }

    memset(p, 0, sizeof(*p));
    p->weights = req->weights == DS4_MEDIA_BF16 ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8;
    p->n = req->n > 0 ? req->n : 1;
    p->steps = req->steps > 0 ? req->steps : 25;
    p->seed = req->seed >= 0 ? req->seed : (long)(time(NULL) ^ ((long)getpid() << 8)) & 0x7fffffff;
    p->resolution = 1024;
    bool neg = req->negative && req->negative[0];
    if (req->cfg > 0) {
        p->cfg = req->cfg;
        if (neg && p->cfg <= 1.0) media_log(m, "ds4: media attenzione: con cfg %.2f ComfyUI ignora il prompt negativo", p->cfg);
    } else {
        p->cfg = neg ? DS4_MEDIA_NEG_CFG : 1.0;
        if (neg) media_log(m, "ds4: media prompt negativo: cfg %.1f (il campionamento costa circa il doppio)", p->cfg);
    }
    bool explicit_size = req->width > 0 || req->height > 0;
    if (req->n_refs > 0) {
        int rw = 0, rh = 0;
        if (!media_file_dims(req->refs[0], &rw, &rh))
            media_log(m, "ds4: media non leggo le dimensioni di %s: risoluzione 1024", req->refs[0]);
        p->resolution = media_edit_resolution(rw, rh, &p->width, &p->height);
        p->ref_latent = !explicit_size;
        if (!p->resolution) {
            media_set_err(err, err_len, "proporzioni del riferimento estreme (%dx%d): ritaglialo o dai --size", rw, rh);
            return false;
        }
    }
    if (explicit_size || req->n_refs == 0) {
        p->width = req->width > 0 ? req->width : 1024;
        p->height = req->height > 0 ? req->height : 1024;
        if (p->width % 32 || p->height % 32) { media_set_err(err, err_len, "dimensioni multiple di 32"); return false; }
        if (!media_size_ok(p->width, p->height)) {
            media_set_err(err, err_len, "dimensioni oltre i limiti: lato max %d, area max %ld pixel (2752x1536)",
                          DS4_MEDIA_MAX_SIDE, DS4_MEDIA_MAX_AREA);
            return false;
        }
    }
    return true;
}

static bool media_image_job(ds4_media *m, const ds4_media_image_req *req, media_job *j,
                            ds4_media_result *out, char *err, size_t err_len) {
    media_plan p;
    if (!media_image_plan(m, req, &p, err, err_len)) { out->invalid = true; return false; }

    if (!media_mem_gate(m, media_need_kib(p.weights, p.width, p.height, p.n),
                        "usa int8, riduci dimensioni o numero di immagini, o spegni un modello", err, err_len))
        return false;
    if (media_job_cancelled(m, j)) { media_set_err(err, err_len, "annullato"); return false; }

    char *names[10] = {0};
    for (int i = 0; i < req->n_refs; i++) {
        names[i] = media_upload_ref(m, req->refs[i], i, err, err_len);
        if (!names[i]) { for (int k = 0; k < i; k++) free(names[k]); return false; }
    }
    p.refs = names;
    p.n_refs = req->n_refs;
    p.cache = req->n_refs > 0 && media_has_cache_node(m);

    char stem[64];
    media_make_stem(stem, sizeof(stem), "img");
    snprintf(p.prefix, sizeof(p.prefix), "ds4/%s-%ld", stem, p.seed);
    char *nodes = media_build_graph(req, &p);
    bool ok = media_run(m, j, nodes, stem, 900000, out, err, err_len);   /* in coda puo' attendere */
    free(nodes);
    for (int i = 0; i < req->n_refs; i++) { media_forget_upload(m, names[i]); free(names[i]); }
    if (!ok) return false;
    if (!out->width) { out->width = p.width; out->height = p.height; }
    out->seed = p.seed;
    media_log(m, "ds4: media %s %dx%d seed=%ld n=%d in %ld ms (riferimenti %ld, coda %ld, generazione %ld, scarico %ld) -> %s",
              out->prompt_id, out->width, out->height, p.seed, p.n, out->ms, out->ms_upload,
              out->ms_queue, out->ms_run, out->ms_fetch, out->files[0]);
    return true;
}

bool ds4_media_image(ds4_media *m, const ds4_media_image_req *req,
                     ds4_media_result *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&m->lock);
    media_job j = {.cancel = req->cancel, .cancel_privdata = req->cancel_privdata, .t_start = media_now_ms()};
    media_job_bind(m, &j);
    bool ok = media_image_job(m, req, &j, out, err, err_len);
    media_job_bind(m, NULL);
    pthread_mutex_unlock(&m->lock);
    return ok;
}

/* ── servizio ─────────────────────────────────────────────────────────────── */

bool ds4_media_health(ds4_media *m, char *err, size_t err_len) {
    media_http_response resp = {0};
    if (!media_http_get(m->host, m->port, "/system_stats", 5000, &resp, err, err_len)) return false;
    bool ok = resp.status == 200;
    if (!ok) media_set_err(err, err_len, "ComfyUI ha risposto %d a /system_stats", resp.status);
    media_http_response_free(&resp);
    return ok;
}

static bool media_free_models_locked(ds4_media *m, char *err, size_t err_len) {
    const char *body = "{\"unload_models\":true,\"free_memory\":true}";
    media_http_response resp = {0};
    if (!media_http_post(m->host, m->port, "/free", "application/json", body, strlen(body),
                         10000, &resp, err, err_len)) return false;
    bool ok = resp.status == 200;
    if (!ok) media_set_err(err, err_len, "ComfyUI ha risposto %d a /free", resp.status);
    media_http_response_free(&resp);
    return ok;
}

bool ds4_media_free_models(ds4_media *m, char *err, size_t err_len) {
    pthread_mutex_lock(&m->lock);
    bool ok = media_free_models_locked(m, err, err_len);
    if (ok) m->last_job = 0;
    pthread_mutex_unlock(&m->lock);
    return ok;
}

bool ds4_media_idle_tick(ds4_media *m) {
    if (m->idle_free_sec <= 0) return false;
    if (pthread_mutex_trylock(&m->lock) != 0) return false;   /* lavoro in corso */
    bool freed = false;
    if (m->last_job && time(NULL) - m->last_job >= m->idle_free_sec) {
        char err[128] = {0};
        freed = media_free_models_locked(m, err, sizeof(err));
        if (freed) media_log(m, "ds4: media inattivo da %d s, modelli scaricati da ComfyUI (free)", m->idle_free_sec);
        else media_log(m, "ds4: media free automatico non riuscito: %s", err);
        m->last_job = 0;   /* in ogni caso non riprovare a ogni giro */
    }
    pthread_mutex_unlock(&m->lock);
    return freed;
}

/* Cartella di uscita, per il server che restituisce i file con response_format=url. */
const char *ds4_media_dir(const ds4_media *m) { return m->media_dir; }
