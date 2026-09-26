/* ds4_media_doppia_proc - programmi esterni e montaggio del doppiaggio. Vedi
 * ds4_media_doppia.h.
 *
 * Ogni programma (ffmpeg, yt-dlp, whisper, gli script Python) gira nel suo gruppo di
 * processi, con l'uscita nel log del lavoro: il terminale resta pulito e il Ctrl+C
 * arriva solo a noi, che fermiamo il gruppo intero (Python con i suoi figli compresi)
 * e aspettiamo che se ne vada, cosi' la GPU e' davvero libera per la fase dopo. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

/* Il gruppo del programma esterno in corso (0 = nessuno): il gestore dei segnali della
 * CLI lo ferma prima di uscire, cosi' un secondo Ctrl+C non lascia orfani che tengono
 * la GPU. */
volatile pid_t doppia_figlio = 0;

static void proc_dormi_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Esegue argv; se out non e' NULL raccoglie stdout+stderr (fino a 1 MiB) invece di
 * scriverli nel log. */
static int proc_run(const char *const *argv, const char *log, media_buf *out,
                    doppia_cancel_fn cancel, void *pd) {
    int lfd = -1, pfd[2] = {-1, -1};
    if (out) { if (pipe(pfd) != 0) return -1; }
    else if (log) {
        lfd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) {
            media_buf l = {0};
            media_buf_puts(&l, "\n$");
            for (int i = 0; argv[i]; i++) { media_buf_puts(&l, " "); media_buf_puts(&l, argv[i]); }
            media_buf_puts(&l, "\n");
            if (write(lfd, l.ptr, l.len) < 0) {}
            free(l.ptr);
        }
    }
    pid_t p = fork();
    if (p == 0) {
        setpgid(0, 0);
#ifdef __linux__
        /* se ds4-media muore (kill -9, terminale chiuso) il figlio riceve SIGTERM */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(1);   /* il padre e' gia' morto prima del prctl */
#endif
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) dup2(dn, 0);
        int o = out ? pfd[1] : lfd >= 0 ? lfd : dn;
        if (o >= 0) { dup2(o, 1); dup2(o, 2); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (p < 0) { if (lfd >= 0) close(lfd); if (out) { close(pfd[0]); close(pfd[1]); } return -1; }
    setpgid(p, p);   /* anche dal padre: nessuna finestra in cui il figlio e' nel nostro gruppo */
    doppia_figlio = p;
    if (out) {
        close(pfd[1]);
        fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    }
    int st = 0;
    for (;;) {
        if (out) {
            char b[4096];
            ssize_t n;
            while ((n = read(pfd[0], b, sizeof(b))) > 0) if (out->len < (1 << 20)) media_buf_append(out, b, (size_t)n);
        }
        if (waitpid(p, &st, WNOHANG) == p) break;
        if (cancel && cancel(pd)) {
            kill(-p, SIGTERM);
            int i;
            for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++) proc_dormi_ms(100);
            if (i == 100) { kill(-p, SIGKILL); waitpid(p, &st, 0); }
            doppia_figlio = 0;
            if (lfd >= 0) close(lfd);
            if (out) close(pfd[0]);
            return 130;
        }
        proc_dormi_ms(100);
    }
    if (out) {
        char b[4096];
        ssize_t n;
        while ((n = read(pfd[0], b, sizeof(b))) > 0) if (out->len < (1 << 20)) media_buf_append(out, b, (size_t)n);
        close(pfd[0]);
    }
    doppia_figlio = 0;
    if (lfd >= 0) close(lfd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

char *doppia_cattura(const char *const *argv) {
    media_buf o = {0};
    int rc = proc_run(argv, NULL, &o, NULL, NULL);
    if (rc < 0 || rc == 127) { free(o.ptr); return NULL; }
    return media_buf_take(&o);
}

int doppia_esegui(const char *const *argv, const char *log, doppia_cancel_fn cancel, void *privdata) {
    return proc_run(argv, log, NULL, cancel, privdata);
}

double doppia_durata(const char *ffmpeg, const char *file, int *w, int *h) {
    const char *argv[] = {ffmpeg, "-hide_banner", "-i", file, NULL};
    media_buf o = {0};
    proc_run(argv, NULL, &o, NULL, NULL);   /* ffmpeg -i esce con 1: conta solo cio' che stampa */
    double d = -1;
    const char *p = o.ptr ? strstr(o.ptr, "Duration: ") : NULL;
    int hh, mm;
    double ss;
    if (p && sscanf(p + 10, "%d:%d:%lf", &hh, &mm, &ss) == 3) d = hh * 3600 + mm * 60 + ss;
    const char *v = o.ptr ? strstr(o.ptr, " Video: ") : NULL;
    if (v && w && h) {
        *w = *h = 0;
        for (const char *q = v; *q && *q != '\n'; q++) {
            int a, b;
            char sep;
            if ((q[-1] == ' ') && sscanf(q, "%d%*[x]%d%c", &a, &b, &sep) == 3 && a > 15 && b > 15 &&
                (sep == ' ' || sep == ',' || sep == '\n')) { *w = a; *h = b; break; }
        }
    }
    free(o.ptr);
    return d;
}

/* Esegue ffmpeg scrivendo su out.tmp e rinomina solo se e' riuscito: un file finale
 * esiste solo se e' completo, e la ripresa del lavoro si fida della sua presenza. */
bool doppia_ffmpeg(const char **argv, int argc, const char *out, const char *log,
                        doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp.mp4", out);
    argv[argc] = tmp;
    argv[argc + 1] = NULL;
    int rc = proc_run(argv, log, NULL, cancel, pd);
    if (rc != 0 || rename(tmp, out) != 0) {
        unlink(tmp);
        media_set_err(err, err_len, rc == 130 ? "annullato" : "ffmpeg non e' riuscito a scrivere %s (vedi il log)", out);
        return false;
    }
    return true;
}

bool doppia_monta_testa(const doppia_conf *c, char **pezzi, const int *fotogrammi, int n, double durata,
                        const char *voce, const char *out, const char *log,
                        doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    const char **argv = media_xmalloc(sizeof(char *) * (size_t)(2 * n + 40));
    int k = 0;
    argv[k++] = doppia_get(c, "ffmpeg");
    argv[k++] = "-hide_banner"; argv[k++] = "-loglevel"; argv[k++] = "error"; argv[k++] = "-y";
    for (int i = 0; i < n; i++) { argv[k++] = "-i"; argv[k++] = pezzi[i]; }
    argv[k++] = "-i"; argv[k++] = voce;
    media_buf f = {0};
    char t[256];
    for (int i = 0; i < n; i++) {
        /* Dal secondo pezzo il primo fotogramma e' la foto, uguale all'ultimo di prima.
         * Ogni pezzo e' portato ai suoi fotogrammi previsti (tpad + trim): se H3 ne desse
         * uno in piu' o in meno, lo scarto non si sommerebbe sui pezzi dopo, e il labiale
         * resta allineato alla voce fino alla fine. */
        snprintf(t, sizeof(t), "[%d:v]fps=24,format=yuv420p,setsar=1,trim=start_frame=%d,setpts=PTS-STARTPTS,"
                 "tpad=stop_mode=clone:stop=48,trim=end_frame=%d,setpts=PTS-STARTPTS[v%d];",
                 i, i ? 1 : 0, fotogrammi[i] - (i ? 1 : 0), i);
        media_buf_puts(&f, t);
    }
    for (int i = 0; i < n; i++) { snprintf(t, sizeof(t), "[v%d]", i); media_buf_puts(&f, t); }
    snprintf(t, sizeof(t), "concat=n=%d:v=1:a=0,tpad=stop_mode=clone:stop_duration=2,trim=duration=%.3f,"
             "setpts=PTS-STARTPTS[v];[%d:a]loudnorm=I=%.1f:TP=-1.5:LRA=11,aresample=48000,apad,atrim=0:%.3f[a]",
             n, durata, n, doppia_get_double(c, "volume"), durata);
    media_buf_puts(&f, t);
    const char *resto[] = {"-filter_complex", f.ptr, "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-preset",
                           "medium", "-crf", "17", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart",
                           "-f", "mp4"};
    for (size_t i = 0; i < sizeof(resto) / sizeof(resto[0]); i++) argv[k++] = resto[i];
    bool ok = doppia_ffmpeg(argv, k, out, log, cancel, pd, err, err_len);
    free(f.ptr);
    free(argv);
    return ok;
}

bool doppia_monta_scena(const doppia_conf *c, const char *sorgente, const char *testa,
                        int cx, int cy, int r, const char *voce, const char *out, const char *log,
                        doppia_cancel_fn cancel, void *pd, char *err, size_t err_len) {
    int W = 0, H = 0;
    double dur = doppia_durata(doppia_get(c, "ffmpeg"), sorgente, &W, &H);
    if (dur <= 0 || W <= 0) { media_set_err(err, err_len, "non leggo durata e dimensioni di %s", sorgente); return false; }
    char f[1024], ds[32], maschera[1300];
    snprintf(maschera, sizeof(maschera), "%s.maschera.png", out);
    if (r > 0) {
        /* La maschera del cerchio (2 px di bordo sfumato: 255 dentro, 0 fuori) si calcola
         * una volta in un PNG: geq valutato su ogni fotogramma di un video lungo era la
         * parte piu' lenta del montaggio. */
        char mf[256], ms[64];
        snprintf(ms, sizeof(ms), "color=c=black:s=%dx%d:d=1", 2 * r, 2 * r);
        snprintf(mf, sizeof(mf), "format=gray,geq=lum='255*clip((%d-hypot(X-%d+0.5,Y-%d+0.5))/2,0,1)'", r, r, r);
        const char *mv[] = {doppia_get(c, "ffmpeg"), "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                            "-i", ms, "-vf", mf, "-frames:v", "1", maschera, NULL};
        if (doppia_esegui(mv, log, cancel, pd) != 0) {
            media_set_err(err, err_len, "non preparo la maschera del cerchio (vedi il log)");
            return false;
        }
        snprintf(f, sizeof(f),
                 "[1:v]scale=%d:%d,format=yuva420p[t0];[3:v]format=gray,scale=%d:%d[m];[t0][m]alphamerge[t];"
                 "[0:v][t]overlay=%d:%d:eof_action=repeat[v];"
                 "[2:a]loudnorm=I=%.1f:TP=-1.5:LRA=11,aresample=48000,apad[a]",
                 2 * r, 2 * r, 2 * r, 2 * r, cx - r, cy - r, doppia_get_double(c, "volume"));
    } else {
        snprintf(f, sizeof(f),
                 "[1:v]scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,"
                 "pad=%d:%d:(ow-iw)/2:(oh-ih)/2[t];[0:v][t]overlay=0:0:eof_action=repeat[v];"
                 "[2:a]loudnorm=I=%.1f:TP=-1.5:LRA=11,aresample=48000,apad[a]",
                 W, H, W, H, doppia_get_double(c, "volume"));
    }
    snprintf(ds, sizeof(ds), "%.3f", dur);
    /* la maschera e' un quarto ingresso solo con il cerchio (-loop vale solo per le immagini) */
    const char *argv[48] = {doppia_get(c, "ffmpeg"), "-hide_banner", "-loglevel", "error", "-y",
                            "-i", sorgente, "-i", testa, "-i", voce};
    int k = 11;
    if (r > 0) {
        const char *m4[] = {"-loop", "1", "-framerate", "24", "-i", maschera};
        for (int i = 0; i < 6; i++) argv[k++] = m4[i];
    }
    const char *resto[] = {"-filter_complex", f, "-map", "[v]", "-map", "[a]", "-t", ds, "-c:v", "libx264",
                           "-preset", "medium", "-crf", "18", "-c:a", "aac", "-b:a", "192k",
                           "-movflags", "+faststart", "-f", "mp4"};
    for (size_t i = 0; i < sizeof(resto) / sizeof(resto[0]); i++) argv[k++] = resto[i];
    argv[k] = NULL;
    bool ok = doppia_ffmpeg(argv, k, out, log, cancel, pd, err, err_len);
    unlink(maschera);
    return ok;
}

/* mostra: e durata: di titoli.txt (le righe da disegnare le legge lo script). */
static void titoli_leggi(const char *path, bool *mostra, double *durata) {
    *mostra = true;
    *durata = 4;
    FILE *fp = fopen(path, "r");
    if (!fp) { *mostra = false; return; }
    char l[1024];
    while (fgets(l, sizeof(l), fp)) {
        if (!strncmp(l, "mostra:", 7)) {
            char *v = l + 7;
            while (*v == ' ') v++;
            *mostra = !(v[0] == 'n' || v[0] == 'N');
        } else if (!strncmp(l, "durata:", 7)) {
            double d = strtod(l + 7, NULL);
            if (d >= 1 && d <= 30) *durata = d;
        }
    }
    fclose(fp);
}

bool doppia_titolo(const doppia_conf *c, const char *aiuti, const char *in, const char *titoli,
                   const char *out, const char *log, doppia_cancel_fn cancel, void *pd,
                   char *err, size_t err_len) {
    bool mostra;
    double dur;
    titoli_leggi(titoli, &mostra, &dur);
    if (!mostra || !doppia_is(c, "titolo", "si")) {
        const char *argv[] = {doppia_get(c, "ffmpeg"), "-hide_banner", "-loglevel", "error", "-y", "-i", in,
                              "-c", "copy", "-movflags", "+faststart", "-f", "mp4", NULL, NULL};
        return doppia_ffmpeg(argv, 13, out, log, cancel, pd, err, err_len);
    }
    int W = 0, H = 0;
    if (doppia_durata(doppia_get(c, "ffmpeg"), in, &W, &H) <= 0 || W <= 0) {
        media_set_err(err, err_len, "non leggo le dimensioni di %s", in);
        return false;
    }
    char png[1200], ws[16], hs[16], script[1200], ds[16], fo[16], f[512];
    snprintf(png, sizeof(png), "%s.titolo.png", out);
    snprintf(ws, sizeof(ws), "%d", W);
    snprintf(hs, sizeof(hs), "%d", H);
    snprintf(script, sizeof(script), "%s/titolo.py", aiuti);
    const char *py[] = {doppia_get(c, "python_cv"), script, ws, hs, png, titoli, NULL};
    if (doppia_esegui(py, log, cancel, pd) != 0) {
        media_set_err(err, err_len, "il titolo non e' stato disegnato (vedi il log)");
        return false;
    }
    snprintf(ds, sizeof(ds), "%.2f", dur);
    snprintf(fo, sizeof(fo), "%.2f", dur - 0.5);
    snprintf(f, sizeof(f), "[1:v]fade=t=in:st=0:d=0.5,fade=t=out:st=%s:d=0.5,format=yuv420p,setsar=1[cv];"
             "[0:v]fps=24,format=yuv420p,setsar=1[v0];[0:a]aresample=48000,aformat=channel_layouts=stereo[a0];"
             "[cv][2:a][v0][a0]concat=n=2:v=1:a=1[v][a]", fo);
    const char *argv[] = {doppia_get(c, "ffmpeg"), "-hide_banner", "-loglevel", "error", "-y", "-i", in,
                          "-loop", "1", "-framerate", "24", "-t", ds, "-i", png,
                          "-f", "lavfi", "-t", ds, "-i", "anullsrc=r=48000:cl=stereo",
                          "-filter_complex", f, "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-preset",
                          "medium", "-crf", "18", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart",
                          "-f", "mp4", NULL, NULL};
    int k = 0;
    while (argv[k]) k++;
    bool ok = doppia_ffmpeg(argv, k, out, log, cancel, pd, err, err_len);
    unlink(png);
    return ok;
}
