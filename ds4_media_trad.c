/* ds4_media_trad - traduzione delle frasi con ds4-server (API OpenAI). Vedi
 * ds4_media_doppia.h.
 *
 * Le frasi vanno a blocchi di 20, con le ultime 5 gia' tradotte come contesto. Ogni
 * frase porta la sua durata e una lunghezza obiettivo in caratteri: l'italiano deve
 * occupare lo stesso tempo dell'originale, ne' troppo lungo (andrebbe accelerato) ne'
 * troppo corto (lascerebbe buchi). La risposta vale solo se e' un array JSON con
 * esattamente tante stringhe quante frasi: se un blocco fallisce tre volte lo si
 * spezza in blocchi da 5 e poi in frasi singole, cosi' un intoppo alla frase 600 non
 * butta via ore di lavoro. Si salva dopo ogni blocco. Alla fine, le frasi in cui chi
 * parla dice di se' ("sono", "mi chiamo", "insegno") passano in terza persona: la
 * faccia e la voce del doppiaggio sono di un'altra persona. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TRAD_BLOCCO 20
#define TRAD_CONTESTO 5
#define TRAD_TENTATIVI 3
#define TRAD_TIMEOUT_MS 900000

int doppia_json_array(const char *testo, char ***out) {
    *out = NULL;
    const char *a = testo ? strchr(testo, '[') : NULL, *z = testo ? strrchr(testo, ']') : NULL;
    if (!a || !z || z < a) return -1;
    char **v = NULL;
    int n = 0;
    const char *p = a + 1;
    for (;;) {
        while (p < z && isspace((unsigned char)*p)) p++;
        if (p == z) break;
        const char *end = NULL;
        char *s = *p == '"' ? media_json_parse_string(p, &end) : NULL;
        if (!s || end > z || end[-1] != '"') { free(s); goto fail; }
        v = realloc(v, sizeof(char *) * (size_t)(n + 1));
        if (!v) abort();
        v[n++] = s;
        p = end;
        while (p < z && isspace((unsigned char)*p)) p++;
        if (p < z && *p == ',') { p++; continue; }
        if (p != z) goto fail;
    }
    *out = v;
    return n;
fail:
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
    return -1;
}

static void trad_free_arr(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* "http://host:porta" -> host, porta (80 senza porta). */
static bool trad_url(const char *url, char *host, size_t hn, int *port) {
    const char *p = strncmp(url, "http://", 7) ? url : url + 7;
    size_t n = strcspn(p, ":/");
    if (!n || n >= hn) return false;
    memcpy(host, p, n);
    host[n] = '\0';
    *port = p[n] == ':' ? atoi(p + n + 1) : 80;
    return *port > 0 && *port < 65536;
}

/* Una richiesta di chat: il contenuto della risposta (malloc'd) o NULL con err. */
static char *trad_chiedi(doppia_conf *c, const char *sistema, const char *utente, char *err, size_t err_len) {
    char host[256];
    int port;
    if (!trad_url(doppia_get(c, "traduttore_url"), host, sizeof(host), &port)) {
        media_set_err(err, err_len, "URL del traduttore non valido: %s", doppia_get(c, "traduttore_url"));
        return NULL;
    }
    char *qm = media_json_quote(doppia_get(c, "traduttore_modello"));
    char *qs = media_json_quote(sistema), *qu = media_json_quote(utente);
    media_buf b = {0};
    media_buf_puts(&b, "{\"model\":");
    media_buf_puts(&b, qm);
    media_buf_puts(&b, ",\"temperature\":0.2,\"max_tokens\":4000,\"think\":false,\"messages\":["
                       "{\"role\":\"system\",\"content\":");
    media_buf_puts(&b, qs);
    media_buf_puts(&b, "},{\"role\":\"user\",\"content\":");
    media_buf_puts(&b, qu);
    media_buf_puts(&b, "}]}");
    free(qm); free(qs); free(qu);
    media_http_response r = {0};
    bool ok = media_http_post(host, port, "/v1/chat/completions", "application/json", b.ptr, b.len,
                              TRAD_TIMEOUT_MS, &r, err, err_len);
    free(b.ptr);
    if (!ok) return NULL;
    char *content = r.status == 200 ? media_json_str(r.body, "content") : NULL;
    if (!content) media_set_err(err, err_len, "il traduttore ha risposto %d senza testo", r.status);
    media_http_response_free(&r);
    return content;
}

static char *trad_sistema(doppia_conf *c) {
    media_buf b = {0};
    media_buf_puts(&b,
        "Sei un adattatore di doppiaggio. Traduci in italiano parlato, naturale e scorrevole, "
        "frasi trascritte da un video. Non riassumere e non aggiungere nulla. Ogni frase ha "
        "una durata in secondi e una lunghezza obiettivo in caratteri: stai tra min_caratteri "
        "e max_caratteri, perche' l'italiano deve durare quanto l'originale. Se chi parla dice "
        "chi e' o cosa fa (sono, mi chiamo, lavoro, insegno), usa la terza persona: il video "
        "sara' doppiato da un'altra persona. Rispondi SOLO con un array JSON di stringhe, una "
        "per frase, nello stesso ordine e nello stesso numero delle frasi ricevute.");
    const char *g = doppia_get(c, "glossario");
    if (g[0]) {
        media_buf_puts(&b, " Scrivi questi termini esattamente cosi', senza tradurli: ");
        media_buf_puts(&b, g);
        media_buf_puts(&b, ".");
    }
    return media_buf_take(&b);
}

/* Traduce [da, da+n): true se tutte hanno ricevuto la loro traduzione. */
static bool trad_blocco(doppia_conf *c, doppia_frase *f, int da, int n, const char *sistema,
                        char *err, size_t err_len) {
    double cps = doppia_get_double(c, "caratteri_secondo");
    if (cps <= 0) cps = 14;
    media_buf u = {0};
    char line[160];
    int ctx0 = da - TRAD_CONTESTO < 0 ? 0 : da - TRAD_CONTESTO;
    if (ctx0 < da) {
        media_buf_puts(&u, "Contesto gia' tradotto (non ripeterlo):\n");
        for (int i = ctx0; i < da; i++) {
            char *qe = media_json_quote(f[i].en), *qi = media_json_quote(f[i].it ? f[i].it : "");
            media_buf_puts(&u, qe); media_buf_puts(&u, " -> "); media_buf_puts(&u, qi); media_buf_puts(&u, "\n");
            free(qe); free(qi);
        }
    }
    snprintf(line, sizeof(line), "\nTraduci queste %d frasi e rispondi con un array JSON di %d stringhe:\n", n, n);
    media_buf_puts(&u, line);
    for (int i = da; i < da + n; i++) {
        double sec = f[i].a - f[i].da;
        int obi = (int)(sec * cps + 0.5);
        if (obi < 12) obi = 12;
        char *qe = media_json_quote(f[i].en);
        media_buf_puts(&u, "{\"en\":");
        media_buf_puts(&u, qe);
        snprintf(line, sizeof(line), ",\"secondi\":%.1f,\"caratteri_obiettivo\":%d,\"min_caratteri\":%d,\"max_caratteri\":%d}\n",
                 sec, obi, (int)(obi * 0.8), (int)(obi * 1.1 + 0.5));
        media_buf_puts(&u, line);
        free(qe);
    }
    bool ok = false;
    for (int t = 0; t < TRAD_TENTATIVI && !ok; t++) {
        char *risp = trad_chiedi(c, sistema, u.ptr, err, err_len);
        if (!risp) { if (!strcmp(err, "annullato")) break; continue; }
        char **v;
        int k = doppia_json_array(risp, &v);
        free(risp);
        if (k != n) {
            media_set_err(err, err_len, "risposta con %d traduzioni invece di %d", k, n);
            if (k > 0) trad_free_arr(v, k);
            continue;
        }
        for (int i = 0; i < n; i++) { free(f[da + i].it); f[da + i].it = v[i]; }
        free(v);
        ok = true;
    }
    free(u.ptr);
    return ok;
}

/* Un blocco che non passa si spezza: 20 -> 5 -> 1. Una frase che non passa resta in
 * inglese, segnalata, e il lavoro va avanti. */
static void trad_ostinato(doppia_conf *c, doppia_frase *f, int da, int n, const char *sistema,
                          int *rimaste, char *err, size_t err_len) {
    if (trad_blocco(c, f, da, n, sistema, err, err_len) || !strcmp(err, "annullato")) return;
    if (n == 1) {
        fprintf(stderr, "ds4-media: frase %d resta in inglese (%s)\n", da + 1, err);
        free(f[da].it);
        f[da].it = media_xstrdup(f[da].en);
        (*rimaste)++;
        return;
    }
    int passo = n > 5 ? 5 : 1;
    for (int i = da; i < da + n; i += passo)
        trad_ostinato(c, f, i, i + passo > da + n ? da + n - i : passo, sistema, rimaste, err, err_len);
}

/* Cerca `pat` in `s` come parole intere (senza lettere subito prima e subito dopo;
 * un pat che finisce con l'apostrofo o uno spazio si attacca alla parola che segue).
 * Con `non_dopo`, scarta le occorrenze precedute da una di quelle parole. */
static bool trad_trova(const char *s, const char *pat, const char *const *non_dopo) {
    size_t n = strlen(pat);
    bool aperto = n && (pat[n - 1] == '\'' || pat[n - 1] == ' ');
    for (const char *p = s; (p = strcasestr(p, pat)); p++) {
        if (p > s && isalpha((unsigned char)p[-1])) continue;
        if (!aperto && isalpha((unsigned char)p[n])) continue;
        bool escluso = false;
        for (int k = 0; non_dopo && non_dopo[k] && !escluso; k++) {
            size_t m = strlen(non_dopo[k]);
            const char *q = p;
            while (q > s && q[-1] == ' ') q--;   /* la parola prima, senza gli spazi */
            escluso = (size_t)(q - s) >= m && !strncasecmp(q - m, non_dopo[k], m) &&
                      (q - m == s || !isalpha((unsigned char)q[-(long)m - 1]));
        }
        if (!escluso) return true;
    }
    return false;
}

/* Chi parla dice chi e' o cosa fa? L'inglese originale e' il segnale piu' affidabile;
 * l'italiano copre le frasi senza originale. Solo costruzioni d'identita': "sono" da
 * solo compare in "ci sono dubbi", "ne sono sicuro", "sono stati"... e il filtro
 * mandava al modello frasi che non parlavano di chi parla. */
bool doppia_parla_di_se(const char *en, const char *it) {
    static const char *en_pat[] = {"my name is", "my name's", "i am a ", "i am an ", "i am the ",
        "i'm a ", "i'm an ", "i'm the ", "i work at", "i work for", "i work as", "i work on", "i teach",
        "i do research", "i'm a professor", "i founded", "i created", "i am the author", "i'm the author",
        "i am the creator", "i'm the creator", "i'm the founder", "i am the founder", NULL};
    static const char *it_pat[] = {"mi chiamo", "io sono", "sono un ", "sono una ", "sono uno ", "sono il ",
        "sono la ", "sono lo ", "sono l'", "lavoro a ", "lavoro per ", "lavoro presso", "lavoro come",
        "lavoro in ", "insegno", "faccio ricerca", "ho fondato", "ho creato", NULL};
    static const char *non_dopo[] = {"ci", "vi", "ne", "che", "non ci", NULL};
    for (int k = 0; en && en_pat[k]; k++) if (trad_trova(en, en_pat[k], NULL)) return true;
    for (int k = 0; it && it_pat[k]; k++) if (trad_trova(it, it_pat[k], non_dopo)) return true;
    return false;
}

bool doppia_traduci(doppia_conf *c, doppia_frase *f, int n, const char *salva_in,
                    doppia_cancel_fn cancel, void *privdata, char *err, size_t err_len) {
    char *sistema = trad_sistema(c);
    int rimaste = 0;
    if (err_len) err[0] = '\0';   /* un messaggio vecchio non deve sembrare "annullato" */
    for (int i = 0; i < n; i += TRAD_BLOCCO) {
        int k = i + TRAD_BLOCCO > n ? n - i : TRAD_BLOCCO, manca = 0;
        for (int j = i; j < i + k; j++) manca += !f[j].it || !f[j].it[0];
        if (!manca) continue;   /* gia' tradotto in un giro precedente */
        if (cancel && cancel(privdata)) { free(sistema); media_set_err(err, err_len, "annullato"); return false; }
        trad_ostinato(c, f, i, k, sistema, &rimaste, err, err_len);
        if (!strcmp(err, "annullato")) { free(sistema); return false; }
        doppia_frasi_save(salva_in, f, n);
        fprintf(stderr, "  traduzione %d/%d\n", i + k, n);
    }
    free(sistema);

    /* Terza persona: una sola volta per lavoro (segnata da un file accanto). */
    char segno[1100];
    snprintf(segno, sizeof(segno), "%s.terza", salva_in);
    if (access(segno, F_OK) != 0) {
        int idx[64], m = 0;
        for (int i = 0; i < n && m < 64; i++) if (f[i].it && doppia_parla_di_se(f[i].en, f[i].it)) idx[m++] = i;
        if (m) {
            media_buf u = {0};
            char line[96];
            snprintf(line, sizeof(line), "Ecco %d frasi. Rispondi con un array JSON di %d stringhe:\n", m, m);
            media_buf_puts(&u, line);
            for (int j = 0; j < m; j++) {   /* con l'originale: si giudica sul significato vero */
                char *qe = media_json_quote(f[idx[j]].en), *qi = media_json_quote(f[idx[j]].it);
                media_buf_puts(&u, "{\"originale\":"); media_buf_puts(&u, qe);
                media_buf_puts(&u, ",\"italiano\":"); media_buf_puts(&u, qi); media_buf_puts(&u, "}\n");
                free(qe); free(qi);
            }
            char *risp = trad_chiedi(c,
                "Riscrivi ogni frase italiana in modo che, dove chi parla dice chi e' o che lavoro fa "
                "(il suo nome, la professione, dove lavora o insegna, cosa ha fondato o creato), "
                "diventi una terza persona riferita all'autore del video. Usa l'originale per capire il "
                "senso. Lascia identiche le frasi che non parlano dell'identita' di chi parla. Rispondi "
                "SOLO con un array JSON delle frasi italiane, nello stesso ordine.",
                u.ptr, err, err_len);
            free(u.ptr);
            char **v = NULL;
            int k = risp ? doppia_json_array(risp, &v) : -1;
            free(risp);
            if (k == m) {
                for (int j = 0; j < m; j++) { free(f[idx[j]].it); f[idx[j]].it = v[j]; }
                free(v);
                doppia_frasi_save(salva_in, f, n);
            } else if (k > 0) {
                trad_free_arr(v, k);
            }
            fprintf(stderr, "  terza persona: %d frasi %s\n", m, k == m ? "riviste" : "lasciate come erano");
        }
        FILE *s = fopen(segno, "w");
        if (s) fclose(s);
    }
    if (rimaste) fprintf(stderr, "ds4-media: %d frasi rimaste in inglese\n", rimaste);
    return true;
}
