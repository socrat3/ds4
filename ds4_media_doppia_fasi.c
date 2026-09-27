/* ds4_media_doppia_fasi - le fasi del doppiaggio che parlano con il mondo: motori
 * (spark-switch), lingua, voce, inquadrature, teste parlanti H3, titolo. L'ordine e
 * la ripresa sono in ds4_media_doppia.c; vedi ds4_media_doppia.h.
 *
 * La macchina ha un solo motore grande acceso per volta: il traduttore (ds4-server,
 * ~80 GiB) e ComfyUI con H3 (~110 GiB) non stanno insieme. Le fasi li accendono quando
 * servono, se la configurazione lo permette; altrimenti dicono quale riga accendere. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define P(buf, c, nome) snprintf(buf, sizeof(buf), "%s/%s", (c)->dir, nome)

static bool esiste(const char *p) { struct stat st; return stat(p, &st) == 0 && st.st_size > 0; }

/* ── motori ───────────────────────────────────────────────────────────────── */

int doppia_traduttore(const doppia_conf *c, char *host, size_t n) {
    int port = 0;
    if (!doppia_url(doppia_get(c, "traduttore_url"), host, n, &port)) { snprintf(host, n, "127.0.0.1"); return 0; }
    return port;
}

bool doppia_servizio_su(const char *host, int port, const char *path) {
    media_http_response r = {0};
    char e[128];
    bool ok = media_http_get(host, port, path, 3000, &r, e, sizeof(e)) && r.status == 200;
    media_http_response_free(&r);
    return ok;
}

/* Accende la riga di spark-switch (se permesso) e aspetta che host:port risponda. */
bool doppia_accendi(doppia_conf *c, const char *riga, const char *host, int port, const char *path,
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
        if (doppia_servizio_su(host, port, path)) return true;
        if (cancel && cancel(pd)) { media_set_err(err, err_len, "annullato"); return false; }
        struct timespec ts = {5, 0};
        nanosleep(&ts, NULL);
    }
    media_set_err(err, err_len, rc == 130 ? "annullato" : "%s non si e' acceso (spark-switch avvia %s, vedi il log)", cosa, riga);
    return false;
}

const char *doppia_aiuti(const doppia_conf *c) {
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

bool doppia_ha_audio(const char *ffmpeg, const char *file) {
    const char *argv[] = {ffmpeg, "-hide_banner", "-i", file, NULL};
    char *o = doppia_cattura(argv);
    bool si = o && strstr(o, " Audio: ");
    free(o);
    return si;
}

/* ── lingua ───────────────────────────────────────────────────────────────── */

/* La lingua parlata: quella scelta, o riconosciuta da whisper sul primo minuto e
 * mezzo e ricordata in lingua.txt. Decide se tradurre e come fare la voce. */
bool doppia_fase_lingua(doppia_conf *c, const char *a16, const char *log, doppia_cancel_fn cancel, void *pd,
                        char *lingua, size_t n, char *err, size_t err_len) {
    char p[1100];
    P(p, c, "lingua.txt");
    (void)cancel; (void)pd; (void)log;
    if (!doppia_is(c, "lingua", "auto")) { snprintf(lingua, n, "%s", doppia_get(c, "lingua")); return true; }
    FILE *fp = fopen(p, "r");
    if (fp) {
        char l[32] = {0};
        bool ok = fgets(l, sizeof(l), fp) != NULL;
        fclose(fp);
        l[strcspn(l, " \r\n")] = '\0';
        if (ok && l[0]) { snprintf(lingua, n, "%s", l); return true; }
    }
    const char *argv[] = {doppia_get(c, "whisper_cli"), "-m", doppia_get(c, "whisper_modello"), "-f", a16,
                          "-l", "auto", "-dl", "-d", "90000", NULL};
    char *o = doppia_cattura(argv);
    const char *hit = o ? strstr(o, "auto-detected language: ") : NULL;
    char cod[16] = {0};
    if (hit) {
        hit += 24;
        size_t k = 0;
        while (k < sizeof(cod) - 1 && islower((unsigned char)hit[k])) { cod[k] = hit[k]; k++; }
    }
    free(o);
    if (strlen(cod) < 2) { media_set_err(err, err_len, "non riconosco la lingua del video: indica lingua (es. en)"); return false; }
    fp = fopen(p, "w");
    if (fp) { fprintf(fp, "%s\n", cod); fclose(fp); }
    snprintf(lingua, n, "%s", cod);
    return true;
}

/* ── voce ─────────────────────────────────────────────────────────────────── */

/* Stessa lingua: la voce originale diventa la tua, con i suoi tempi (la bocca del video
 * resta sincronizzata da sola). Lingua diversa: la traduzione letta con la tua voce,
 * ogni frase al suo posto. ComfyUI a riposo terrebbe la memoria che serve qui. */
bool doppia_fase_voce(doppia_conf *c, const char *lingua, double durata, const char *log,
                      doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    char voce[1100], tsv[1100], vdir[1100], script[1100], dur[32], orig[1100], e[256];
    P(voce, c, "voce_it.wav");
    if (esiste(voce)) return true;
    P(tsv, c, "frasi.tsv");
    P(vdir, c, "voce");
    snprintf(dur, sizeof(dur), "%.3f", durata);
    bool conversione = doppia_is(c, "voce_modalita", "conversione") ||
                       (doppia_is(c, "voce_modalita", "auto") && !strcmp(lingua, "it"));
    fprintf(stderr, "[6/10] voce italiana: %s\n", conversione ? "conversione della voce originale" : doppia_get(c, "voce_motore"));
    int port = (int)doppia_get_long(c, "comfy_porta");
    if (doppia_servizio_su("127.0.0.1", port, "/system_stats")) {
        ds4_media_config mc = {.host = "127.0.0.1", .port = port};
        ds4_media *m = ds4_media_create(&mc);
        ds4_media_free_models(m, e, sizeof(e));
        ds4_media_free(m);
    }
    const char *ff = doppia_get(c, "ffmpeg");
    int rc;
    if (conversione) {
        P(orig, c, "voce_originale.wav");
        char src[1100];
        P(src, c, "sorgente.mp4");
        const char *a1[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-vn", "-ac", "1", "-ar", "24000", orig, NULL};
        snprintf(script, sizeof(script), "%s/voce_vc.py", doppia_aiuti(c));
        const char *a2[] = {doppia_get(c, "python_vc"), script, tsv, orig, doppia_get(c, "voce_campione"), voce, dur, NULL};
        rc = doppia_esegui(a1, log, cancel, pd);
        if (rc == 0) rc = doppia_esegui(a2, log, cancel, pd);
    } else {
        snprintf(script, sizeof(script), "%s/voce.py", doppia_aiuti(c));
        bool qwen = doppia_is(c, "voce_motore", "qwen");
        const char *argv[] = {doppia_get(c, qwen ? "python_qwen" : "python_xtts"), script, doppia_get(c, "voce_motore"),
                              tsv, doppia_get(c, "voce_campione"), doppia_get(c, "voce_testo"), vdir, voce, dur, ff, NULL};
        rc = doppia_esegui(argv, log, cancel, pd);
    }
    if (rc != 0 || !esiste(voce)) {
        media_set_err(err, err_len, rc == 130 ? "annullato" : "voce non riuscita (vedi %s)", log);
        return false;
    }
    return true;
}

/* ── inquadrature ─────────────────────────────────────────────────────────── */

bool doppia_fase_posizione(doppia_conf *c, double durata, const char *log, doppia_cancel_fn cancel, void *pd,
                           doppia_segmento **seg, int *nseg, char *err, size_t err_len) {
    char p[1100], s[1100], script[1100];
    P(p, c, "posizione.txt");
    P(s, c, "sorgente.mp4");
    const char *pos = doppia_get(c, "posizione");
    if (!strcmp(pos, "auto")) {
        if (!esiste(p)) {
            fprintf(stderr, "  riconosco le inquadrature (una passata sul video, solo CPU)...\n");
            snprintf(script, sizeof(script), "%s/scene.py", doppia_aiuti(c));
            const char *argv[] = {doppia_get(c, "python_cv"), script, s, p, NULL};
            int rc = doppia_esegui(argv, log, cancel, pd);
            if (rc != 0 || !esiste(p)) {
                media_set_err(err, err_len, rc == 130 ? "annullato" : "riconoscimento delle inquadrature non riuscito (vedi %s)", log);
                return false;
            }
        }
    } else {
        FILE *fp = fopen(p, "w");
        if (!fp) { media_set_err(err, err_len, "non scrivo %s", p); return false; }
        int a, b, k;
        if (sscanf(pos, "%d,%d,%d", &a, &b, &k) == 3 && k > 0) fprintf(fp, "0 %.3f cerchio %d %d %d\n", durata, a, b, k);
        else fprintf(fp, "0 %.3f intero\n", durata);
        fclose(fp);
    }
    *nseg = doppia_posizione_load(p, durata, seg);
    if (*nseg <= 0) { media_set_err(err, err_len, "mappa delle inquadrature vuota: %s", p); return false; }
    double t[4] = {0};
    for (int i = 0; i < *nseg; i++) t[(*seg)[i].tipo] += (*seg)[i].a - (*seg)[i].da;
    fprintf(stderr, "  %d tratti: tutto schermo %.0f s, webcam tonda %.0f s, webcam rettangolare %.0f s, senza persona %.0f s\n",
            *nseg, t[SEG_INTERO], t[SEG_CERCHIO], t[SEG_RIQUADRO], t[SEG_VUOTO]);
    return true;
}

/* ── teste parlanti H3 ────────────────────────────────────────────────────── */

static void doppia_logfn(void *pd, const char *msg) { (void)pd; fprintf(stderr, "  %s\n", msg); }

static char *prompt_testa(const doppia_conf *c) {
    media_buf b = {0};
    media_buf_puts(&b, "SUBJECT: ");
    /* senza pronomi: la descrizione puo' essere di chiunque */
    media_buf_puts(&b, doppia_get(c, "descrizione")[0] ? doppia_get(c, "descrizione") : "the person in the photo");
    media_buf_puts(&b, ". ACTION: looks into the camera and talks calmly in Italian, explaining a topic; "
                       "the lip sync follows the speech exactly, the mouth closes in the pauses, small natural "
                       "head movements and occasional hand gestures. The same face, glasses and clothes for "
                       "the whole clip. SETTING: plain, softly lit room. CAMERA: static, frontal, "
                       "medium close-up. STYLE: realistic video, natural skin, no deformations.");
    return media_buf_take(&b);
}

bool doppia_fase_pezzi(doppia_conf *c, double durata, const char *log, doppia_cancel_fn cancel, void *pd,
                       char *err, size_t err_len) {
    doppia_pezzo *pz;
    int n = doppia_piano_pezzi(durata, &pz), manca = 0;
    char p[1100], a[1100], tmpd[1100];
    for (int i = 0; i < n; i++) { snprintf(p, sizeof(p), "%s/pezzi/p%03d.mp4", c->dir, i); manca += !esiste(p); }
    if (!manca) { free(pz); return true; }
    int port = (int)doppia_get_long(c, "comfy_porta");
    snprintf(tmpd, sizeof(tmpd), "%s/pezzi/.h3", c->dir);
    const char *home = getenv("HOME");
    char comfy_out[1100];
    snprintf(comfy_out, sizeof(comfy_out), "%s/comfy/ComfyUI/output", home ? home : "");
    ds4_media_config mc = {.host = "127.0.0.1", .port = port, .media_dir = tmpd,
                           .comfy_output_dir = access(comfy_out, W_OK) == 0 ? comfy_out : NULL,
                           .log = doppia_logfn, .cancel = cancel, .cancel_privdata = pd};
    ds4_media *m = ds4_media_create(&mc);
    /* Si cambia motore se ComfyUI e' spento, o se e' acceso ma la memoria non basta
     * (il traduttore ancora carico): lo dice la misura vera. */
    bool su = doppia_servizio_su("127.0.0.1", port, "/system_stats");
    long libera = su ? ds4_media_comfy_avail_gib(m) : -1;
    if ((!su || (libera >= 0 && libera < DS4_MEDIA_VIDEO_NEED_GIB)) &&
        !doppia_accendi(c, doppia_get(c, "riga_video"), "127.0.0.1", port, "/system_stats", "ComfyUI (H3)",
                        log, cancel, pd, err, err_len)) { ds4_media_free(m); free(pz); return false; }
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

/* ── titolo ───────────────────────────────────────────────────────────────── */

/* Il titolo si rifa' solo se e' cambiato qualcosa: il testo di titoli.txt, la scelta
 * titolo si/no, o il video sotto (dimensione e data). */
static unsigned long long firma_titolo(const doppia_conf *c, const char *in) {
    unsigned long long h = 1469598103934665603ULL;
    FILE *fp = fopen(doppia_titoli_path(c), "r");
    for (int ch; fp && (ch = fgetc(fp)) != EOF;) h = (h ^ (unsigned char)ch) * 1099511628211ULL;
    if (fp) fclose(fp);
    struct stat st;
    char buf[160];
    snprintf(buf, sizeof(buf), "|%s|%lld|%lld", doppia_get(c, "titolo"),
             stat(in, &st) == 0 ? (long long)st.st_size : -1LL, stat(in, &st) == 0 ? (long long)st.st_mtime : -1LL);
    for (const char *q = buf; *q; q++) h = (h ^ (unsigned char)*q) * 1099511628211ULL;
    return h;
}

bool doppia_titolo_se_serve(const doppia_conf *c, const char *in, const char *out, const char *log,
                            doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    char fp_path[1300], vecchia[32] = {0}, nuova[32];
    snprintf(fp_path, sizeof(fp_path), "%s.firma", out);
    snprintf(nuova, sizeof(nuova), "%016llx", firma_titolo(c, in));
    FILE *f = fopen(fp_path, "r");
    if (f) { if (!fgets(vecchia, sizeof(vecchia), f)) vecchia[0] = '\0'; fclose(f); }
    vecchia[strcspn(vecchia, "\n")] = '\0';
    if (esiste(out) && !strcmp(vecchia, nuova)) { fprintf(stderr, "  %s: titolo invariato\n", out); return true; }
    if (!doppia_titolo(c, doppia_aiuti(c), in, doppia_titoli_path(c), out, log, cancel, pd, err, err_len)) return false;
    f = fopen(fp_path, "w");
    if (f) { fprintf(f, "%s\n", nuova); fclose(f); }
    return true;
}
