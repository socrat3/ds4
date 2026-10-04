/* ds4_media_storia - da una sceneggiatura a un video. Vedi ds4_media_storia.h.
 * Per ogni scena: fotogramma con Qwen-Image, clip con MiniMax H3 (stessa misura), poi
 * ffmpeg unisce le clip senza ricodificarle. Ogni file nasce intero (rinomina a fine
 * lavoro), cosi' la ripresa si fida della sua presenza. */
#include "ds4_media_storia.h"
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

void storia_libera(storia *s) {
    for (int i = 0; i < STORIA_MAX_SCENE; i++) {
        free(s->v[i].fotogramma);
        free(s->v[i].movimento);
    }
    memset(s, 0, sizeof(*s));
}

/* Una scena e' completa se ha secondi validi e i due testi. */
static bool scena_completa(const storia_scena *c, int k, char *err, size_t err_len) {
    if (!(c->secondi >= 0.1 && c->secondi <= 15)) {
        media_set_err(err, err_len, "scena %d: secondi mancanti o fuori da 0.1..15", k);
        return false;
    }
    if (!c->fotogramma || !c->fotogramma[0]) { media_set_err(err, err_len, "scena %d: manca fotogramma", k); return false; }
    if (!c->movimento || !c->movimento[0]) { media_set_err(err, err_len, "scena %d: manca movimento", k); return false; }
    return true;
}

bool storia_leggi(const char *path, storia *s, char *err, size_t err_len) {
    memset(s, 0, sizeof(*s));
    FILE *fp = fopen(path, "r");
    if (!fp) { media_set_err(err, err_len, "non riesco a leggere %s: %s", path, strerror(errno)); return false; }
    char line[8192];
    int riga = 0, cur = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof(line), fp)) {
        riga++;
        char *l = trim(line);
        if (!l[0] || l[0] == '#') continue;
        if (l[0] == '[') {
            int k;
            char chiusa;
            if (sscanf(l, "[scena %d%c", &k, &chiusa) != 2 || chiusa != ']') {
                media_set_err(err, err_len, "riga %d: intestazione non valida (atteso [scena N]): %s", riga, l);
                ok = false; break;
            }
            if (k != s->n + 1) {
                media_set_err(err, err_len, "riga %d: scena %d, attesa la scena %d (numerate di seguito da 1)", riga, k, s->n + 1);
                ok = false; break;
            }
            if (s->n && !scena_completa(&s->v[s->n - 1], s->n, err, err_len)) { ok = false; break; }
            if (s->n == STORIA_MAX_SCENE) { media_set_err(err, err_len, "al massimo %d scene", STORIA_MAX_SCENE); ok = false; break; }
            cur = ++s->n;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) { media_set_err(err, err_len, "riga %d: atteso chiave = valore: %s", riga, l); ok = false; break; }
        *eq = '\0';
        char *chiave = trim(l), *valore = trim(eq + 1);
        if (!cur) { media_set_err(err, err_len, "riga %d: %s prima di [scena 1]", riga, chiave); ok = false; break; }
        storia_scena *c = &s->v[cur - 1];
        if (!strcmp(chiave, "secondi")) {
            char *end = NULL;
            errno = 0;
            double v = strtod(valore, &end);
            if (errno || end == valore || *trim(end)) { media_set_err(err, err_len, "scena %d: secondi non e' un numero: %s", cur, valore); ok = false; break; }
            c->secondi = v;
        } else if (!strcmp(chiave, "fotogramma")) {
            free(c->fotogramma); c->fotogramma = media_xstrdup(valore);
        } else if (!strcmp(chiave, "movimento")) {
            free(c->movimento); c->movimento = media_xstrdup(valore);
        } else {
            media_set_err(err, err_len, "scena %d: chiave sconosciuta %s (secondi, fotogramma, movimento)", cur, chiave);
            ok = false; break;
        }
    }
    fclose(fp);
    if (ok && !s->n) { media_set_err(err, err_len, "%s non contiene scene ([scena 1] ...)", path); ok = false; }
    if (ok && !scena_completa(&s->v[s->n - 1], s->n, err, err_len)) ok = false;
    if (!ok) storia_libera(s);
    return ok;
}

static bool c_esiste(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Lascia memoria al prossimo lavoro: Qwen-Image e H3 stanno nella stessa ComfyUI. */
static void libera_se_serve(ds4_media *m, long need_gib) {
    long avail = ds4_media_comfy_avail_gib(m);
    if (avail < 0 || avail >= need_gib) return;
    char e[256] = {0};
    fprintf(stderr, "memoria: %ld GiB disponibili, ne servono ~%ld: libero i modelli di ComfyUI\n", avail, need_gib);
    if (!ds4_media_free_models(m, e, sizeof(e))) fprintf(stderr, "free non riuscito: %s\n", e);
}

/* Sposta il file prodotto da ds4-media nel nome della scena. */
static bool sposta(ds4_media_result *r, const char *dest, char *err, size_t err_len) {
    if (!r->n_files) { media_set_err(err, err_len, "nessun file prodotto per %s", dest); return false; }
    if (rename(r->files[0], dest) != 0) {
        media_set_err(err, err_len, "non riesco a spostare %s in %s: %s", r->files[0], dest, strerror(errno));
        return false;
    }
    return true;
}

static bool cancellato(const storia_opzioni *o) { return o->cancel && o->cancel(o->cancel_privdata); }

/* Lista per il concat demuxer di ffmpeg. I nomi sono relativi: ffmpeg li risolve dalla
 * cartella della lista, che e' la stessa delle clip, anche se --dir e' relativa. */
static bool scrivi_lista(const char *lista, int n, char *err, size_t err_len) {
    FILE *fp = fopen(lista, "w");
    if (!fp) { media_set_err(err, err_len, "non riesco a scrivere %s", lista); return false; }
    for (int k = 1; k <= n; k++) fprintf(fp, "file 'scena-%02d.mp4'\n", k);
    return fclose(fp) == 0;
}

int storia_esegui(ds4_media *m, const storia *s, const storia_opzioni *o,
                  char *out, size_t out_len, char *err, size_t err_len) {
    int w = o->width ? o->width : 864, h = o->height ? o->height : 480;
    int da = o->da > 0 ? o->da : 1;
    char png[PATH_MAX], mp4[PATH_MAX];
    if (da > s->n) { media_set_err(err, err_len, "--da %d oltre le %d scene", da, s->n); return 1; }
    for (int k = 1; k < da; k++) {
        snprintf(mp4, sizeof(mp4), "%s/scena-%02d.mp4", o->dir, k);
        if (!c_esiste(mp4)) { media_set_err(err, err_len, "--da %d: manca %s", da, mp4); return 1; }
    }
    for (int k = da; k <= s->n; k++) {
        const storia_scena *c = &s->v[k - 1];
        snprintf(png, sizeof(png), "%s/scena-%02d.png", o->dir, k);
        snprintf(mp4, sizeof(mp4), "%s/scena-%02d.mp4", o->dir, k);
        if (cancellato(o)) { media_set_err(err, err_len, "annullato"); return 130; }
        if (c_esiste(mp4)) { fprintf(stderr, "[%d/%d] gia' fatta: %s\n", k, s->n, mp4); continue; }
        ds4_media_result r = {0};
        if (!c_esiste(png)) {
            fprintf(stderr, "[%d/%d] fotogramma (Qwen-Image): \"%.70s\"\n", k, s->n, c->fotogramma);
            ds4_media_image_req ir = {.prompt = c->fotogramma, .width = w, .height = h, .steps = o->steps,
                                      .seed = o->seed < 0 ? -1 : o->seed + k, .weights = ds4_media_default_weights(m)};
            libera_se_serve(m, ds4_media_image_need_gib(ir.weights, w, h, 1));
            bool ok = ds4_media_image(m, &ir, &r, err, err_len) && sposta(&r, png, err, err_len);
            ds4_media_result_free(&r);
            if (!ok) return cancellato(o) ? 130 : 1;
        }
        fprintf(stderr, "[%d/%d] clip di %.1f s (MiniMax H3, dura minuti): \"%.70s\"\n", k, s->n, c->secondi, c->movimento);
        ds4_media_video_req vr = {.prompt = c->movimento, .ref = png, .seconds = c->secondi, .width = w, .height = h,
                                  .steps = o->steps, .seed = o->seed < 0 ? -1 : o->seed + k};
        libera_se_serve(m, DS4_MEDIA_VIDEO_NEED_GIB);
        bool ok = ds4_media_video(m, &vr, &r, err, err_len) && sposta(&r, mp4, err, err_len);
        ds4_media_result_free(&r);
        if (!ok) return cancellato(o) ? 130 : 1;
    }
    char lista[PATH_MAX], log[PATH_MAX];
    snprintf(lista, sizeof(lista), "%s/storia.lista", o->dir);
    snprintf(log, sizeof(log), "%s/storia-ffmpeg.log", o->dir);
    snprintf(out, out_len, "%s/storia.mp4", o->dir);
    if (!scrivi_lista(lista, s->n, err, err_len)) return 1;
    fprintf(stderr, "unisco %d clip in %s\n", s->n, out);
    /* Le clip di H3 con la stessa misura hanno gli stessi parametri: si uniscono senza
     * ricodifica. Se ffmpeg rifiuta, si ricodifica (piu' lento, sempre corretto). */
    const char *copia[24] = {o->ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
                             "-i", lista, "-c", "copy", "-movflags", "+faststart"};
    if (doppia_ffmpeg(copia, 15, out, log, o->cancel, o->cancel_privdata, err, err_len)) return 0;
    if (cancellato(o)) return 130;
    fprintf(stderr, "unione senza ricodifica non riuscita: ricodifico\n");
    const char *ricod[28] = {o->ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
                             "-i", lista, "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "aac",
                             "-movflags", "+faststart"};
    if (doppia_ffmpeg(ricod, 21, out, log, o->cancel, o->cancel_privdata, err, err_len)) return 0;
    return cancellato(o) ? 130 : 1;
}

/* ── comando ─────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; if (g_stop) _exit(130); g_stop = 1; }
static bool cli_cancel(void *pd) { (void)pd; return g_stop != 0; }
static void cli_log(void *pd, const char *msg) { (void)pd; fprintf(stderr, "%s\n", msg); }
static void cli_progress(void *pd, int step, int total) {
    (void)pd;
    if (!isatty(2)) return;
    fprintf(stderr, "\r  passo %d/%d %s", step, total, step >= total ? "\n" : "");
}

/* Eseguibile: con '/' deve esistere ed essere eseguibile, altrimenti si cerca nel PATH. */
static bool eseguibile(const char *p) {
    if (!p || !p[0]) return false;
    if (strchr(p, '/')) return access(p, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char buf[PATH_MAX];
    for (const char *a = path; *a;) {
        const char *b = strchr(a, ':');
        size_t n = b ? (size_t)(b - a) : strlen(a);
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)n, a, p);
        if (n && access(buf, X_OK) == 0) return true;
        a = b ? b + 1 : a + n;
    }
    return false;
}

int ds4_media_storia_cli(int argc, char **argv) {
    const char *file = NULL, *dir = NULL, *size = NULL, *ffmpeg = NULL, *host = "127.0.0.1";
    long port = 8188, steps = 0, seed = -1, da = 1;
    bool bf16 = false, tieni = false, no_gate = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end = NULL;
        #define VAL() do { if (!v) { fprintf(stderr, "ds4-media: %s vuole un valore\n", a); return 2; } i++; } while (0)
        #define NUM(var, lo, hi) do { VAL(); errno = 0; var = strtol(v, &end, 10); \
            if (errno || !*v || *end || var < (lo) || var > (hi)) { fprintf(stderr, "ds4-media: %s non valido: %s\n", a, v); return 2; } } while (0)
        if (!strcmp(a, "--dir")) { VAL(); dir = v; }
        else if (!strcmp(a, "--size")) { VAL(); size = v; }
        else if (!strcmp(a, "--ffmpeg")) { VAL(); ffmpeg = v; }
        else if (!strcmp(a, "--host")) { VAL(); host = v; }
        else if (!strcmp(a, "--comfy-port")) NUM(port, 1, 65535);
        else if (!strcmp(a, "--passi")) NUM(steps, 1, 60);
        else if (!strcmp(a, "--seed")) NUM(seed, -1, 0x7fffffffL);
        else if (!strcmp(a, "--da")) NUM(da, 1, STORIA_MAX_SCENE);
        else if (!strcmp(a, "--bf16")) bf16 = true;
        else if (!strcmp(a, "--tieni-comfy")) tieni = true;
        else if (!strcmp(a, "--no-gate")) no_gate = true;
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "ds4-media: opzione sconosciuta %s (ds4-media help storia)\n", a); return 2; }
        else if (file) { fprintf(stderr, "ds4-media: una sola sceneggiatura (%s, %s)\n", file, a); return 2; }
        else file = a;
        #undef NUM
        #undef VAL
    }
    if (!file) { fprintf(stderr, "uso: ds4-media storia SCENEGGIATURA [--dir D] [opzioni] (ds4-media help storia)\n"); return 2; }
    storia s;
    char err[512] = {0};
    if (!storia_leggi(file, &s, err, sizeof(err))) { fprintf(stderr, "ds4-media: %s\n", err); return 2; }
    int w = 0, h = 0;
    if (size && !ds4_media_parse_size(size, &w, &h)) { fprintf(stderr, "ds4-media: size non valida: %s\n", size); storia_libera(&s); return 2; }
    /* ffmpeg prima di tutto: scoprirlo assente dopo mezz'ora di GPU sarebbe uno spreco. */
    doppia_conf conf;
    doppia_conf_init(&conf);
    if (!ffmpeg) ffmpeg = doppia_get(&conf, "ffmpeg");
    if (!eseguibile(ffmpeg)) {
        fprintf(stderr, "ds4-media: ffmpeg non trovato (%s): indicalo con --ffmpeg o nella voce ffmpeg di %s/doppia.conf\n",
                ffmpeg && ffmpeg[0] ? ffmpeg : "vuoto", ds4_media_home());
        doppia_conf_free(&conf); storia_libera(&s); return 2;
    }
    char cartella[PATH_MAX];
    if (dir) snprintf(cartella, sizeof(cartella), "%s", dir);
    else {
        snprintf(cartella, sizeof(cartella), "%s", file);
        char *sl = strrchr(cartella, '/');
        if (sl) *sl = '\0'; else snprintf(cartella, sizeof(cartella), ".");
    }
    if (mkdir(cartella, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "ds4-media: non riesco a creare %s: %s\n", cartella, strerror(errno));
        doppia_conf_free(&conf); storia_libera(&s); return 1;
    }
    double tot = 0;
    for (int k = 0; k < s.n; k++) tot += s.v[k].secondi;
    fprintf(stderr, "storia: %d scene, %.0f s di video, in %s\n", s.n, tot, cartella);

    static char comfy_out[PATH_MAX];
    const char *home = getenv("HOME");
    ds4_media_config mc = {.host = host, .port = (int)port, .media_dir = cartella, .no_gate = no_gate,
                           .weights = bf16 ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8, .log = cli_log, .cancel = cli_cancel,
                           .progress = cli_progress};
    if (!tieni && home && (!strcmp(host, "127.0.0.1") || !strcmp(host, "localhost"))) {
        snprintf(comfy_out, sizeof(comfy_out), "%s/comfy/ComfyUI/output", home);
        mc.comfy_output_dir = comfy_out;
    }
    ds4_media *m = ds4_media_create(&mc);
    if (!m) { doppia_conf_free(&conf); storia_libera(&s); return 2; }
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    storia_opzioni o = {.dir = cartella, .width = w, .height = h, .steps = (int)steps, .seed = seed,
                        .da = (int)da, .ffmpeg = ffmpeg, .cancel = cli_cancel};
    char out[PATH_MAX];
    int rc = storia_esegui(m, &s, &o, out, sizeof(out), err, sizeof(err));
    if (rc == 0) printf("%s\n", out);
    else fprintf(stderr, "ds4-media: %s%s\n", err, rc == 1 ? " (rilancia lo stesso comando per riprendere)" : "");
    ds4_media_free(m);
    doppia_conf_free(&conf);
    storia_libera(&s);
    return rc;
}
