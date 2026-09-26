/* ds4_media_doppia_testo - le parti pure del doppiaggio, senza processi ne' rete:
 * parole di whisper -> frasi, il file frasi.tsv, il piano dei pezzi H3. Vedi
 * ds4_media_doppia.h. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void doppia_frasi_free(doppia_frase *f, int n) {
    for (int i = 0; i < n; i++) { free(f[i].en); free(f[i].it); }
    free(f);
}

/* Un campo CSV (con "" per le virgolette dentro); avanza *p oltre la virgola. */
static char *csv_campo(const char **p) {
    media_buf b = {0};
    const char *s = *p;
    if (*s == '"') {
        for (s++; *s; s++) {
            if (*s == '"' && s[1] == '"') { media_buf_append(&b, "\"", 1); s++; }
            else if (*s == '"') { s++; break; }
            else media_buf_append(&b, s, 1);
        }
    } else {
        size_t n = strcspn(s, ",\n");
        media_buf_append(&b, s, n);
        s += n;
    }
    if (*s == ',') s++;
    *p = s;
    return media_buf_take(&b);
}

static void frase_chiudi(doppia_frase **out, int *n, int *cap, double da, double a, media_buf *t) {
    char *s = media_buf_take(t);
    char *x = s;
    while (*x == ' ') x++;
    if (!*x) { free(s); return; }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *out = realloc(*out, sizeof(doppia_frase) * (size_t)*cap);
        if (!*out) abort();
    }
    (*out)[*n] = (doppia_frase){.da = da, .a = a, .en = media_xstrdup(x), .it = NULL};
    (*n)++;
    free(s);
}

/* Una frase si chiude alla punteggiatura quando dura almeno min_s, e comunque prima
 * che una parola la porti oltre max_s: frasi abbastanza lunghe da tradurre con senso,
 * abbastanza corte da stare in un pezzo e da riallineare. */
int doppia_frasi_da_csv(const char *csv, double min_s, double max_s, doppia_frase **out) {
    *out = NULL;
    int n = 0, cap = 0;
    const char *p = csv;
    media_buf t = {0};
    double da = -1, a = 0;
    for (int riga = 0; *p; riga++) {
        char *c0 = csv_campo(&p), *c1 = csv_campo(&p), *c2 = csv_campo(&p);
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        char *end0, *end1;
        double s0 = strtod(c0, &end0) / 1000.0, s1 = strtod(c1, &end1) / 1000.0;
        const char *w = c2;
        size_t wl = strlen(w);
        bool parola = end0 != c0 && end1 != c1 && wl && !(w[wl - 1] == ']' && strchr(w, '['));
        if (parola && s1 >= s0) {
            if (da >= 0 && s1 - da > max_s) { frase_chiudi(out, &n, &cap, da, a, &t); da = -1; }
            if (da < 0) da = s0 < 0 ? 0 : s0;   /* un tempo negativo non ha posto nella traccia */
            if (w[0] != ' ' && t.len) media_buf_append(&t, " ", 1);
            media_buf_puts(&t, w);
            a = s1;
            char last = w[wl - 1];
            if (strchr(".,;:?!", last) && a - da >= min_s) { frase_chiudi(out, &n, &cap, da, a, &t); da = -1; }
        }
        free(c0); free(c1); free(c2);
        (void)riga;
    }
    if (da >= 0) frase_chiudi(out, &n, &cap, da, a, &t);
    free(t.ptr);
    return n;
}

/* Tab e a capo dentro il testo diventano spazi: una frase, una riga. */
static void tsv_puts(FILE *fp, const char *s) {
    for (; s && *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, fp);
}

bool doppia_frasi_save(const char *path, const doppia_frase *f, int n) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return false;
    for (int i = 0; i < n; i++) {
        fprintf(fp, "%.3f\t%.3f\t", f[i].da, f[i].a);
        tsv_puts(fp, f[i].en);
        fputc('\t', fp);
        tsv_puts(fp, f[i].it);
        fputc('\n', fp);
    }
    return fclose(fp) == 0 && rename(tmp, path) == 0;   /* mai un file a meta' */
}

int doppia_frasi_load(const char *path, doppia_frase **out) {
    *out = NULL;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = 0, cap = 0;
    media_buf line = {0};
    for (int ch; (ch = fgetc(fp)) != EOF || line.len;) {
        if (ch != '\n' && ch != EOF) { char c = (char)ch; media_buf_append(&line, &c, 1); continue; }
        char *l = media_buf_take(&line);
        char *f1 = strchr(l, '\t'), *f2 = f1 ? strchr(f1 + 1, '\t') : NULL, *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
        if (f3) {
            *f1 = *f2 = *f3 = '\0';
            if (n == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, sizeof(doppia_frase) * (size_t)cap); if (!*out) abort(); }
            (*out)[n++] = (doppia_frase){.da = strtod(l, NULL), .a = strtod(f1 + 1, NULL),
                                         .en = media_xstrdup(f2 + 1), .it = f3[1] ? media_xstrdup(f3 + 1) : NULL};
        }
        free(l);
        if (ch == EOF) break;
    }
    fclose(fp);
    return n;
}

/* Pezzi da al massimo 362 fotogrammi (15,08 s), il limite di H3. Dal secondo pezzo il
 * primo fotogramma si scarta al montaggio (e' la stessa foto dell'ultimo del pezzo
 * prima), quindi un pezzo pieno aggiunge 361 fotogrammi. I pezzi pieni sono ancorati
 * alla foto anche alla fine; l'ultimo, tagliato, no. Un ultimo pezzo corto sale
 * almeno a 124 fotogrammi (5,2 s): sotto, H3 rende male. */
int doppia_piano_pezzi(double durata, doppia_pezzo **out) {
    *out = NULL;
    long totale = (long)ceil(durata * 24.0 - 1e-9);
    if (totale < 1) return 0;
    int n = 0, cap = 0;
    long fatto = 0;
    while (fatto < totale) {
        bool primo = n == 0;
        long serve = totale - fatto + (primo ? 0 : 1);
        int fot;
        bool pieno = serve >= DS4_MEDIA_TALK_MAX_FRAMES;
        if (pieno) {
            fot = DS4_MEDIA_TALK_MAX_FRAMES;
        } else {
            fot = 5 + (int)((serve - 5 + 16) / 17) * 17;
            if (fot < 124) fot = 124;
        }
        if (n == cap) { cap = cap ? cap * 2 : 16; *out = realloc(*out, sizeof(doppia_pezzo) * (size_t)cap); if (!*out) abort(); }
        (*out)[n++] = (doppia_pezzo){.inizio = primo ? 0.0 : (fatto - 1) / 24.0, .fotogrammi = fot, .ancora_fine = pieno};
        fatto += primo ? fot : fot - 1;
    }
    return n;
}
