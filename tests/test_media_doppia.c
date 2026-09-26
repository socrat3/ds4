/* Test di ds4-media doppia senza GPU ne' rete: frasi da whisper, frasi.tsv, piano dei
 * pezzi H3, array JSON, voci della configurazione, traduzione contro un finto
 * ds4-server (risposte sbagliate, blocchi che si spezzano, terza persona), grafo della
 * testa parlante contro il finto ComfyUI di media_fake.h.
 *
 *   make test-media-doppia */
#include "../ds4_media_doppia.h"
#include "../ds4_media_http.h"
#include "media_fake.h"

#include <math.h>
#include <signal.h>

static int falliti;
#define VERIFICA(c, ...) do { if (!(c)) { falliti++; printf("FALLITO: " __VA_ARGS__); printf("\n"); } \
                              else printf("ok: %s\n", #c); } while (0)
static char DIR_[256];

static void t_frasi(void) {
    const char *csv =
        "start,end,text\n"
        "0,400,\" Hello\"\n400,900,\" world.\"\n900,1400,\" This\"\n1400,2000,\" is\"\n"
        "2000,2600,\" a\"\n2600,3300,\" \"\"quoted\"\" test,\"\n3300,3600,\" [BLANK_AUDIO]\"\n"
        "3600,4000,\" and\"\n4000,12700,\" long\"\n12700,13000,\" end\"\n";
    doppia_frase *f;
    int n = doppia_frasi_da_csv(csv, 3.0, 9.0, &f);
    /* "Hello world." dura 0,9 s: la virgola/punto non chiude sotto 3 s */
    VERIFICA(n == 3, "tre frasi (%d)", n);
    if (n == 3) {
        VERIFICA(!strcmp(f[0].en, "Hello world. This is a \"quoted\" test,") && f[0].da == 0 && fabs(f[0].a - 3.3) < 1e-9,
                 "prima frase chiusa alla virgola dopo 3 s: [%s] %.1f-%.1f", f[0].en, f[0].da, f[0].a);
        VERIFICA(!strcmp(f[1].en, "and") && fabs(f[1].a - 4.0) < 1e-9, "max 9 s: 'long' non entra con 'and' [%s]", f[1].en);
        VERIFICA(!strcmp(f[2].en, "long end"), "ultima frase senza punteggiatura [%s]", f[2].en);
        VERIFICA(!strstr(f[0].en, "BLANK"), "i marcatori [..] di whisper sono scartati");
    }
    char p[400];
    snprintf(p, sizeof(p), "%s/frasi.tsv", DIR_);
    if (n > 0) {
        free(f[1].it); f[1].it = strdup("con\ttab\ne a capo");
        VERIFICA(doppia_frasi_save(p, f, n), "salva frasi.tsv");
        doppia_frase *g;
        int m = doppia_frasi_load(p, &g);
        VERIFICA(m == n && !strcmp(g[0].en, f[0].en) && !g[0].it && !strcmp(g[1].it, "con tab e a capo"),
                 "tsv: andata e ritorno, tab e a capo diventano spazi, it vuoto resta NULL");
        doppia_frasi_free(g, m);
    }
    doppia_frasi_free(f, n);
    VERIFICA(doppia_frasi_da_csv("", 3, 9, &f) == 0, "csv vuoto: nessuna frase");
    free(f);
}

static void t_piano(void) {
    doppia_pezzo *p;
    int n = doppia_piano_pezzi(15.08, &p);
    VERIFICA(n == 1 && p[0].fotogrammi == 362 && p[0].ancora_fine && p[0].inizio == 0, "15,08 s: un pezzo pieno ancorato");
    free(p);
    n = doppia_piano_pezzi(30.9, &p);
    VERIFICA(n == 3 && p[0].fotogrammi == 362 && p[1].fotogrammi == 362 && p[2].fotogrammi == 124 &&
             p[1].ancora_fine && !p[2].ancora_fine, "30,9 s: due pieni e un ultimo corto non ancorato (%d)", n);
    if (n == 3) VERIFICA(fabs(p[1].inizio - 361 / 24.0) < 1e-9 && fabs(p[2].inizio - 722 / 24.0) < 1e-9,
                         "il pezzo dopo parte dall'ultimo fotogramma del prima (%.4f, %.4f)", p[1].inizio, p[2].inizio);
    free(p);
    for (double d = 0.5; d < 400; d += 7.3) {
        n = doppia_piano_pezzi(d, &p);
        long tot = 0;
        bool griglia = true;
        for (int i = 0; i < n; i++) { tot += i ? p[i].fotogrammi - 1 : p[i].fotogrammi; griglia &= (p[i].fotogrammi - 5) % 17 == 0 && p[i].fotogrammi <= 362; }
        if (tot < (long)ceil(d * 24 - 1e-9) || !griglia) { VERIFICA(0, "piano %.1f s: copre %ld fotogrammi, griglia %d", d, tot, griglia); break; }
        free(p);
    }
    VERIFICA(doppia_piano_pezzi(0, &p) == 0, "durata zero: nessun pezzo");
}

static void t_json(void) {
    char **v;
    int n = doppia_json_array("ecco: [\"uno\", \"d\\\"ue\", \"tr\\u00e8\"] fine", &v);
    VERIFICA(n == 3 && !strcmp(v[1], "d\"ue") && !strcmp(v[2], "tr\xc3\xa8"), "array con testo intorno ed escape");
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
    VERIFICA(doppia_json_array("[]", &v) == 0, "array vuoto");
    free(v);
    VERIFICA(doppia_json_array("[\"a\", 3]", &v) == -1, "un numero nell'array: rifiutato");
    VERIFICA(doppia_json_array("[\"a\" \"b\"]", &v) == -1, "manca la virgola: rifiutato");
    VERIFICA(doppia_json_array("[\"aperta]", &v) == -1, "stringa non chiusa: rifiutata");
    VERIFICA(doppia_json_array("niente", &v) == -1, "nessun array");
}

static void t_conf(void) {
    doppia_conf c;
    setenv("HOME", DIR_, 1);   /* niente ~/.ds4/doppia.conf dell'utente nei test */
    doppia_conf_init(&c);
    char e[256];
    VERIFICA(doppia_is(&c, "voce_motore", "qwen") && doppia_get_long(&c, "risoluzione") == 576, "predefiniti");
    VERIFICA(!doppia_set(&c, "resa", "tutte", e, sizeof(e)) && strstr(e, "entrambe"), "scelta fuori elenco: %s", e);
    VERIFICA(!doppia_set(&c, "passi", "20x", e, sizeof(e)), "intero sporco rifiutato");
    VERIFICA(!doppia_set(&c, "foto", "/nonesiste.png", e, sizeof(e)), "file che non esiste rifiutato");
    VERIFICA(!doppia_set(&c, "glossario", "a\nb", e, sizeof(e)), "a capo nel valore rifiutato");
    VERIFICA(!doppia_set(&c, "boh", "1", e, sizeof(e)), "voce sconosciuta rifiutata");
    VERIFICA(doppia_set(&c, "resa", "scena", e, sizeof(e)) && doppia_set(&c, "glossario", "A, B = C", e, sizeof(e)), "valori validi");
    char p[400];
    snprintf(p, sizeof(p), "%s/c.conf", DIR_);
    VERIFICA(doppia_conf_save(&c, p, e, sizeof(e)), "salva doppia.conf");
    doppia_conf d;
    doppia_conf_init(&d);
    VERIFICA(doppia_conf_load(&d, p, e, sizeof(e)) && doppia_is(&d, "resa", "scena") && doppia_is(&d, "glossario", "A, B = C"),
             "andata e ritorno (anche un '=' nel valore)");
    doppia_conf_free(&d);
    /* prepara: il doppia.conf del lavoro vince sui predefiniti, la riga di comando sul file */
    char video[400];
    snprintf(video, sizeof(video), "%s/lezione.mp4", DIR_);
    FILE *fp = fopen(video, "w"); fputs("x", fp); fclose(fp);
    const char *ov[] = {"sorgente", video, "da", "10", NULL};
    VERIFICA(doppia_prepara(&c, ov, e, sizeof(e)) && strstr(c.dir, "/.ds4/doppia/lezione_10-0"), "cartella del lavoro [%s] %s", c.dir, e);
    char p2[1200];
    snprintf(p2, sizeof(p2), "%s/doppia.conf", c.dir);
    doppia_set(&c, "passi", "7", e, sizeof(e));
    doppia_set(&c, "resa", "testa", e, sizeof(e));
    doppia_conf_save(&c, p2, e, sizeof(e));
    doppia_conf_free(&c);
    doppia_conf_init(&c);
    const char *ov2[] = {"sorgente", video, "da", "10", "resa", "entrambe", NULL};
    VERIFICA(doppia_prepara(&c, ov2, e, sizeof(e)) && doppia_get_long(&c, "passi") == 7 && doppia_is(&c, "resa", "entrambe"),
             "il file del lavoro vince sui predefiniti, la riga di comando sul file");
    VERIFICA(access(doppia_titoli_path(&c), R_OK) == 0, "titoli.txt creato");
    doppia_conf_free(&c);
}

/* ── finto ds4-server ─────────────────────────────────────────────────────── */

static int srv_fd, srv_port, srv_richieste, srv_blocchi20;
static void *finto_llm(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(srv_fd, NULL, NULL);
        if (fd < 0) break;
        size_t n;
        char *req = fk_read(fd, &n);
        srv_richieste++;
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : req;
        int frasi = 0;
        for (char *p = body; (p = strstr(p, "{\\\"en\\\":")); p++) frasi++;
        media_buf out = {0};
        if (strstr(body, "Riscrivi ogni frase")) {
            /* la terza persona: tante righe quante sono le frasi elencate */
            int k = 0;
            for (char *p = strstr(body, "stringhe:\\n"); p && (p = strstr(p + 1, "\\n")); ) k++;
            media_buf_puts(&out, "[");
            for (int i = 0; i + 1 < k; i++) media_buf_puts(&out, i ? ",\\\"lui insegna\\\"" : "\\\"lui insegna\\\"");
            media_buf_puts(&out, "]");
        } else if (frasi == 20) {
            srv_blocchi20++;
            media_buf_puts(&out, "Ecco: [\\\"solo una\\\"]");   /* conteggio sbagliato: il blocco si spezza */
        } else if (strstr(body, "{\\\"en\\\":\\\"IMPOSSIBILE")) {   /* solo se e' da tradurre, non nel contesto */
            media_buf_puts(&out, "non so");
        } else {
            media_buf_puts(&out, "[");
            for (int i = 0; i < frasi; i++) {
                char t[64];
                snprintf(t, sizeof(t), "%s\\\"%s %d\\\"", i ? "," : "", strstr(body, "teach") ? "insegno" : "IT", i);
                media_buf_puts(&out, t);
            }
            media_buf_puts(&out, "]");
        }
        media_buf r = {0};
        media_buf_puts(&r, "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"");
        media_buf_puts(&r, out.ptr);
        media_buf_puts(&r, "\"}}]}");
        fk_send(fd, "200 OK", "application/json", r.ptr, r.len);
        free(out.ptr); free(r.ptr); free(req);
        close(fd);
    }
    return NULL;
}

static void t_traduci(void) {
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(srv_fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(srv_fd, (struct sockaddr *)&a, &al);
    srv_port = ntohs(a.sin_port);
    listen(srv_fd, 16);
    pthread_t th;
    pthread_create(&th, NULL, finto_llm, NULL);
    pthread_detach(th);

    doppia_conf c;
    doppia_conf_init(&c);
    char url[64], e[256], p[400];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", srv_port);
    doppia_set(&c, "traduttore_url", url, e, sizeof(e));
    int n = 23;
    doppia_frase *f = calloc((size_t)n, sizeof(*f));
    for (int i = 0; i < n; i++) {
        char t[64];
        snprintf(t, sizeof(t), i == 7 ? "IMPOSSIBILE %d" : i == 21 ? "I teach %d" : "sentence %d", i);
        f[i] = (doppia_frase){.da = i * 4.0, .a = i * 4.0 + 3.5, .en = strdup(t)};
    }
    snprintf(p, sizeof(p), "%s/trad.tsv", DIR_);
    bool ok = doppia_traduci(&c, f, n, p, NULL, NULL, e, sizeof(e));
    VERIFICA(ok, "traduzione completata: %s", e);
    VERIFICA(srv_blocchi20 == 3, "blocco da 20 provato 3 volte prima di spezzarlo (%d)", srv_blocchi20);
    int it = 0;
    for (int i = 0; i < n; i++) it += f[i].it && strcmp(f[i].it, f[i].en);
    VERIFICA(it == 22, "22 frasi tradotte, spezzando i blocchi (%d)", it);
    VERIFICA(f[7].it && !strcmp(f[7].it, f[7].en), "la frase impossibile resta in inglese [%s]", f[7].it ? f[7].it : "-");
    VERIFICA(f[21].it && !strcmp(f[21].it, "lui insegna"), "terza persona sulla frase che parla di se' [%s]", f[21].it ? f[21].it : "-");
    doppia_frase *g;
    int m = doppia_frasi_load(p, &g);
    VERIFICA(m == n && g[22].it, "salvato dopo ogni blocco");
    doppia_frasi_free(g, m);
    int prima = srv_richieste;
    VERIFICA(doppia_traduci(&c, f, n, p, NULL, NULL, e, sizeof(e)) && srv_richieste == prima, "ripresa: niente da rifare, nessuna richiesta");
    doppia_frasi_free(f, n);
    doppia_conf_free(&c);
}

static void t_terza(void) {
    struct { const char *en, *it; bool si; } casi[] = {
        {"My name is John", "Mi chiamo John", true},
        {"I teach at Stanford", "Insegno a Stanford", true},
        {"I'm the author of Redis", "Sono l'autore di Redis", true},
        {"", "Io sono un ingegnere", true},
        {NULL, "Lavoro presso una grande azienda", true},
        {"there is no doubt about this", "non ci sono dubbi", false},
        {"there are a lot of options", "ci sono una serie di opzioni", false},
        {"I'm sure of it", "ne sono sicuro", false},
        {"I am going to show you", "vi mostrero'", false},
        {"these are the results", "questi sono i risultati", false},
        {"the models that are loaded", "i modelli che sono un problema", false},
        {"we work on it", "lavoriamo su questo", false},
        {"a parsonage", "personalmente insegnoXYZ", false},
    };
    for (size_t i = 0; i < sizeof(casi) / sizeof(casi[0]); i++)
        VERIFICA(doppia_parla_di_se(casi[i].en, casi[i].it) == casi[i].si, "terza persona: [%s] / [%s] -> %d",
                 casi[i].en ? casi[i].en : "(null)", casi[i].it, casi[i].si);
}

/* ds4_media_talk: il grafo ha la guida audio e (con anchor_end) l'ultimo fotogramma. */
static void t_talk(void) {
    static fake_comfy F;
    fk_start(&F);
    fk_reset(&F);
    char face[400], audio[400];
    snprintf(face, sizeof(face), "%s/f.png", DIR_);
    snprintf(audio, sizeof(audio), "%s/a.wav", DIR_);
    unsigned char png[64];
    fk_png(png, 64, 64);
    FILE *fp = fopen(face, "wb"); fwrite(png, 1, 64, fp); fclose(fp);
    fp = fopen(audio, "wb"); fputs("RIFFxxxxWAVE", fp); fclose(fp);
    ds4_media_config mc = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_, .no_gate = true};
    ds4_media *m = ds4_media_create(&mc);
    ds4_media_talk_req r = {.prompt = "parla", .face = face, .audio = audio, .frames = 362, .seed = 1, .anchor_end = true};
    ds4_media_result res;
    char e[256];
    bool ok = ds4_media_talk(m, &r, &res, e, sizeof(e));
    VERIFICA(ok, "testa parlante: %s", e);
    VERIFICA(strstr(F.graph, "MiniMaxH3AddGuide") && strstr(F.graph, "LoadAudio") && strstr(F.graph, "\"conditioning\": [\"201\", 0]"),
             "guida audio tra condizionamento e guider");
    VERIFICA(strstr(F.graph, "\"last_frame\": [\"105\", 0]") && strstr(F.graph, "\"length\": 362"), "ultimo fotogramma = foto");
    VERIFICA(F.uploads == 2, "caricate foto e audio (%d)", F.uploads);
    ds4_media_result_free(&res);
    r.anchor_end = false;
    r.frames = 124;
    ok = ds4_media_talk(m, &r, &res, e, sizeof(e));
    VERIFICA(ok && !strstr(F.graph, "last_frame"), "senza ancora finale: nessun last_frame");
    ds4_media_result_free(&res);
    r.frames = 363;
    VERIFICA(!ds4_media_talk(m, &r, &res, e, sizeof(e)) && strstr(e, "17k+5"), "fotogrammi fuori griglia rifiutati");
    ds4_media_free(m);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    snprintf(DIR_, sizeof(DIR_), "/tmp/ds4doppiaXXXXXX");
    if (!mkdtemp(DIR_)) return 1;
    t_frasi();
    t_piano();
    t_json();
    t_conf();
    t_terza();
    t_traduci();
    t_talk();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DIR_);
    if (system(cmd) != 0) printf("pulizia non riuscita\n");
    printf(falliti ? "\n%d FALLITI\n" : "\ntutti i test superati\n", falliti);
    return falliti != 0;
}
