/* ds4_media_doppia - il driver del doppiaggio: cartella del lavoro e ordine delle
 * fasi, ognuna saltata se il suo file c'e' gia' (vedi ds4_media_doppia.h). Le fasi che
 * parlano con motori, voce e inquadrature sono in ds4_media_doppia_fasi.c.
 *
 * La sorgente e' un URL (yt-dlp) o un file locale in qualsiasi formato che ffmpeg sa
 * leggere: mp4, mkv, mov, webm, avi, ts... */
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
    /* un file locale si ricorda con il percorso assoluto: la ripresa puo' partire da
     * qualsiasi cartella */
    const char *src = doppia_get(c, "sorgente");
    char assoluto[4096];
    if (src[0] && !e_url(src) && realpath(src, assoluto) && strcmp(assoluto, src))
        doppia_set(c, "sorgente", assoluto, e, sizeof(e));
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

bool doppia_run(doppia_conf *c, doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    char log[1100], orig[1100], src[1100], a16[1100], csv[1100], tsv[1100], voce[1100], lingua[16];
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
    doppia_segmento *seg = NULL;
    int nseg = 0;

    /* 1 scarica, o prende il file locale */
    if (!esiste(src)) {
        fprintf(stderr, "[1/10] %s\n", e_url(in) ? "scarico il video" : "preparo il video");
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
            if (doppia_durata(ff, orig, NULL, NULL) <= 0) { media_set_err(err, err_len, "ffmpeg non legge %s come video", orig); goto fine; }
        }
        double da = doppia_get_double(c, "da"), a = doppia_get_double(c, "a");
        char sda[32], sa[32];
        snprintf(sda, sizeof(sda), "%.3f", da);
        snprintf(sa, sizeof(sa), "%.3f", a);
        if (a > 0 && a <= da) { media_set_err(err, err_len, "intervallo vuoto: a deve superare da"); goto fine; }
        /* Senza taglio si prova a copiare (YouTube e la maggior parte dei file danno
         * h264/aac); se il contenitore mp4 non accetta i flussi (mkv con vorbis, avi con
         * mpeg4...) si ricodifica. Con il taglio si ricodifica sempre, per tagliare
         * esatti e non al keyframe. Sempre su un file temporaneo rinominato alla fine. */
        bool taglio = da > 0 || a > 0;
        const char *copia[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-i", orig, "-map", "0:v:0", "-map", "0:a:0?",
                               "-c", "copy", "-movflags", "+faststart", "-f", "mp4", NULL, NULL};
        const char *ricod[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-ss", sda, a > 0 ? "-to" : "-nostdin",
                               a > 0 ? sa : "-nostdin", "-i", orig, "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "libx264",
                               "-preset", "medium", "-crf", "16", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
                               "-f", "mp4", NULL, NULL};
        bool fatto = !taglio && doppia_ffmpeg(copia, 17, src, log, cancel, pd, err, err_len);
        if (!fatto && !strcmp(err, "annullato")) goto fine;
        if (!fatto) {
            if (!taglio) fprintf(stderr, "  il contenitore mp4 non accetta i flussi di questo file: ricodifico\n");
            if (!doppia_ffmpeg(ricod, 28, src, log, cancel, pd, err, err_len)) goto fine;
        }
    }
    double durata = doppia_durata(ff, src, NULL, NULL);
    if (durata <= 0) { media_set_err(err, err_len, "non leggo la durata di %s", src); goto fine; }
    if (!doppia_ha_audio(ff, src)) { media_set_err(err, err_len, "il video non ha una traccia audio: non c'e' niente da doppiare"); goto fine; }

    /* 2 lingua, 3 trascrivi, 4 frasi */
    if (!esiste(a16)) {
        const char *a1[] = {ff, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-vn", "-ac", "1", "-ar", "16000", a16, NULL};
        if (doppia_esegui(a1, log, cancel, pd) != 0) { media_set_err(err, err_len, "non estraggo l'audio (vedi %s)", log); goto fine; }
    }
    if (!doppia_fase_lingua(c, a16, log, cancel, pd, lingua, sizeof(lingua), err, err_len)) goto fine;
    fprintf(stderr, "[2/10] lingua: %s%s\n", lingua, !strcmp(lingua, "it") ? " (gia' italiano: nessuna traduzione)" : "");
    if (!esiste(tsv)) {
        if (!esiste(csv)) {
            fprintf(stderr, "[3/10] trascrivo (%.0f s di video)\n", durata);
            char of[1100];
            P(of, c, "parole");
            const char *g = doppia_get(c, "glossario");
            const char *a2[] = {doppia_get(c, "whisper_cli"), "-m", doppia_get(c, "whisper_modello"), "-f", a16,
                                "-l", lingua, "-ml", "1", "-sow", "-ocsv", "-np", "-of", of,
                                g[0] ? "--prompt" : NULL, g, NULL};
            if (doppia_esegui(a2, log, cancel, pd) != 0 || !esiste(csv)) {
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
        fprintf(stderr, "[4/10] %d frasi\n", n);
        doppia_frasi_free(f, n);
    }

    /* 5 traduci (se la lingua non e' gia' l'italiano) */
    doppia_frase *f;
    int n = doppia_frasi_load(tsv, &f), manca = 0;
    for (int i = 0; i < n; i++) manca += !f[i].it;
    if (manca && !strcmp(lingua, "it")) {
        for (int i = 0; i < n; i++) if (!f[i].it) f[i].it = media_xstrdup(f[i].en);   /* le parole restano le sue */
        doppia_frasi_save(tsv, f, n);
    } else if (manca) {
        fprintf(stderr, "[5/10] traduco %d frasi\n", manca);
        char host[256];
        int port = doppia_traduttore(c, host, sizeof(host));
        if (!doppia_servizio_su(host, port, "/v1/models") &&
            !doppia_accendi(c, doppia_get(c, "riga_traduzione"), host, port, "/v1/models", "il traduttore", log, cancel, pd, err, err_len)) {
            doppia_frasi_free(f, n);
            goto fine;
        }
        if (!doppia_traduci(c, f, n, tsv, cancel, pd, err, err_len)) { doppia_frasi_free(f, n); goto fine; }
    }
    doppia_frasi_free(f, n);

    /* 6 voce, 7 inquadrature, 8 pezzi, 9 monta, 10 titolo */
    if (!doppia_fase_voce(c, lingua, durata, log, cancel, pd, err, err_len)) goto fine;
    bool scena = !doppia_is(c, "resa", "testa"), testa_fin = !doppia_is(c, "resa", "scena");
    if (scena) {
        fprintf(stderr, "[7/10] inquadrature\n");
        if (!doppia_fase_posizione(c, durata, log, cancel, pd, &seg, &nseg, err, err_len)) goto fine;
    }
    fprintf(stderr, "[8/10] teste parlanti H3\n");
    if (!doppia_fase_pezzi(c, durata, log, cancel, pd, err, err_len)) goto fine;
    char testa[1100], scena_p[1100], nome[128], out[1200];
    P(testa, c, "testa.mp4");
    P(scena_p, c, "scena.mp4");
    if (!esiste(testa)) {
        fprintf(stderr, "[9/10] monto\n");
        doppia_pezzo *pz;
        int np = doppia_piano_pezzi(durata, &pz);
        char **pezzi = media_xmalloc(sizeof(char *) * (size_t)np);
        int *fot = media_xmalloc(sizeof(int) * (size_t)np);
        for (int i = 0; i < np; i++) {
            char q[1100];
            snprintf(q, sizeof(q), "%s/pezzi/p%03d.mp4", c->dir, i);
            pezzi[i] = media_xstrdup(q);
            fot[i] = pz[i].fotogrammi;
        }
        bool mo = doppia_monta_testa(c, pezzi, fot, np, durata, voce, testa, log, cancel, pd, err, err_len);
        for (int i = 0; i < np; i++) free(pezzi[i]);
        free(pezzi);
        free(fot);
        free(pz);
        if (!mo) goto fine;
    }
    if (scena && !esiste(scena_p) &&
        !doppia_monta_scena(c, src, testa, seg, nseg, voce, scena_p, log, cancel, pd, err, err_len)) goto fine;
    fprintf(stderr, "[10/10] titolo di testa\n");
    snprintf(nome, sizeof(nome), "%.100s", strrchr(c->dir, '/') ? strrchr(c->dir, '/') + 1 : c->dir);
    if (testa_fin) {
        snprintf(out, sizeof(out), "%s/%s_testa.mp4", c->dir, nome);
        if (!doppia_titolo_se_serve(c, testa, out, log, cancel, pd, err, err_len)) goto fine;
        printf("%s\n", out);
    }
    if (scena) {
        snprintf(out, sizeof(out), "%s/%s_scena.mp4", c->dir, nome);
        if (!doppia_titolo_se_serve(c, scena_p, out, log, cancel, pd, err, err_len)) goto fine;
        printf("%s\n", out);
    }
    ok = true;
fine:
    free(seg);
    media_http_set_cancel(NULL, NULL);
    return ok;
}
