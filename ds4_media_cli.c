/* ds4-media - eseguibile autonomo del modulo immagini/video, senza LLM ne' GPU.
 *   ds4-media serve [--port 8010] [--host H] [--comfy-port 8188] [--dir D] [--bf16] [--idle-free S]
 *   ds4-media img [--size WxH] [--seed S] [--passi N] [--bf16] [--rif F]... "descrizione"
 *   ds4-media health
 *   ds4-media free
 * Bersaglio fisso per Open WebUI (serve) e prova da riga di comando (img). */
#include "ds4_media.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile int g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void logline(void *pd, const char *msg) { (void)pd; fprintf(stderr, "%s\n", msg); }

static const char *opt(int argc, char **argv, const char *name, const char *def) {
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static bool flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

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

/* Se i GiB liberi sono sotto la soglia, offre di scaricare i modelli da ComfyUI (/free).
 * Con --auto-free libera senza chiedere; con --no-free non chiede e non libera. */
static void offri_free(ds4_media *m, int argc, char **argv, long need_gib, const char *cosa) {
    if (flag(argc, argv, "--no-free")) return;
    long avail = ds4_media_avail_gib();
    if (avail < 0 || avail >= need_gib) return;
    fprintf(stderr, "memoria: %ld GiB liberi, per %s ne servono ~%ld.\n", avail, cosa, need_gib);
    bool go = flag(argc, argv, "--auto-free") ||
              ask("Libero i modelli caricati in ComfyUI (free)?", true);
    if (!go) return;
    char err[256] = {0};
    if (ds4_media_free_models(m, err, sizeof(err)))
        fprintf(stderr, "memoria liberata: ora %ld GiB.\n", ds4_media_avail_gib());
    else
        fprintf(stderr, "free non riuscito: %s\n", err);
}

/* Dopo un lavoro riuscito, chiede se aprire il file col visualizzatore di sistema. */
static void offri_vista(int argc, char **argv, const char *path) {
    if (!path || flag(argc, argv, "--no-vista")) return;
    if (!ask("Vuoi visualizzare il risultato?", false)) return;
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "xdg-open %s >/dev/null 2>&1 &", path);
    if (system(cmd) != 0) fprintf(stderr, "non riesco ad aprire il visualizzatore (xdg-open).\n");
}

static ds4_media *build(int argc, char **argv) {
    static ds4_media_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = opt(argc, argv, "--host", "127.0.0.1");
    cfg.port = atoi(opt(argc, argv, "--comfy-port", "8188"));
    cfg.media_dir = opt(argc, argv, "--dir", NULL);
    /* Su host locale togli la copia doppia di ComfyUI: un solo file, niente sparpagliamento.
     * Con --tieni-comfy la si conserva; con host remoto non si tocca (path non locale). */
    static char comfy_out[1024];
    const char *home = getenv("HOME");
    bool local = !strcmp(cfg.host, "127.0.0.1") || !strcmp(cfg.host, "localhost");
    if (local && home && !flag(argc, argv, "--tieni-comfy")) {
        snprintf(comfy_out, sizeof(comfy_out), "%s/comfy/ComfyUI/output", home);
        cfg.comfy_output_dir = comfy_out;
    }
    cfg.weights = flag(argc, argv, "--bf16") ? DS4_MEDIA_BF16 : DS4_MEDIA_INT8;
    cfg.idle_free_sec = atoi(opt(argc, argv, "--idle-free", "0"));
    cfg.no_gate = flag(argc, argv, "--no-gate");
    cfg.log = logline;
    return ds4_media_create(&cfg);
}

static int cmd_serve(int argc, char **argv) {
    ds4_media *m = build(argc, argv);
    int port = atoi(opt(argc, argv, "--port", "8010"));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    char err[256] = {0};
    bool ok = ds4_media_serve(m, port, &g_stop, err, sizeof(err));
    if (!ok) fprintf(stderr, "ds4-media: %s\n", err);
    ds4_media_free(m);
    return ok ? 0 : 1;
}

static int cmd_img(int argc, char **argv) {
    ds4_media *m = build(argc, argv);
    ds4_media_image_req req = {0};
    req.weights = ds4_media_default_weights(m);   /* --bf16 e' gia' nella config */
    req.seed = atol(opt(argc, argv, "--seed", "-1"));
    req.steps = atoi(opt(argc, argv, "--passi", "0"));
    req.transparent = flag(argc, argv, "--trasparente");
    const char *size = opt(argc, argv, "--size", NULL);
    if (size && !ds4_media_parse_size(size, &req.width, &req.height)) {
        fprintf(stderr, "ds4-media: size non valida: %s\n", size);
        ds4_media_free(m);
        return 2;
    }
    const char *refs[10];
    int nref = 0;
    for (int i = 0; i + 1 < argc && nref < 10; i++)
        if (!strcmp(argv[i], "--rif")) refs[nref++] = argv[i + 1];
    req.refs = refs;
    req.n_refs = nref;
    /* il prompt = ultimo argomento che non e' un'opzione ne' un suo valore */
    const char *prompt = NULL;
    for (int i = argc - 1; i >= 1; i--) {
        if (argv[i][0] == '-') break;
        if (i > 1 && argv[i - 1][0] == '-') { prompt = argv[i]; break; }
        prompt = argv[i];
        break;
    }
    if (!prompt) { fprintf(stderr, "uso: ds4-media img [opzioni] \"descrizione\"\n"); ds4_media_free(m); return 2; }
    req.prompt = prompt;

    offri_free(m, argc, argv, 24, "un'immagine");   /* int8 ~18 + margine */
    fprintf(stderr, "genero l'immagine con Qwen-Image (ComfyUI): \"%.60s\"%s\n",
            prompt, strlen(prompt) > 60 ? "..." : "");
    ds4_media_result res = {0};
    char err[256] = {0};
    bool ok = ds4_media_image(m, &req, &res, err, sizeof(err));
    if (ok) {
        printf("%dx%d seed=%ld in %ld ms\n", res.width, res.height, res.seed, res.ms);
        for (int i = 0; i < res.n_files; i++) printf("  %s\n", res.files[i]);
        if (res.n_files) offri_vista(argc, argv, res.files[0]);
    } else {
        fprintf(stderr, "ds4-media: %s\n", err);
    }
    ds4_media_result_free(&res);
    ds4_media_free(m);
    return ok ? 0 : 1;
}

static int cmd_video(int argc, char **argv) {
    ds4_media *m = build(argc, argv);
    ds4_media_video_req req = {0};
    req.seed = atol(opt(argc, argv, "--seed", "-1"));
    req.steps = atoi(opt(argc, argv, "--passi", "0"));
    req.seconds = atof(opt(argc, argv, "--sec", "0"));
    req.ref = opt(argc, argv, "--rif", NULL);
    const char *size = opt(argc, argv, "--size", NULL);
    if (size && !ds4_media_parse_size(size, &req.width, &req.height)) {
        fprintf(stderr, "ds4-media: size non valida: %s\n", size);
        ds4_media_free(m);
        return 2;
    }
    const char *prompt = NULL;
    for (int i = argc - 1; i >= 1; i--) {
        if (argv[i][0] == '-') break;
        if (i > 1 && argv[i - 1][0] == '-') { prompt = argv[i]; break; }
        prompt = argv[i];
        break;
    }
    if (!prompt || !req.ref) {
        fprintf(stderr, "uso: ds4-media video --rif primo_fotogramma.png [--sec S] [--size WxH] \"descrizione\"\n");
        ds4_media_free(m);
        return 2;
    }
    req.prompt = prompt;
    offri_free(m, argc, argv, 100, "un video H3");   /* H3 occupa la GPU per intero */
    fprintf(stderr, "genero il video con MiniMax H3 (occupa la GPU, dura minuti): \"%.50s\"%s\n",
            prompt, strlen(prompt) > 50 ? "..." : "");
    ds4_media_result res = {0};
    char err[256] = {0};
    bool ok = ds4_media_video(m, &req, &res, err, sizeof(err));
    if (ok) {
        printf("%dx%d seed=%ld in %ld ms\n", res.width, res.height, res.seed, res.ms);
        for (int i = 0; i < res.n_files; i++) printf("  %s\n", res.files[i]);
        if (res.n_files) offri_vista(argc, argv, res.files[0]);
    } else {
        fprintf(stderr, "ds4-media: %s\n", err);
    }
    ds4_media_result_free(&res);
    ds4_media_free(m);
    return ok ? 0 : 1;
}

/* Un file da cancellare e' generato da ds4-media se: immagine (img-*.png),
 * video (vid-*.mp4) o, con with_log, un log di generazione (*.log). Mai altro. */
static bool clean_match(const char *name, bool with_log) {
    size_t n = strlen(name);
    if (!strncmp(name, "img-", 4) && n > 4 && !strcmp(name + n - 4, ".png")) return true;
    if (!strncmp(name, "vid-", 4) && n > 4 && !strcmp(name + n - 4, ".mp4")) return true;
    if (with_log && n > 4 && !strcmp(name + n - 4, ".log")) return true;
    return false;
}

/* Elenca i file corrispondenti in `dir`; con do_delete li cancella. Ritorna il numero
 * di file, accumula i byte in *bytes. Non ricorsivo, mai su cartelle. */
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
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        n++;
        if (bytes) *bytes += st.st_size;
        if (do_delete && unlink(path) != 0)
            fprintf(stderr, "  non cancello %s\n", path);
    }
    closedir(d);
    return n;
}

static int cmd_clean(int argc, char **argv) {
    ds4_media *m = build(argc, argv);
    const char *dir = ds4_media_dir(m);
    bool with_log = !flag(argc, argv, "--no-log");     /* i log si cancellano salvo --no-log */
    bool do_comfy = flag(argc, argv, "--comfy") || flag(argc, argv, "--tutto");
    bool forza = flag(argc, argv, "--forza") || flag(argc, argv, "-y");
    const char *home = getenv("HOME");
    char comfy[1024] = {0};
    if (do_comfy && home) snprintf(comfy, sizeof(comfy), "%s/comfy/ComfyUI/output/ds4", home);

    /* conteggio (dry-run) */
    long long bytes = 0;
    int n = clean_dir(dir, with_log, false, &bytes);
    int nc = comfy[0] ? clean_dir(comfy, false, false, &bytes) : 0;
    if (n + nc == 0) { fprintf(stderr, "niente da cancellare in %s%s.\n", dir,
                               comfy[0] ? " (e nelle copie di ComfyUI)" : ""); ds4_media_free(m); return 0; }
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

static int cmd_simple(int argc, char **argv, bool health) {
    ds4_media *m = build(argc, argv);
    char err[256] = {0};
    bool ok = health ? ds4_media_health(m, err, sizeof(err))
                     : ds4_media_free_models(m, err, sizeof(err));
    printf("%s\n", ok ? "ok" : err);
    ds4_media_free(m);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: ds4-media {serve|img|video|clean|health|free} [opzioni]\n"
                        "  clean [--dir D] [--comfy] [--no-log] [--forza]  cancella img-*.png, vid-*.mp4, *.log\n");
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "serve")) return cmd_serve(argc - 1, argv + 1);
    if (!strcmp(c, "img")) return cmd_img(argc - 1, argv + 1);
    if (!strcmp(c, "video")) return cmd_video(argc - 1, argv + 1);
    if (!strcmp(c, "clean")) return cmd_clean(argc - 1, argv + 1);
    if (!strcmp(c, "health")) return cmd_simple(argc - 1, argv + 1, true);
    if (!strcmp(c, "free")) return cmd_simple(argc - 1, argv + 1, false);
    fprintf(stderr, "ds4-media: comando sconosciuto '%s'\n", c);
    return 2;
}
