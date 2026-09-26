/* ds4-media - eseguibile autonomo del modulo immagini/video, senza LLM ne' GPU.
 *   ds4-media serve [--port 8010] [--host H] [--comfy-port 8188] [--dir D] [--bf16] [--idle-free S]
 *   ds4-media img [--size WxH] [--seed S] [--passi N] [--n N] [--cfg C] [--negativo T]
 *                 [--bf16] [--trasparente] [--rif F]... [--] descrizione...
 *   ds4-media video --rif F [--sec S] [--size WxH] [--seed S] [--passi N] [--] descrizione...
 *   ds4-media clean [--dir D] [--comfy] [--no-log] [--forza]
 *   ds4-media health | free
 * Bersaglio fisso per Open WebUI (serve) e prova da riga di comando (img, video).
 *
 * Gli argomenti si leggono in un solo passaggio, e ogni opzione sa se vuole un valore:
 * la descrizione e' tutto cio' che non e' opzione (o che segue "--"), in qualsiasi
 * posizione. Cosi' `img "gatto" --bf16` funziona e `img --rif foto.png` senza
 * descrizione e' un errore, invece di usare il percorso come descrizione. */
#include "ds4_media.h"
#include "ds4_media_doppia.h"
#include "ds4_media_help.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

/* Il primo Ctrl+C annulla il lavoro (ds4_media lo toglie dalla coda di ComfyUI); il
 * secondo esce subito, per chi non vuole aspettare nemmeno quello. */
static void on_signal(int sig) {
    (void)sig;
    if (g_stop) {
        /* il programma esterno di doppia (ffmpeg, whisper, la sintesi) non deve restare
         * orfano con la GPU in mano: kill e' sicuro in un gestore di segnali */
        if (doppia_figlio > 0) kill(-doppia_figlio, SIGTERM);
        _exit(130);
    }
    g_stop = 1;
}

static bool cli_cancelled(void *pd) { (void)pd; return g_stop != 0; }

static void logline(void *pd, const char *msg) { (void)pd; fprintf(stderr, "%s\n", msg); }

static int g_progress_shown;
static void cli_progress(void *pd, int step, int total) {
    (void)pd;
    if (!isatty(2)) return;
    fprintf(stderr, "\r  passo %d/%d ", step, total);
    g_progress_shown = 1;
}

/* ── argomenti ────────────────────────────────────────────────────────────── */

typedef struct { const char *name; bool value; } cli_opt;
static const cli_opt OPTS[] = {
    {"--port", true}, {"--host", true}, {"--comfy-port", true}, {"--dir", true},
    {"--idle-free", true}, {"--size", true}, {"--seed", true}, {"--passi", true},
    {"--rif", true}, {"--sec", true}, {"--negativo", true}, {"--cfg", true}, {"--n", true},
    {"--da", true}, {"--a", true}, {"--resa", true}, {"--riprendi", true}, {"--voce", true},
    {"--si", false}, {"--senza-titolo", false}, {"--help", false}, {"-h", false},
    {"--bf16", false}, {"--trasparente", false}, {"--no-free", false}, {"--auto-free", false},
    {"--no-vista", false}, {"--tieni-comfy", false}, {"--no-gate", false}, {"--forza", false},
    {"-y", false}, {"--comfy", false}, {"--tutto", false}, {"--no-log", false},
};
#define N_OPTS ((int)(sizeof(OPTS) / sizeof(OPTS[0])))

typedef struct {
    const char *val[N_OPTS];   /* valore (o "" per i flag) dell'ultima occorrenza */
    const char *refs[10];
    int nref;
    char prompt[8192];         /* le parole della descrizione, unite da spazi */
} cli_args;

static bool cli_parse(int argc, char **argv, cli_args *a) {
    memset(a, 0, sizeof(*a));
    size_t plen = 0;
    bool only_words = false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (!only_words && !strcmp(s, "--")) { only_words = true; continue; }
        if (!only_words && s[0] == '-' && s[1]) {
            int k = 0;
            while (k < N_OPTS && strcmp(OPTS[k].name, s)) k++;
            if (k == N_OPTS) { fprintf(stderr, "ds4-media: opzione sconosciuta %s\n", s); return false; }
            if (!OPTS[k].value) { a->val[k] = ""; continue; }
            if (i + 1 >= argc) { fprintf(stderr, "ds4-media: %s vuole un valore\n", s); return false; }
            a->val[k] = argv[++i];
            if (!strcmp(s, "--rif")) {
                if (a->nref == 10) { fprintf(stderr, "ds4-media: al massimo 10 --rif\n"); return false; }
                a->refs[a->nref++] = a->val[k];
            }
            continue;
        }
        size_t n = strlen(s);
        if (plen + n + 2 > sizeof(a->prompt)) { fprintf(stderr, "ds4-media: descrizione troppo lunga\n"); return false; }
        if (plen) a->prompt[plen++] = ' ';
        memcpy(a->prompt + plen, s, n + 1);
        plen += n;
    }
    return true;
}

static const char *opt(const cli_args *a, const char *name, const char *def) {
    for (int k = 0; k < N_OPTS; k++)
        if (!strcmp(OPTS[k].name, name)) return a->val[k] ? a->val[k] : def;
    return def;
}
static bool flag(const cli_args *a, const char *name) { return opt(a, name, NULL) != NULL; }

/* Numeri: tutta la stringa deve essere un numero nei limiti, altrimenti errore. */
static bool opt_long(const cli_args *a, const char *name, long def, long lo, long hi, long *out) {
    const char *s = opt(a, name, NULL);
    if (!s) { *out = def; return true; }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno || v < lo || v > hi) {
        fprintf(stderr, "ds4-media: %s non valido: %s\n", name, s);
        return false;
    }
    *out = v;
    return true;
}
static bool opt_double(const cli_args *a, const char *name, double def, double lo, double hi, double *out) {
    const char *s = opt(a, name, NULL);
    if (!s) { *out = def; return true; }
    /* strtod accetterebbe anche 0x10, 1e1, inf: qui solo cifre e un punto. */
    if (s[strspn(s, "0123456789.")] || !strchr("0123456789.", s[0])) {
        fprintf(stderr, "ds4-media: %s non valido: %s\n", name, s);
        return false;
    }
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end || !(v >= lo && v <= hi)) {
        fprintf(stderr, "ds4-media: %s non valido: %s\n", name, s);
        return false;
    }
    *out = v;
    return true;
}

/* ── interazione ──────────────────────────────────────────────────────────── */

/* Domanda s/n sul terminale; default_yes = risposta con solo Invio. Fuori da un
 * terminale (script) non chiede e restituisce il default. */
static bool ask(const char *question, bool default_yes) {
    if (!isatty(0)) return default_yes;
    fprintf(stderr, "%s [%s] ", question, default_yes ? "S/n" : "s/N");
    char line[16];
    if (!fgets(line, sizeof(line), stdin)) return default_yes;
    if (line[0] == '\n') return default_yes;
    return line[0] == 's' || line[0] == 'S' || line[0] == 'y' || line[0] == 'Y';
}

/* Se la memoria per ComfyUI e' sotto il fabbisogno, offre di scaricarne i modelli
 * (/free). Con --auto-free libera senza chiedere; con --no-free non chiede. */
static void offri_free(ds4_media *m, const cli_args *a, long need_gib, const char *cosa) {
    if (flag(a, "--no-free")) return;
    long avail = ds4_media_comfy_avail_gib(m);
    if (avail < 0 || avail >= need_gib) return;
    fprintf(stderr, "memoria: %ld GiB disponibili per ComfyUI, per %s ne servono ~%ld.\n", avail, cosa, need_gib);
    if (!flag(a, "--auto-free") && !ask("Libero i modelli caricati in ComfyUI (free)?", true)) return;
    char err[256] = {0};
    if (ds4_media_free_models(m, err, sizeof(err)))
        fprintf(stderr, "memoria liberata: ora %ld GiB.\n", ds4_media_comfy_avail_gib(m));
    else
        fprintf(stderr, "free non riuscito: %s\n", err);
}

/* Apre il file col visualizzatore di sistema. Niente shell: il percorso va a execlp
 * come argomento, spazi e caratteri speciali compresi. Doppio fork, cosi' il
 * visualizzatore non resta figlio nostro e non serve aspettarlo. */
static void offri_vista(const cli_args *a, const char *path) {
    if (!path || flag(a, "--no-vista")) return;
    if (!ask("Vuoi visualizzare il risultato?", false)) return;
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
            execlp("xdg-open", "xdg-open", path, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid < 0) { fprintf(stderr, "non riesco ad aprire il visualizzatore\n"); return; }
    waitpid(pid, NULL, 0);
}

static void on_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;   /* senza SA_RESTART: le attese si svegliano e vedono g_stop */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);   /* terminale chiuso: come un Ctrl+C, il lavoro si ferma pulito */
}

static ds4_media *build(const cli_args *a) {
    static ds4_media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    long port, idle;
    if (!opt_long(a, "--comfy-port", 8188, 1, 65535, &port) ||
        !opt_long(a, "--idle-free", 0, 0, 1000000, &idle)) return NULL;
    cfg.host = opt(a, "--host", "127.0.0.1");
    cfg.port = (int)port;
    cfg.media_dir = opt(a, "--dir", NULL);
    /* Su host locale togli la copia doppia di ComfyUI: un solo file, niente sparpagliamento.
     * Con --tieni-comfy la si conserva; con host remoto non si tocca (path non locale). */
    static char comfy_out[1024];
    const char *home = getenv("HOME");
    bool local = !strcmp(cfg.host, "127.0.0.1") || !strcmp(cfg.host, "localhost");
    if (local && home && !flag(a, "--tieni-comfy")) {
        snprintf(comfy_out, sizeof(comfy_out), "%s/comfy/ComfyUI/output", home);
        cfg.comfy_output_dir = comfy_out;
    }
    cfg.weights = flag(a, "--bf16") ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8;
    cfg.idle_free_sec = (int)idle;
    cfg.no_gate = flag(a, "--no-gate");
    cfg.log = logline;
    cfg.cancel = cli_cancelled;
    cfg.progress = cli_progress;
    return ds4_media_create(&cfg);
}

static void print_result(const ds4_media_result *r) {
    printf("%dx%d seed=%ld in %ld ms (coda %ld, generazione %ld, scarico %ld)\n",
           r->width, r->height, r->seed, r->ms, r->ms_queue, r->ms_run, r->ms_fetch);
    for (int i = 0; i < r->n_files; i++) printf("  %s\n", r->files[i]);
}

/* ── comandi ──────────────────────────────────────────────────────────────── */

static int cmd_serve(const cli_args *a) {
    long port;
    if (!opt_long(a, "--port", 8010, 1, 65535, &port)) return 2;
    ds4_media *m = build(a);
    if (!m) return 2;
    on_signals();
    char err[256] = {0};
    bool ok = ds4_media_serve(m, (int)port, (volatile int *)&g_stop, err, sizeof(err));
    if (!ok) fprintf(stderr, "ds4-media: %s\n", err);
    if (ok) ds4_media_free(m);   /* altrimenti un thread la usa ancora: esce il processo */
    return ok ? 0 : 1;
}

static int cmd_img(const cli_args *a) {
    ds4_media_image_req req = {0};
    long seed, steps, n;
    double cfgv;
    if (!opt_long(a, "--seed", -1, -1, 0x7fffffffffffffffL, &seed) ||
        !opt_long(a, "--passi", 0, 1, 60, &steps) ||
        !opt_long(a, "--n", 1, 1, DS4_MEDIA_MAX_N, &n) ||
        !opt_double(a, "--cfg", 0, 0.1, 20, &cfgv)) return 2;
    const char *size = opt(a, "--size", NULL);
    if (size && !ds4_media_parse_size(size, &req.width, &req.height)) {
        fprintf(stderr, "ds4-media: size non valida: %s (WxH multipli di 32, lato max 2752, area max 2752x1536)\n", size);
        return 2;
    }
    if (!a->prompt[0]) { fprintf(stderr, "uso: ds4-media img [opzioni] [--] descrizione... (ds4-media help img)\n"); return 2; }
    ds4_media *m = build(a);
    if (!m) return 2;
    on_signals();
    req.weights = ds4_media_default_weights(m);   /* --bf16 e' gia' nella config */
    req.seed = seed;
    req.steps = (int)steps;
    req.n = (int)n;
    req.cfg = cfgv;
    req.negative = opt(a, "--negativo", NULL);
    req.transparent = flag(a, "--trasparente");
    req.refs = a->refs;
    req.n_refs = a->nref;
    req.prompt = a->prompt;

    int w = req.width ? req.width : 1024, h = req.height ? req.height : 1024;
    offri_free(m, a, ds4_media_image_need_gib(req.weights, w, h, req.n), "un'immagine");
    fprintf(stderr, "genero l'immagine con Qwen-Image (ComfyUI): \"%.60s\"%s\n",
            a->prompt, strlen(a->prompt) > 60 ? "..." : "");
    ds4_media_result res = {0};
    char err[256] = {0};
    bool ok = ds4_media_image(m, &req, &res, err, sizeof(err));
    if (g_progress_shown) fputc('\n', stderr);
    if (ok) {
        print_result(&res);
        if (res.n_files) offri_vista(a, res.files[0]);
    } else {
        fprintf(stderr, "ds4-media: %s\n", err);
    }
    int rc = ok ? 0 : g_stop ? 130 : res.invalid ? 2 : 1;
    ds4_media_result_free(&res);
    ds4_media_free(m);
    return rc;
}

static int cmd_video(const cli_args *a) {
    ds4_media_video_req req = {0};
    long seed, steps;
    if (!opt_long(a, "--seed", -1, -1, 0x7fffffffffffffffL, &seed) ||
        !opt_long(a, "--passi", 0, 1, 60, &steps) ||
        !opt_double(a, "--sec", 0, 0.1, 15, &req.seconds)) return 2;
    const char *size = opt(a, "--size", NULL);
    if (size && !ds4_media_parse_size(size, &req.width, &req.height)) {
        fprintf(stderr, "ds4-media: size non valida: %s\n", size);
        return 2;
    }
    if (!a->prompt[0] || a->nref != 1) {
        fprintf(stderr, "uso: ds4-media video --rif primo_fotogramma.png [--sec S] [--size WxH] [--] descrizione...\n"
                        "(ds4-media help video)\n");
        return 2;
    }
    ds4_media *m = build(a);
    if (!m) return 2;
    on_signals();
    req.seed = seed;
    req.steps = (int)steps;
    req.ref = a->refs[0];
    req.prompt = a->prompt;
    offri_free(m, a, DS4_MEDIA_VIDEO_NEED_GIB, "un video H3");
    fprintf(stderr, "genero il video con MiniMax H3 (occupa la GPU, dura minuti): \"%.50s\"%s\n",
            a->prompt, strlen(a->prompt) > 50 ? "..." : "");
    ds4_media_result res = {0};
    char err[256] = {0};
    bool ok = ds4_media_video(m, &req, &res, err, sizeof(err));
    if (g_progress_shown) fputc('\n', stderr);
    if (ok) {
        print_result(&res);
        if (res.n_files) offri_vista(a, res.files[0]);
    } else {
        fprintf(stderr, "ds4-media: %s\n", err);
    }
    ds4_media_result_free(&res);
    ds4_media_free(m);
    return ok ? 0 : (g_stop ? 130 : 1);
}

/* Un file da cancellare e' generato da ds4-media se: immagine (img-*.png),
 * video (vid-*.mp4) o, con with_log, un log di generazione (*.log). Mai altro. */
static bool clean_match(const char *name, bool with_log) {
    size_t n = strlen(name);
    if (!strncmp(name, "img-", 4) && n > 8 && !strcmp(name + n - 4, ".png")) return true;
    if (!strncmp(name, "vid-", 4) && n > 8 && !strcmp(name + n - 4, ".mp4")) return true;
    if (with_log && n > 4 && !strcmp(name + n - 4, ".log")) return true;
    return false;
}

/* Elenca i file corrispondenti in `dir`; con do_delete li cancella. Ritorna il numero
 * di file, accumula i byte in *bytes. Non ricorsivo, mai su cartelle ne' link. */
static int clean_dir(const char *dir, bool with_log, bool do_delete, long long *bytes) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    char path[2048];
    while ((e = readdir(d)) != NULL) {
        if (!clean_match(e->d_name, with_log)) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        n++;
        if (bytes) *bytes += st.st_size;
        if (do_delete && unlink(path) != 0)
            fprintf(stderr, "  non cancello %s\n", path);
    }
    closedir(d);
    return n;
}

static int cmd_clean(const cli_args *a) {
    ds4_media *m = build(a);
    if (!m) return 2;
    const char *dir = ds4_media_dir(m);
    bool with_log = !flag(a, "--no-log");     /* i log si cancellano salvo --no-log */
    bool do_comfy = flag(a, "--comfy") || flag(a, "--tutto");
    bool forza = flag(a, "--forza") || flag(a, "-y");
    const char *home = getenv("HOME");
    char comfy[1024] = {0};
    if (do_comfy && home) snprintf(comfy, sizeof(comfy), "%s/comfy/ComfyUI/output/ds4", home);

    long long bytes = 0;
    int n = clean_dir(dir, with_log, false, &bytes);
    int nc = comfy[0] ? clean_dir(comfy, false, false, &bytes) : 0;
    if (n + nc == 0) {
        fprintf(stderr, "niente da cancellare in %s%s.\n", dir, comfy[0] ? " (e nelle copie di ComfyUI)" : "");
        ds4_media_free(m);
        return 0;
    }
    fprintf(stderr, "cancellazione: %d file in %s%s%s, %.1f MB.\n", n + nc, dir,
            nc ? " + " : "", nc ? comfy : "", bytes / 1e6);
    fprintf(stderr, "  (immagini img-*.png, video vid-*.mp4%s)\n", with_log ? ", log *.log" : "");
    if (!forza && !ask("Procedo con la cancellazione?", false)) {
        fprintf(stderr, "annullato.\n");
        ds4_media_free(m);
        return 0;
    }
    n = clean_dir(dir, with_log, true, NULL);
    nc = comfy[0] ? clean_dir(comfy, false, true, NULL) : 0;
    fprintf(stderr, "cancellati %d file.\n", n + nc);
    ds4_media_free(m);
    return 0;
}

/* ds4-media doppia: il wizard (se al terminale e senza --si) fa rivedere e cambiare
 * ogni scelta; poi le fasi mancanti del lavoro. Le opzioni della riga di comando
 * vincono sul doppia.conf del lavoro, che vince su ~/.ds4/doppia.conf. */
static int cmd_doppia(const cli_args *a) {
    static doppia_conf c;
    doppia_conf_init(&c);
    const char *ov[24];
    int k = 0;
    if (a->prompt[0]) { ov[k++] = "sorgente"; ov[k++] = a->prompt; }
    const char *keys[][2] = {{"--da", "da"}, {"--a", "a"}, {"--resa", "resa"}, {"--voce", "voce_motore"}};
    for (int i = 0; i < 4; i++)
        if (opt(a, keys[i][0], NULL)) { ov[k++] = keys[i][1]; ov[k++] = opt(a, keys[i][0], NULL); }
    if (flag(a, "--senza-titolo")) { ov[k++] = "titolo"; ov[k++] = "no"; }
    ov[k] = NULL;
    const char *dir = opt(a, "--riprendi", opt(a, "--dir", NULL));
    if (dir) snprintf(c.dir, sizeof(c.dir), "%s", dir);
    if (!a->prompt[0] && !dir) {
        fprintf(stderr, "uso: ds4-media doppia URL|video [opzioni], o --riprendi CARTELLA (ds4-media help doppia)\n");
        doppia_conf_free(&c);
        return 2;
    }
    char err[512] = {0}, conf[1100];
    if (opt(a, "--riprendi", NULL)) {
        snprintf(conf, sizeof(conf), "%s/doppia.conf", dir);
        if (access(conf, R_OK) != 0) {
            fprintf(stderr, "ds4-media: %s non e' la cartella di un lavoro (manca doppia.conf)\n", dir);
            doppia_conf_free(&c);
            return 2;
        }
    }
    if (!doppia_prepara(&c, ov, err, sizeof(err))) { fprintf(stderr, "ds4-media: %s\n", err); doppia_conf_free(&c); return 1; }
    on_signals();
    if (!flag(a, "--si") && isatty(0) && !doppia_wizard(&c, doppia_titoli_path(&c))) {
        fprintf(stderr, "uscito senza partire: le scelte non sono state salvate\n");
        doppia_conf_free(&c);
        return 0;
    }
    if (!doppia_get(&c, "foto")[0] || !doppia_get(&c, "voce_campione")[0]) {
        fprintf(stderr, "ds4-media: servono foto e voce_campione (wizard, oppure ~/.ds4/doppia.conf)\n");
        doppia_conf_free(&c);
        return 2;
    }
    snprintf(conf, sizeof(conf), "%s/doppia.conf", c.dir);
    if (!doppia_conf_save(&c, conf, err, sizeof(err))) fprintf(stderr, "ds4-media: %s\n", err);
    fprintf(stderr, "lavoro in %s (log: doppia.log); Ctrl+C ferma, lo stesso comando riprende\n", c.dir);
    bool ok = doppia_run(&c, cli_cancelled, NULL, err, sizeof(err));
    if (!ok) fprintf(stderr, "ds4-media: %s\nper riprendere: ds4-media doppia --riprendi %s\n", err, c.dir);
    doppia_conf_free(&c);
    return ok ? 0 : g_stop ? 130 : 1;
}

static int cmd_simple(const cli_args *a, bool health) {
    ds4_media *m = build(a);
    if (!m) return 2;
    char err[256] = {0};
    bool ok = health ? ds4_media_health(m, err, sizeof(err))
                     : ds4_media_free_models(m, err, sizeof(err));
    printf("%s\n", ok ? "ok" : err);
    ds4_media_free(m);
    return ok ? 0 : 1;
}

/* --help o -h prima di un eventuale "--": dopo sono parole della descrizione. */
static bool vuole_aiuto(int argc, char **argv) {
    for (int i = 2; i < argc && strcmp(argv[i], "--"); i++)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return true;
    return false;
}

static const char *COMANDI[] = {"serve", "img", "video", "doppia", "clean", "health", "free", NULL};

int main(int argc, char **argv) {
    if (argc < 2) { ds4_media_help(stderr, NULL); return 2; }
    const char *c = argv[1];
    if (!strcmp(c, "help") || !strcmp(c, "--help") || !strcmp(c, "-h"))
        return ds4_media_help(stdout, argc > 2 ? argv[2] : NULL);
    bool noto = false;
    for (int i = 0; COMANDI[i]; i++) noto |= !strcmp(c, COMANDI[i]);
    if (!noto) { fprintf(stderr, "ds4-media: comando sconosciuto '%s' (ds4-media help)\n", c); return 2; }
    if (vuole_aiuto(argc, argv)) return ds4_media_help(stdout, c);
    static cli_args a;
    if (!cli_parse(argc - 1, argv + 1, &a)) { fprintf(stderr, "(ds4-media help %s)\n", c); return 2; }
    bool words_ok = !strcmp(c, "img") || !strcmp(c, "video") || !strcmp(c, "doppia");
    if (!words_ok && a.prompt[0]) { fprintf(stderr, "ds4-media: argomento inatteso: %s\n", a.prompt); return 2; }
    if (!strcmp(c, "serve")) return cmd_serve(&a);
    if (!strcmp(c, "img")) return cmd_img(&a);
    if (!strcmp(c, "video")) return cmd_video(&a);
    if (!strcmp(c, "doppia")) return cmd_doppia(&a);
    if (!strcmp(c, "clean")) return cmd_clean(&a);
    if (!strcmp(c, "health")) return cmd_simple(&a, true);
    return cmd_simple(&a, false);   /* free: l'unico rimasto dei COMANDI */
}
