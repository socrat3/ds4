/* ds4_media_doppia - il driver del doppiaggio: cartella del lavoro, motori, fasi.
 * Vedi ds4_media_doppia.h per l'elenco delle fasi e dei loro file.
 *
 * La macchina ha un solo motore grande acceso per volta (spark-switch): il traduttore
 * (ds4-server, ~80 GiB) e ComfyUI con H3 (~110 GiB) non stanno insieme. Il driver li
 * accende quando servono, se la configurazione lo permette; altrimenti dice quale
 * riga accendere. Voce e trascrizione non vogliono ComfyUI carico: prima gli si
 * chiede di liberare la memoria. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define P(buf, c, nome) snprintf(buf, sizeof(buf), "%s/%s", (c)->dir, nome)

static char g_titoli[1100];
const char *doppia_titoli_path(const doppia_conf *c) {
    snprintf(g_titoli, sizeof(g_titoli), "%s/titoli.txt", c->dir);
    return g_titoli;
}

static bool esiste(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 0; }
static bool e_url(const char *s) { return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8); }

static bool mkdir_p(const char *path) {
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

/* Nome del lavoro: id YouTube o nome del file, piu' l'intervallo se c'e'. Solo
 * [A-Za-z0-9_-]: finisce in percorsi e nomi di file. */
static void doppia_nome(const doppia_conf *c, char *out, size_t n) {
    const char *s = doppia_get(c, "sorgente"), *id = NULL;
    size_t len = 0;
    const char *v = strstr(s, "v="), *b = strstr(s, "youtu.be/");
    if (e_url(s) && (v || b)) { id = v ? v + 2 : b + 9; len = strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"); }
    else {
        id = strrchr(s, '/') ? strrchr(s, '/') + 1 : s;
        const char *dot = strrchr(id, '.');
        len = dot && dot > id ? (size_t)(dot - id) : strlen(id);
    }
    size_t k = 0;
    for (size_t i = 0; i < len && k + 1 < n && k < 64; i++) {
        char ch = id[i];
        out[k++] = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ? ch : '_';
    }
    out[k] = '\0';
    if (!k) snprintf(out, n, "video");
    double da = doppia_get_double(c, "da"), a = doppia_get_double(c, "a");
    if (da > 0 || a > 0) snprintf(out + strlen(out), n - strlen(out), "_%g-%g", da, a);
}

static void titoli_crea(const doppia_conf *c, const char *path) {
    const char *src = doppia_get(c, "sorgente");
    char *tit = NULL, *aut = NULL, *lnk = NULL;
    if (e_url(src)) {
        const char *argv[] = {doppia_get(c, "yt_dlp"), "--no-warnings", "--skip-download", "--print", "%(title)s",
                              "--print", "%(uploader)s", "--print", "%(webpage_url)s", src, NULL};
        char *o = doppia_cattura(argv);
        char *save = NULL, *l1 = o ? strtok_r(o, "\n", &save) : NULL, *l2 = l1 ? strtok_r(NULL, "\n", &save) : NULL;
        char *l3 = l2 ? strtok_r(NULL, "\n", &save) : NULL;
        if (l3) {
            char *sub = strstr(l1, " (SUB"); if (sub) *sub = '\0';
            tit = media_xstrdup(l1); aut = media_xstrdup(l2);
            lnk = media_xstrdup(!strncmp(l3, "https://www.", 12) ? l3 + 12 : !strncmp(l3, "https://", 8) ? l3 + 8 : l3);
        }
        free(o);
    }
    if (!tit) { const char *b = strrchr(src, '/'); tit = media_xstrdup(b ? b + 1 : src); }
    FILE *fp = fopen(path, "w");
    if (!fp) { free(tit); free(aut); free(lnk); return; }
    fprintf(fp, "# Titolo di testa del video doppiato. Il file e' tuo: non viene piu' sovrascritto.\n#\n"
                "# mostra: si = c'e' il titolo di testa, no = il video parte subito\n"
                "# durata: secondi del titolo di testa\nmostra: si\ndurata: 4\n#\n"
                "# Le righe qui sotto sono disegnate nell'ordine in cui le scrivi. Cambia, togli o\n"
                "# aggiungi quello che vuoi; la parola prima dei due punti sceglie lo stile:\n"
                "#   grande:  grassetto grande     testo:  normale\n"
                "#   piccolo: piccolo              spazio: una riga vuota\ngrande: %s\n", tit);
    if (aut) fprintf(fp, "testo: Video originale di %s\n", aut);
    if (lnk) fprintf(fp, "piccolo: %s\n", lnk);
    fprintf(fp, "spazio:\npiccolo: Doppiaggio italiano con voce e volto generati dall'IA%s\n",
            aut ? ", con il permesso dell'autore" : "");
    fclose(fp);
    free(tit); free(aut); free(lnk);
}

bool doppia_prepara(doppia_conf *c, const char *const *override, char *err, size_t err_len) {
    char e[512], nome[128], p[1100];
    for (int i = 0; override && override[i]; i += 2)   /* prima: servono a trovare la cartella */
        if (!doppia_set(c, override[i], override[i + 1], err, err_len)) return false;
    if (!c->dir[0]) {
        if (!doppia_get(c, "sorgente")[0]) { media_set_err(err, err_len, "manca il video da doppiare"); return false; }
        doppia_nome(c, nome, sizeof(nome));
        const char *h = getenv("HOME");
        snprintf(c->dir, sizeof(c->dir), "%s/.ds4/doppia/%s", h && h[0] ? h : ".", nome);
    }
    P(p, c, "doppia.conf");
    if (access(p, R_OK) == 0) {
        doppia_conf_load(c, p, e, sizeof(e));
        for (int i = 0; override && override[i]; i += 2) doppia_set(c, override[i], override[i + 1], e, sizeof(e));
    }
    P(p, c, "pezzi");
    if (!mkdir_p(p)) { media_set_err(err, err_len, "non creo %s: %s", p, strerror(errno)); return false; }
    P(p, c, "voce");
    mkdir_p(p);
    if (access(doppia_titoli_path(c), F_OK) != 0) titoli_crea(c, doppia_titoli_path(c));
    return true;
}

/* ── motori ───────────────────────────────────────────────────────────────── */

/* host e porta del traduttore da traduttore_url ("http://host:porta"). */
static int traduttore(const doppia_conf *c, char *host, size_t n) {
    const char *u = doppia_get(c, "traduttore_url");
    const char *hp = strncmp(u, "http://", 7) ? u : u + 7;
    size_t hl = strcspn(hp, ":/");
    snprintf(host, n, "%.*s", (int)hl, hp);
    return hp[hl] == ':' ? atoi(hp + hl + 1) : 80;
}

static bool servizio_su(const char *host, int port, const char *path) {
    media_http_response r = {0};
    char e[128];
    bool ok = media_http_get(host, port, path, 3000, &r, e, sizeof(e)) && r.status == 200;
    media_http_response_free(&r);
    return ok;
}

/* Accende la riga di spark-switch (se permesso) e aspetta che host:port risponda. */
static bool accendi(doppia_conf *c, const char *riga, const char *host, int port, const char *path,
                    const char *cosa, const char *log, doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    if (!doppia_is(c, "cambio_motori", "si")) {
        media_set_err(err, err_len, "%s non risponde su %s:%d: accendilo (spark-switch avvia %s) e rilancia", cosa, host, port, riga);
        return false;
    }
    fprintf(stderr, "  accendo %s: spark-switch avvia %s (ferma il motore acceso)\n", cosa, riga);
    const char *argv[12] = {"spark-switch", "avvia"};
    char r[128];
    snprintf(r, sizeof(r), "%s", riga);
    int k = 2;
    for (char *save = NULL, *t = strtok_r(r, " ", &save); t && k < 10; t = strtok_r(NULL, " ", &save)) argv[k++] = t;
    argv[k] = NULL;
    int rc = doppia_esegui(argv, log, cancel, pd);
    for (int i = 0; i < 120 && rc == 0; i++) {   /* fino a 10 minuti */
        if (servizio_su(host, port, path)) return true;
        if (cancel && cancel(pd)) { media_set_err(err, err_len, "annullato"); return false; }
        struct timespec ts = {5, 0};
        nanosleep(&ts, NULL);
    }
    media_set_err(err, err_len, rc == 130 ? "annullato" : "%s non si e' acceso (spark-switch avvia %s, vedi il log)", cosa, riga);
    return false;
}

/* ── fasi ─────────────────────────────────────────────────────────────────── */

static const char *g_aiuti(const doppia_conf *c) {
    static char buf[1100];
    if (doppia_get(c, "aiuti")[0]) return doppia_get(c, "aiuti");
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';
    char *sl = strrchr(exe, '/');
    if (sl) *sl = '\0';
    snprintf(buf, sizeof(buf), "%s/media/doppia", n > 0 ? exe : ".");
    return buf;
}

static void doppia_logfn(void *pd, const char *msg) { (void)pd; fprintf(stderr, "  %s\n", msg); }

static char *prompt_testa(const doppia_conf *c) {
    media_buf b = {0};
    media_buf_puts(&b, "SUBJECT: ");
    media_buf_puts(&b, doppia_get(c, "descrizione")[0] ? doppia_get(c, "descrizione") : "the man in the photo");
    media_buf_puts(&b, ". ACTION: he looks into the camera and talks calmly in Italian, explaining a topic; "
                       "the lip sync follows the speech exactly, the mouth closes in the pauses, small natural "
                       "head movements and occasional hand gestures. He keeps the same face, glasses and "
                       "clothes for the whole clip. SETTING: plain, softly lit room. CAMERA: static, frontal, "
                       "medium close-up. STYLE: realistic video, natural skin, no deformations.");
    return media_buf_take(&b);
}

static bool fase_pezzi(doppia_conf *c, double durata, const char *log, doppia_cancel_fn cancel, void *pd,
                       char *err, size_t err_len) {
    doppia_pezzo *pz;
    int n = doppia_piano_pezzi(durata, &pz), manca = 0;
    char p[1100], a[1100], tmpd[1100];
    for (int i = 0; i < n; i++) { snprintf(p, sizeof(p), "%s/pezzi/p%03d.mp4", c->dir, i); manca += !esiste(p); }
    if (!manca) { free(pz); return true; }
    int port = (int)doppia_get_long(c, "comfy_porta");
    char th[256];
    int tport = traduttore(c, th, sizeof(th));
    /* anche con ComfyUI acceso, se il traduttore e' ancora carico H3 non ci sta */
    if ((!servizio_su("127.0.0.1", port, "/system_stats") || servizio_su(th, tport, "/v1/models")) &&
        !accendi(c, doppia_get(c, "riga_video"), "127.0.0.1", port, "/system_stats", "ComfyUI (H3)",
                 log, cancel, pd, err, err_len)) { free(pz); return false; }
    snprintf(tmpd, sizeof(tmpd), "%s/pezzi/.h3", c->dir);
    const char *home = getenv("HOME");
    char comfy_out[1100];
    snprintf(comfy_out, sizeof(comfy_out), "%s/comfy/ComfyUI/output", home ? home : "");
    ds4_media_config mc = {.host = "127.0.0.1", .port = port, .media_dir = tmpd,
                           .comfy_output_dir = access(comfy_out, W_OK) == 0 ? comfy_out : NULL,
                           .log = doppia_logfn, .cancel = cancel, .cancel_privdata = pd};
    ds4_media *m = ds4_media_create(&mc);
    char *prompt = prompt_testa(c);
    long somma = 0;
    int fatti = 0;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        snprintf(p, sizeof(p), "%s/pezzi/p%03d.mp4", c->dir, i);
        if (esiste(p)) continue;
        snprintf(a, sizeof(a), "%s/pezzi/p%03d.wav", c->dir, i);
        char ss[32], dd[32], v[1100];
        snprintf(ss, sizeof(ss), "%.3f", pz[i].inizio);
        snprintf(dd, sizeof(dd), "%.3f", pz[i].fotogrammi / 24.0);
        snprintf(v, sizeof(v), "%s/voce_it.wav", c->dir);
        const char *argv[] = {doppia_get(c, "ffmpeg"), "-hide_banner", "-loglevel", "error", "-y", "-ss", ss,
                              "-i", v, "-af", "apad", "-t", dd, "-ac", "1", "-ar", "48000", a, NULL};
        if (doppia_esegui(argv, log, cancel, pd) != 0) { media_set_err(err, err_len, "non taglio l'audio del pezzo %d", i + 1); ok = false; break; }
        fprintf(stderr, "  pezzo %d/%d (%.1f s da %.1f s)...\n", i + 1, n, pz[i].fotogrammi / 24.0, pz[i].inizio);
        long t0 = time(NULL);
        ds4_media_talk_req r = {.prompt = prompt, .face = doppia_get(c, "foto"), .audio = a,
                                .width = (int)doppia_get_long(c, "risoluzione"), .height = (int)doppia_get_long(c, "risoluzione"),
                                .frames = pz[i].fotogrammi, .steps = (int)doppia_get_long(c, "passi"),
                                .seed = doppia_get_long(c, "seme") + i, .anchor_end = pz[i].ancora_fine};
        ds4_media_result res;
        ok = ds4_media_talk(m, &r, &res, err, err_len);
        if (ok && rename(res.files[0], p) != 0) { media_set_err(err, err_len, "non sposto il pezzo in %s", p); ok = false; }
        ds4_media_result_free(&res);
        if (ok) {
            somma += time(NULL) - t0;
            fatti++;
            int restano = 0;
            for (int k = i + 1; k < n; k++) { snprintf(a, sizeof(a), "%s/pezzi/p%03d.mp4", c->dir, k); restano += !esiste(a); }
            fprintf(stderr, "  pezzo %d/%d fatto in %ld s; ne restano %d, circa %ld min\n",
                    i + 1, n, (long)(time(NULL) - t0), restano, somma / fatti * restano / 60);
        }
    }
    free(prompt);
    ds4_media_free(m);
    free(pz);
    return ok;
}

/* posizione.txt: "cerchio cx cy r" o "intero". */
static bool fase_posizione(doppia_conf *c, const char *log, doppia_cancel_fn cancel, void *pd,
                           int *cx, int *cy, int *r, char *err, size_t err_len) {
    char p[1100], s[1100], script[1100];
    P(p, c, "posizione.txt");
    P(s, c, "sorgente.mp4");
    const char *pos = doppia_get(c, "posizione");
    if (!strcmp(pos, "auto")) {
        if (!esiste(p)) {
            snprintf(script, sizeof(script), "%s/cerchio.py", g_aiuti(c));
            const char *argv[] = {doppia_get(c, "python_cv"), script, s, p, NULL};
            if (doppia_esegui(argv, log, cancel, pd) != 0 || !esiste(p)) {
                media_set_err(err, err_len, "non trovo la webcam nel video: indica la posizione (cx,cy,r o intero)");
                return false;
            }
        }
    } else {
        FILE *fp = fopen(p, "w");
        if (!fp) { media_set_err(err, err_len, "non scrivo %s", p); return false; }
        int a, b, k;
        if (sscanf(pos, "%d,%d,%d", &a, &b, &k) == 3 && k > 0) fprintf(fp, "cerchio %d %d %d\n", a, b, k);
        else fprintf(fp, "intero\n");
        fclose(fp);
    }
    FILE *fp = fopen(p, "r");
    char l[256] = {0};
    if (fp) { if (!fgets(l, sizeof(l), fp)) l[0] = '\0'; fclose(fp); }
    *r = 0;
    if (sscanf(l, "cerchio %d %d %d", cx, cy, r) != 3) *r = 0;
    fprintf(stderr, "  posizione: %s", l[0] ? l : "intero\n");
    return true;
}

bool doppia_run(doppia_conf *c, doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    char log[1100], orig[1100], src[1100], a16[1100], csv[1100], tsv[1100], voce[1100], e[512];
    P(log, c, "doppia.log");
    P(src, c, "sorgente.mp4");
    P(a16, c, "audio16k.wav");
    P(csv, c, "parole.csv");
    P(tsv, c, "frasi.tsv");
    P(voce, c, "voce_it.wav");
    const char *ff = doppia_get(c, "ffmpeg"), *in = doppia_get(c, "sorgente");
    if (!doppia_verifica(c, err, err_len)) return false;
    media_http_set_cancel(cancel, pd);
    bool ok = false;

    /* 1 scarica */
    if (!esiste(src)) {
        fprintf(stderr, "[1/9] scarico il video\n");
        if (e_url(in)) {
            char tmpl[1100];
            P(tmpl, c, "originale.%(ext)s");
            P(orig, c, "originale.mp4");
            const char *argv[] = {doppia_get(c, "yt_dlp"), "-q", "--no-warnings", "-f",
                                  "bv*[height<=1080][ext=mp4]+ba[ext=m4a]/b[height<=1080]", "--merge-output-format", "mp4",
                                  "--ffmpeg-location", ff, "-o", tmpl, in, NULL};
            if (!esiste(orig) && doppia_esegui(argv, log, cancel, pd) != 0) { media_set_err(err, err_len, "download non riuscito (vedi %s)", log); goto fine; }
        } else {
            snprintf(orig, sizeof(orig), "%s", in);
        }
        double da = doppia_get_double(c, "da"), a = doppia_get_double(c, "a");
        char sda[32], sa[32];
        snprintf(sda, sizeof(sda), "%.3f", da);
        snprintf(sa, sizeof(sa), "%.3f", a);
        /* senza fine si ripete -nostdin: un secondo -ss annullerebbe il primo */
        const char *argv[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-ss", sda, a > 0 ? "-to" : "-nostdin",
                              a > 0 ? sa : "-nostdin", "-i", orig, "-c:v", "libx264", "-preset", "medium", "-crf", "16",
                              "-c:a", "aac", "-b:a", "192k", src, NULL};
        if (a > 0 && a <= da) { media_set_err(err, err_len, "intervallo vuoto: a deve superare da"); goto fine; }
        if (doppia_esegui(argv, log, cancel, pd) != 0) { unlink(src); media_set_err(err, err_len, "non preparo il video (vedi %s)", log); goto fine; }
    }
    double durata = doppia_durata(ff, src, NULL, NULL);
    if (durata <= 0) { media_set_err(err, err_len, "non leggo la durata di %s", src); goto fine; }

    /* 2-3 trascrivi, frasi */
    if (!esiste(tsv)) {
        if (!esiste(csv)) {
            fprintf(stderr, "[2/9] trascrivo (%.0f s di video)\n", durata);
            const char *a1[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-vn", "-ac", "1", "-ar", "16000", a16, NULL};
            char of[1100];
            P(of, c, "parole");
            const char *g = doppia_get(c, "glossario");
            const char *a2[] = {doppia_get(c, "whisper_cli"), "-m", doppia_get(c, "whisper_modello"), "-f", a16,
                                "-l", doppia_get(c, "lingua"), "-ml", "1", "-sow", "-ocsv", "-np", "-of", of,
                                g[0] ? "--prompt" : NULL, g, NULL};
            if (doppia_esegui(a1, log, cancel, pd) != 0 || doppia_esegui(a2, log, cancel, pd) != 0 || !esiste(csv)) {
                media_set_err(err, err_len, "trascrizione non riuscita (vedi %s)", log);
                goto fine;
            }
        }
        FILE *fp = fopen(csv, "r");
        media_buf b = {0};
        char tmp[8192];
        size_t k;
        while (fp && (k = fread(tmp, 1, sizeof(tmp), fp)) > 0) media_buf_append(&b, tmp, k);
        if (fp) fclose(fp);
        doppia_frase *f;
        char *testo = media_buf_take(&b);
        int n = doppia_frasi_da_csv(testo, 3.0, 9.0, &f);
        free(testo);
        if (n <= 0 || !doppia_frasi_save(tsv, f, n)) { doppia_frasi_free(f, n > 0 ? n : 0); media_set_err(err, err_len, "nessuna frase dalla trascrizione"); goto fine; }
        fprintf(stderr, "[3/9] %d frasi\n", n);
        doppia_frasi_free(f, n);
    }

    /* 4 traduci */
    doppia_frase *f;
    int n = doppia_frasi_load(tsv, &f), manca = 0;
    for (int i = 0; i < n; i++) manca += !f[i].it;
    if (manca) {
        fprintf(stderr, "[4/9] traduco %d frasi\n", manca);
        char host[256];
        int port = traduttore(c, host, sizeof(host));
        if (!servizio_su(host, port, "/v1/models") &&
            !accendi(c, doppia_get(c, "riga_traduzione"), host, port, "/v1/models", "il traduttore", log, cancel, pd, err, err_len)) {
            doppia_frasi_free(f, n);
            goto fine;
        }
        if (!doppia_traduci(c, f, n, tsv, cancel, pd, err, err_len)) { doppia_frasi_free(f, n); goto fine; }
    }
    doppia_frasi_free(f, n);

    /* 5 voce: ComfyUI a riposo terrebbe la memoria che serve alla sintesi */
    if (!esiste(voce)) {
        fprintf(stderr, "[5/9] voce italiana (%s)\n", doppia_get(c, "voce_motore"));
        int port = (int)doppia_get_long(c, "comfy_porta");
        if (servizio_su("127.0.0.1", port, "/system_stats")) {
            ds4_media_config mc = {.host = "127.0.0.1", .port = port};
            ds4_media *m = ds4_media_create(&mc);
            ds4_media_free_models(m, e, sizeof(e));
            ds4_media_free(m);
        }
        char script[1100], vdir[1100], dur[32];
        snprintf(script, sizeof(script), "%s/voce.py", g_aiuti(c));
        P(vdir, c, "voce");
        snprintf(dur, sizeof(dur), "%.3f", durata);
        bool qwen = doppia_is(c, "voce_motore", "qwen");
        const char *argv[] = {doppia_get(c, qwen ? "python_qwen" : "python_xtts"), script, doppia_get(c, "voce_motore"),
                              tsv, doppia_get(c, "voce_campione"), doppia_get(c, "voce_testo"), vdir, voce, dur, ff, NULL};
        if (doppia_esegui(argv, log, cancel, pd) != 0 || !esiste(voce)) { media_set_err(err, err_len, "sintesi della voce non riuscita (vedi %s)", log); goto fine; }
    }

    /* 6 posizione, 7 pezzi, 8 monta, 9 titolo */
    bool scena = !doppia_is(c, "resa", "testa"), testa_fin = !doppia_is(c, "resa", "scena");
    int cx = 0, cy = 0, r = 0;
    if (scena) { fprintf(stderr, "[6/9] posizione nella scena\n"); if (!fase_posizione(c, log, cancel, pd, &cx, &cy, &r, err, err_len)) goto fine; }
    fprintf(stderr, "[7/9] teste parlanti H3\n");
    if (!fase_pezzi(c, durata, log, cancel, pd, err, err_len)) goto fine;
    char testa[1100], scena_p[1100], nome[128], out[1200];
    P(testa, c, "testa.mp4");
    P(scena_p, c, "scena.mp4");
    if (!esiste(testa)) {
        fprintf(stderr, "[8/9] monto\n");
        doppia_pezzo *pz;
        int np = doppia_piano_pezzi(durata, &pz);
        char **pezzi = media_xmalloc(sizeof(char *) * (size_t)np);
        for (int i = 0; i < np; i++) { char q[1100]; snprintf(q, sizeof(q), "%s/pezzi/p%03d.mp4", c->dir, i); pezzi[i] = media_xstrdup(q); }
        bool mo = doppia_monta_testa(c, pezzi, np, durata, voce, testa, log, cancel, pd, err, err_len);
        for (int i = 0; i < np; i++) free(pezzi[i]);
        free(pezzi);
        free(pz);
        if (!mo) goto fine;
    }
    if (scena && !esiste(scena_p) && !doppia_monta_scena(c, src, testa, cx, cy, r, voce, scena_p, log, cancel, pd, err, err_len)) goto fine;
    fprintf(stderr, "[9/9] titolo di testa\n");
    snprintf(nome, sizeof(nome), "%.100s", strrchr(c->dir, '/') ? strrchr(c->dir, '/') + 1 : c->dir);
    if (testa_fin) {
        snprintf(out, sizeof(out), "%s/%s_testa.mp4", c->dir, nome);
        if (!doppia_titolo(c, g_aiuti(c), testa, doppia_titoli_path(c), out, log, cancel, pd, err, err_len)) goto fine;
        printf("%s\n", out);
    }
    if (scena) {
        snprintf(out, sizeof(out), "%s/%s_scena.mp4", c->dir, nome);
        if (!doppia_titolo(c, g_aiuti(c), scena_p, doppia_titoli_path(c), out, log, cancel, pd, err, err_len)) goto fine;
        printf("%s\n", out);
    }
    ok = true;
fine:
    media_http_set_cancel(NULL, NULL);
    return ok;
}
