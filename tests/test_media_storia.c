/* Test di ds4-media storia senza GPU: lettura della sceneggiatura (errori con scena e
 * chiave), scene contro il finto ComfyUI di media_fake.h, ripresa, --da, unione con un
 * finto ffmpeg (senza ricodifica, poi ricodifica se la prima rifiuta), annullamento.
 *
 *   make test-media-storia */
#include "../ds4_media_storia.h"
#include "../ds4_media_http.h"
#include "media_fake.h"

#include <signal.h>
#include <sys/stat.h>

static int falliti;
#define VERIFICA(c, ...) do { if (!(c)) { falliti++; printf("FALLITO: " __VA_ARGS__); printf("\n"); } \
                              else printf("ok: %s\n", #c); } while (0)
static char DIR_[256];

static void scrivi(const char *p, const char *t) {
    FILE *fp = fopen(p, "w");
    fputs(t, fp);
    fclose(fp);
}

static bool esiste(const char *dir, const char *nome) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, nome);
    struct stat st;
    return stat(p, &st) == 0 && st.st_size > 0;
}

static char *leggi(const char *p) {
    FILE *fp = fopen(p, "r");
    if (!fp) return media_xstrdup("");
    static char b[8192];
    size_t n = fread(b, 1, sizeof(b) - 1, fp);
    b[n] = '\0';
    fclose(fp);
    return media_xstrdup(b);
}

/* Un caso di lettura: true se storia_leggi riesce; l'errore resta in e. */
static bool prova(const char *testo, storia *s, char *e, size_t el) {
    char p[400];
    snprintf(p, sizeof(p), "%s/prova.txt", DIR_);
    scrivi(p, testo);
    e[0] = '\0';
    return storia_leggi(p, s, e, el);
}

static void t_leggi(void) {
    storia s;
    char e[512];
    bool ok = prova("# titolo\n\n[scena 1]\nsecondi = 8\nfotogramma = un tempio, al tramonto = oro\n"
                    "movimento = la camera avanza\n[scena 2]\n  secondi=4.5  \nfotogramma=b\nmovimento=c\n", &s, e, sizeof(e));
    VERIFICA(ok && s.n == 2, "due scene lette: %s", e);
    if (ok) {
        VERIFICA(s.v[0].secondi == 8 && !strcmp(s.v[0].fotogramma, "un tempio, al tramonto = oro"),
                 "il valore tiene gli '=' dopo il primo [%s]", s.v[0].fotogramma);
        VERIFICA(s.v[1].secondi == 4.5 && !strcmp(s.v[1].movimento, "c"), "spazi tolti, decimali");
        storia_libera(&s);
    }
    VERIFICA(!prova("[scena 1]\nsecondi = 8\nfotogramma = a\n", &s, e, sizeof(e)) && strstr(e, "scena 1: manca movimento"),
             "manca movimento [%s]", e);
    VERIFICA(!prova("[scena 1]\nsecondi=8\nfotogramma=a\nmovimento=b\n[scena 3]\n", &s, e, sizeof(e)) && strstr(e, "attesa la scena 2"),
             "numerazione con un buco [%s]", e);
    VERIFICA(!prova("[scena 1]\nsecondi=20\nfotogramma=a\nmovimento=b\n", &s, e, sizeof(e)) && strstr(e, "0.1..15"),
             "secondi oltre 15 [%s]", e);
    VERIFICA(!prova("[scena 1]\nsecondi=otto\n", &s, e, sizeof(e)) && strstr(e, "non e' un numero"), "secondi non numerici [%s]", e);
    VERIFICA(!prova("[scena 1]\ncolore=rosso\n", &s, e, sizeof(e)) && strstr(e, "chiave sconosciuta colore"), "chiave ignota [%s]", e);
    VERIFICA(!prova("secondi=8\n", &s, e, sizeof(e)) && strstr(e, "prima di [scena 1]"), "chiave fuori scena [%s]", e);
    VERIFICA(!prova("# solo commenti\n", &s, e, sizeof(e)) && strstr(e, "non contiene scene"), "nessuna scena [%s]", e);
    VERIFICA(!prova("[scena uno]\n", &s, e, sizeof(e)) && strstr(e, "intestazione"), "intestazione storta [%s]", e);
    VERIFICA(!prova("[scena 1]\nsecondi=8\nfotogramma=a\n[scena 2]\n", &s, e, sizeof(e)) && strstr(e, "scena 1: manca movimento"),
             "scena incompleta chiusa dalla successiva [%s]", e);
}

static int g_annulla_dopo = -1, g_chiamate;
static bool annulla(void *pd) { (void)pd; g_chiamate++; return g_annulla_dopo >= 0 && g_chiamate > g_annulla_dopo; }

static void t_esegui(void) {
    static fake_comfy F;
    fk_start(&F);
    fk_reset(&F);
    F.view_w = 864; F.view_h = 480;
    char lav[400], ff[400], argsf[400], sc[400];
    snprintf(lav, sizeof(lav), "%s/lavoro", DIR_);
    mkdir(lav, 0775);
    snprintf(argsf, sizeof(argsf), "%s/ffmpeg.args", DIR_);
    snprintf(ff, sizeof(ff), "%s/ffmpeg", DIR_);
    /* Finto ffmpeg: registra gli argomenti; con STORIA_RIFIUTA_COPIA fallisce su "-c copy". */
    char script[1200];
    snprintf(script, sizeof(script),
             "#!/bin/sh\necho \"$@\" >> '%s'\n"
             "case \"$*\" in *'-c copy'*) [ -n \"$STORIA_RIFIUTA_COPIA\" ] && exit 1;; esac\n"
             "for a; do last=$a; done\necho video > \"$last\"\n", argsf);
    scrivi(ff, script);
    chmod(ff, 0755);
    snprintf(sc, sizeof(sc), "%s/s.txt", DIR_);
    scrivi(sc, "[scena 1]\nsecondi=8\nfotogramma=tempio\nmovimento=avanza\n"
               "[scena 2]\nsecondi=8\nfotogramma=colonne\nmovimento=gira\n"
               "[scena 3]\nsecondi=8\nfotogramma=cielo\nmovimento=sale\n");
    storia s;
    char e[512] = {0}, out[600];
    VERIFICA(storia_leggi(sc, &s, e, sizeof(e)), "sceneggiatura di 3 scene: %s", e);

    ds4_media_config mc = {.host = "127.0.0.1", .port = F.port, .media_dir = lav, .no_gate = true};
    ds4_media *m = ds4_media_create(&mc);
    storia_opzioni o = {.dir = lav, .seed = 10, .ffmpeg = ff};
    int rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    VERIFICA(rc == 0, "tre scene e unione: %d %s", rc, e);
    VERIFICA(esiste(lav, "scena-01.png") && esiste(lav, "scena-03.mp4") && esiste(lav, "storia.mp4"), "file delle scene e finale");
    VERIFICA(!esiste(lav, "storia.mp4.tmp.mp4"), "nessun file temporaneo rimasto");
    VERIFICA(F.prompts == 6, "un fotogramma e una clip per scena (%d lavori)", F.prompts);
    VERIFICA(strstr(F.graph, "\"width\": 864") || strstr(F.graph, "864"), "la clip ha la misura predefinita 864x480");
    char *a = leggi(argsf);
    VERIFICA(strstr(a, "-f concat -safe 0 -i") && strstr(a, "-c copy") && !strstr(a, "libx264"), "unione senza ricodifica [%s]", a);
    free(a);
    char lp[500];
    snprintf(lp, sizeof(lp), "%s/storia.lista", lav);
    a = leggi(lp);
    VERIFICA(!strcmp(a, "file 'scena-01.mp4'\nfile 'scena-02.mp4'\nfile 'scena-03.mp4'\n"), "lista relativa alla cartella [%s]", a);
    free(a);

    /* ripresa: tutto gia' fatto, nessun nuovo lavoro su ComfyUI */
    int prima = F.prompts;
    rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    VERIFICA(rc == 0 && F.prompts == prima, "ripresa: nessuna scena rifatta (%d)", F.prompts - prima);

    /* manca la clip 2 ma c'e' il suo fotogramma: si rifa' solo la clip */
    char p[500];
    snprintf(p, sizeof(p), "%s/scena-02.mp4", lav);
    unlink(p);
    prima = F.prompts;
    rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    VERIFICA(rc == 0 && F.prompts == prima + 1, "solo la clip mancante (%d lavori)", F.prompts - prima);

    /* --da 3 con la clip 2 assente: rifiutato prima di usare la GPU */
    unlink(p);
    o.da = 3;
    prima = F.prompts;
    rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    VERIFICA(rc == 1 && strstr(e, "scena-02.mp4") && F.prompts == prima, "--da senza le scene prima [%s]", e);
    o.da = 9;
    VERIFICA(storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e)) == 1 && strstr(e, "oltre"), "--da oltre le scene [%s]", e);
    o.da = 0;

    /* ffmpeg rifiuta la copia: si ricodifica */
    snprintf(p, sizeof(p), "%s/storia.mp4", lav);
    unlink(p);
    unlink(argsf);
    setenv("STORIA_RIFIUTA_COPIA", "1", 1);
    rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    unsetenv("STORIA_RIFIUTA_COPIA");
    a = leggi(argsf);
    VERIFICA(rc == 0 && strstr(a, "-c copy") && strstr(a, "libx264") && esiste(lav, "storia.mp4"),
             "ricodifica dopo il rifiuto: %d %s", rc, e);
    free(a);

    /* annullamento a meta': 130 e nessun file finale */
    snprintf(p, sizeof(p), "%s/scena-03.mp4", lav);
    unlink(p);
    snprintf(p, sizeof(p), "%s/storia.mp4", lav);
    unlink(p);
    g_chiamate = 0;
    g_annulla_dopo = 0;
    o.cancel = annulla;
    rc = storia_esegui(m, &s, &o, out, sizeof(out), e, sizeof(e));
    VERIFICA(rc == 130 && !esiste(lav, "storia.mp4"), "annullato: 130 e nessun storia.mp4 (%d)", rc);
    o.cancel = NULL;
    g_annulla_dopo = -1;

    ds4_media_free(m);
    storia_libera(&s);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    snprintf(DIR_, sizeof(DIR_), "/tmp/ds4-storia-test-%d", (int)getpid());
    mkdir(DIR_, 0775);
    t_leggi();
    t_esegui();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", DIR_);
    if (system(cmd) != 0) printf("cartella di prova non rimossa: %s\n", DIR_);
    printf(falliti ? "\n%d verifiche FALLITE\n" : "\ntutte le verifiche passate\n", falliti);
    return falliti != 0;
}
