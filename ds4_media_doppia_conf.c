/* ds4_media_doppia_conf - le scelte di un doppiaggio: tabella delle voci, doppia.conf,
 * wizard. Vedi ds4_media_doppia.h.
 *
 * I valori si stratificano: predefiniti qui sotto (generici: nessun percorso
 * personale nel codice), poi ~/.ds4/doppia.conf (le scelte stabili dell'utente: foto,
 * voce, strumenti), poi doppia.conf del lavoro, poi le opzioni della riga di comando.
 * Il wizard mostra il risultato e lascia cambiare tutto. */
#include "ds4_media_doppia.h"
#include "ds4_media_http.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const doppia_voce DOPPIA_VOCI[] = {
    {"sorgente", "Sorgente", "Video", "URL YouTube o percorso di un file video", NULL, 'u', ""},
    {"da", "Sorgente", "Da (s)", "secondo di inizio nel video originale", NULL, 'd', "0"},
    {"a", "Sorgente", "A (s)", "secondo di fine; 0 = fino alla fine", NULL, 'd', "0"},
    {"lingua", "Testo", "Lingua originale", "auto = riconosciuta dall'audio; o un codice whisper (en, es, fr, de...). Se e' it non si traduce", NULL, 't', "auto"},
    {"glossario", "Testo", "Glossario", "nomi e termini da scrivere e tenere cosi' come sono, separati da virgole", NULL, 't', ""},
    {"traduttore_url", "Testo", "Traduttore", "ds4-server compatibile OpenAI", NULL, 't', "http://127.0.0.1:8001"},
    {"traduttore_modello", "Testo", "Modello", "nome del modello da chiedere al server", NULL, 't', "qwen3.8-flash-next"},
    {"caratteri_secondo", "Testo", "Caratteri al secondo", "lunghezza obiettivo dell'italiano (tra 0,8x e 1,1x)", NULL, 'd', "14"},
    {"voce_modalita", "Voce", "Modalita'", "auto = conversione se il video e' gia' in italiano, sintesi se tradotto; conversione = la voce originale diventa la tua con gli stessi tempi; sintesi = la traduzione letta con la tua voce", "auto|conversione|sintesi", 't', "auto"},
    {"voce_motore", "Voce", "Motore di sintesi", "qwen = Qwen3-TTS, xtts = XTTS-v2 (solo uso non commerciale)", "qwen|xtts", 't', "qwen"},
    {"voce_campione", "Voce", "Campione", "wav della tua voce, 30-60 s di parlato pulito", NULL, 'f', ""},
    {"voce_testo", "Voce", "Testo del campione", "trascrizione del campione (serve a Qwen3-TTS)", NULL, 'f', ""},
    {"python_qwen", "Voce", "Python Qwen3-TTS", "interprete con il pacchetto qwen-tts", NULL, 'x', "python3"},
    {"python_xtts", "Voce", "Python XTTS", "interprete con il pacchetto coqui-tts", NULL, 'x', "python3"},
    {"python_vc", "Voce", "Python conversione", "interprete con chatterbox-tts (conversione della voce, MIT)", NULL, 'x', "python3"},
    {"foto", "Volto", "Foto", "la tua foto vera, frontale", NULL, 'f', ""},
    {"descrizione", "Volto", "Descrizione", "come appari, in inglese: aiuta H3 a non inventare", NULL, 't', ""},
    {"resa", "Volto", "Resa", "testa = solo tu che parli, scena = tu nel cerchio del video, entrambe", "entrambe|testa|scena", 't', "entrambe"},
    {"posizione", "Volto", "Posizione nella scena", "auto = inquadrature riconosciute tratto per tratto (webcam tonda o rettangolare, persona a tutto schermo, schermo senza persona); intero; o cx,cy,r in pixel", NULL, 't', "auto"},
    {"risoluzione", "Video H3", "Lato (px)", "lato della testa parlante, multiplo di 32", NULL, 'i', "576"},
    {"passi", "Video H3", "Passi", "passi di campionamento di H3", NULL, 'i', "20"},
    {"seme", "Video H3", "Seme", "seme del primo pezzo (i successivi +1)", NULL, 'i', "42"},
    {"titolo", "Montaggio", "Titolo di testa", "si = schermata iniziale da titoli.txt (t per le righe)", "si|no", 't', "si"},
    {"volume", "Montaggio", "Volume (LUFS)", "loudness del parlato finale", NULL, 'd', "-16"},
    {"cambio_motori", "Macchina", "Cambio motori", "si = spark-switch accende traduttore e ComfyUI quando servono", "si|no", 't', "si"},
    {"riga_traduzione", "Macchina", "Riga traduttore", "riga di spark-switch del ds4-server, con le sue opzioni (es. 6 novisione)", NULL, 't', "6"},
    {"riga_video", "Macchina", "Riga ComfyUI", "riga di spark-switch di ComfyUI con H3", NULL, 't', "15"},
    {"comfy_porta", "Macchina", "Porta ComfyUI", "", NULL, 'i', "8188"},
    {"ffmpeg", "Strumenti", "ffmpeg", "", NULL, 'x', "ffmpeg"},
    {"yt_dlp", "Strumenti", "yt-dlp", "", NULL, 'x', "yt-dlp"},
    {"whisper_cli", "Strumenti", "whisper-cli", "whisper.cpp compilato con CUDA", NULL, 'x', "whisper-cli"},
    {"whisper_modello", "Strumenti", "Modello whisper", "es. ggml-large-v3-turbo.bin", NULL, 'f', ""},
    {"python_cv", "Strumenti", "Python OpenCV/PIL", "per il cerchio automatico e il titolo", NULL, 'x', "python3"},
    {"aiuti", "Strumenti", "Script d'aiuto", "cartella media/doppia; vuoto = accanto all'eseguibile", NULL, 't', ""},
};
const int DOPPIA_N_VOCI = (int)(sizeof(DOPPIA_VOCI) / sizeof(DOPPIA_VOCI[0]));

static int doppia_idx(const char *chiave) {
    for (int i = 0; i < DOPPIA_N_VOCI; i++)
        if (!strcmp(DOPPIA_VOCI[i].chiave, chiave)) return i;
    return -1;
}

static char *doppia_home_conf(void) {
    const char *h = getenv("HOME");
    media_buf b = {0};
    media_buf_puts(&b, h && h[0] ? h : ".");
    media_buf_puts(&b, "/.ds4/doppia.conf");
    return media_buf_take(&b);
}

void doppia_conf_init(doppia_conf *c) {
    memset(c, 0, sizeof(*c));
    c->val = media_xmalloc(sizeof(char *) * (size_t)DOPPIA_N_VOCI);
    for (int i = 0; i < DOPPIA_N_VOCI; i++) c->val[i] = media_xstrdup(DOPPIA_VOCI[i].predefinito);
    char *home = doppia_home_conf(), e[256];
    if (access(home, R_OK) == 0 && !doppia_conf_load(c, home, e, sizeof(e)))
        fprintf(stderr, "ds4-media: %s\n", e);
    free(home);
}

void doppia_conf_free(doppia_conf *c) {
    for (int i = 0; c->val && i < DOPPIA_N_VOCI; i++) free(c->val[i]);
    free(c->val);
    c->val = NULL;
}

const char *doppia_get(const doppia_conf *c, const char *chiave) {
    int i = doppia_idx(chiave);
    return i < 0 ? "" : c->val[i];
}
long doppia_get_long(const doppia_conf *c, const char *chiave) { return strtol(doppia_get(c, chiave), NULL, 10); }
double doppia_get_double(const doppia_conf *c, const char *chiave) { return strtod(doppia_get(c, chiave), NULL); }
bool doppia_is(const doppia_conf *c, const char *chiave, const char *v) { return !strcmp(doppia_get(c, chiave), v); }

/* Un eseguibile: con '/' deve esserlo quel file, senza lo si cerca nel PATH. */
static bool doppia_eseguibile(const char *v) {
    if (strchr(v, '/')) return access(v, X_OK) == 0;
    const char *path = getenv("PATH");
    char buf[1024];
    for (const char *p = path ? path : ""; *p;) {
        const char *q = strchr(p, ':');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        snprintf(buf, sizeof(buf), "%.*s/%s", (int)n, p, v);
        if (access(buf, X_OK) == 0) return true;
        p += n + (q ? 1 : 0);
    }
    return false;
}

static bool doppia_set_ex(doppia_conf *c, const char *chiave, const char *valore, bool esistenza,
                          char *err, size_t err_len) {
    int i = doppia_idx(chiave);
    if (i < 0) { media_set_err(err, err_len, "voce sconosciuta: %s", chiave); return false; }
    const doppia_voce *d = &DOPPIA_VOCI[i];
    /* ~/ davanti a un percorso diventa la home: i file di configurazione restano leggibili */
    media_buf b = {0};
    const char *home = getenv("HOME");
    if (valore[0] == '~' && valore[1] == '/' && home) { media_buf_puts(&b, home); media_buf_puts(&b, valore + 1); }
    else media_buf_puts(&b, valore);
    char *v = media_buf_take(&b);
    char *end;
    bool ok = !strchr(v, '\n') && !strchr(v, '\r');
    double num = 0;
    if (ok && d->tipo == 'i' && v[0]) { errno = 0; num = (double)strtol(v, &end, 10); ok = !*end && !errno; }
    if (ok && d->tipo == 'd' && v[0]) { num = strtod(v, &end); ok = !*end && end != v && isfinite(num); }
    /* limiti che il resto del codice da' per scontati: fuori, l'errore arriverebbe ore
     * dopo da ffmpeg o da ComfyUI */
    static const struct { const char *k; double lo, hi; } LIM[] = {
        {"da", 0, 1e6}, {"a", 0, 1e6}, {"caratteri_secondo", 1, 40}, {"risoluzione", 64, 2752},
        {"passi", 1, 60}, {"seme", -1, 2147483647.0}, {"volume", -70, -1}, {"comfy_porta", 1, 65535}};
    for (size_t l = 0; ok && v[0] && (d->tipo == 'i' || d->tipo == 'd') && l < sizeof(LIM) / sizeof(LIM[0]); l++)
        if (!strcmp(LIM[l].k, chiave)) ok = num >= LIM[l].lo && num <= LIM[l].hi;
    if (ok && !strcmp(chiave, "risoluzione") && v[0]) ok = (long)num % 32 == 0;
    if (ok && (d->tipo == 'i' || d->tipo == 'd') && !v[0]) ok = false;   /* un numero vuoto non ha senso */
    if (ok && esistenza && d->tipo == 'f' && v[0]) ok = access(v, R_OK) == 0;
    if (ok && esistenza && d->tipo == 'x' && v[0]) ok = doppia_eseguibile(v);
    if (ok && d->scelte) {
        size_t n = strlen(v);
        const char *s = d->scelte;
        ok = false;
        while (*s && !ok) {
            const char *q = strchr(s, '|');
            size_t k = q ? (size_t)(q - s) : strlen(s);
            ok = k == n && !strncmp(s, v, n);
            s += k + (q ? 1 : 0);
        }
    }
    if (!ok) {
        media_set_err(err, err_len, "%s: valore non valido \"%s\"%s%s%s", d->etichetta, v,
                      d->scelte ? " (scegli fra " : d->tipo == 'f' ? " (file non leggibile" :
                      d->tipo == 'x' ? " (eseguibile non trovato" : "",
                      d->scelte ? d->scelte : "", d->scelte || d->tipo == 'f' || d->tipo == 'x' ? ")" : "");
        free(v);
        return false;
    }
    free(c->val[i]);
    c->val[i] = v;
    return true;
}

bool doppia_set(doppia_conf *c, const char *chiave, const char *valore, char *err, size_t err_len) {
    return doppia_set_ex(c, chiave, valore, true, err, err_len);
}

bool doppia_verifica(const doppia_conf *c, char *err, size_t err_len) {
    bool url = !strncmp(doppia_get(c, "sorgente"), "http", 4), qwen = doppia_is(c, "voce_motore", "qwen");
    const char *serve[] = {"ffmpeg", "whisper_cli", "whisper_modello", "foto", "voce_campione", "python_cv",
                           qwen ? "python_qwen" : "python_xtts", url ? "yt_dlp" : NULL, qwen ? "voce_testo" : NULL};
    for (size_t k = 0; k < sizeof(serve) / sizeof(serve[0]); k++) {
        if (!serve[k]) continue;
        const doppia_voce *d = &DOPPIA_VOCI[doppia_idx(serve[k])];
        const char *v = doppia_get(c, serve[k]);
        bool ok = v[0] && (d->tipo == 'x' ? doppia_eseguibile(v) : access(v, R_OK) == 0);
        if (!ok) {
            media_set_err(err, err_len, "%s: %s \"%s\" (voce %s nel wizard o in doppia.conf)", d->etichetta,
                          !v[0] ? "manca," : d->tipo == 'x' ? "eseguibile non trovato:" : "file non leggibile:", v, d->chiave);
            return false;
        }
    }
    char h[256];
    int port;
    if (!doppia_url(doppia_get(c, "traduttore_url"), h, sizeof(h), &port)) {
        media_set_err(err, err_len, "Traduttore: URL non valido \"%s\" (serve http://host:porta)", doppia_get(c, "traduttore_url"));
        return false;
    }
    if (!url && access(doppia_get(c, "sorgente"), R_OK) != 0) {
        media_set_err(err, err_len, "video non leggibile: %s", doppia_get(c, "sorgente"));
        return false;
    }
    return true;
}

bool doppia_conf_load(doppia_conf *c, const char *path, char *err, size_t err_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) { media_set_err(err, err_len, "non leggo %s: %s", path, strerror(errno)); return false; }
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        n++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "ds4-media: %s:%d senza '=': ignorata\n", path, n); continue; }
        char *k = s, *v = eq + 1;
        *eq = '\0';
        for (char *t = eq - 1; t >= k && (*t == ' ' || *t == '\t'); t--) *t = '\0';
        while (*v == ' ' || *v == '\t') v++;
        v[strcspn(v, "\r\n")] = '\0';
        for (char *t = v + strlen(v) - 1; t >= v && (*t == ' ' || *t == '\t'); t--) *t = '\0';
        char e[256];
        if (doppia_idx(k) < 0) fprintf(stderr, "ds4-media: %s:%d voce sconosciuta \"%s\": ignorata\n", path, n, k);
        else if (!doppia_set_ex(c, k, v, false, e, sizeof(e))) fprintf(stderr, "ds4-media: %s:%d %s\n", path, n, e);
    }
    fclose(fp);
    return true;
}

bool doppia_conf_save(const doppia_conf *c, const char *path, char *err, size_t err_len) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) { media_set_err(err, err_len, "non scrivo %s: %s", tmp, strerror(errno)); return false; }
    fprintf(fp, "# ds4-media doppia: chiave = valore. Modificabile a mano o con il wizard.\n");
    const char *sez = "";
    for (int i = 0; i < DOPPIA_N_VOCI; i++) {
        const doppia_voce *d = &DOPPIA_VOCI[i];
        if (strcmp(sez, d->sezione)) { fprintf(fp, "\n# --- %s ---\n", d->sezione); sez = d->sezione; }
        fprintf(fp, "# %s%s%s", d->etichetta, d->aiuto[0] ? ": " : "", d->aiuto);
        if (d->scelte) fprintf(fp, " [%s]", d->scelte);
        fprintf(fp, "\n%s = %s\n", d->chiave, c->val[i]);
    }
    bool ok = fclose(fp) == 0 && rename(tmp, path) == 0;
    if (!ok) media_set_err(err, err_len, "non salvo %s: %s", path, strerror(errno));
    return ok;
}

/* ── wizard ───────────────────────────────────────────────────────────────── */

static void doppia_editor(const char *path) {
    const char *ed = getenv("EDITOR");
    pid_t p = fork();
    if (p == 0) {
        execlp(ed && ed[0] ? ed : "nano", ed && ed[0] ? ed : "nano", path, (char *)NULL);
        _exit(127);
    }
    if (p > 0) waitpid(p, NULL, 0);
}

static void doppia_mostra(const doppia_conf *c) {
    const char *sez = "";
    printf("\n  ds4-media doppia - impostazioni del lavoro (%s)\n", c->dir[0] ? c->dir : "cartella da decidere");
    for (int i = 0; i < DOPPIA_N_VOCI; i++) {
        const doppia_voce *d = &DOPPIA_VOCI[i];
        if (strcmp(sez, d->sezione)) { printf("\n  %s\n", d->sezione); sez = d->sezione; }
        const char *v = c->val[i];
        size_t n = strlen(v);
        printf("  %3d  %-22s %s%.*s%s\n", i + 1, d->etichetta, n ? "" : "(vuoto)",
               n > 70 ? 32 : (int)n, v, n > 70 ? "..." : "");
        if (n > 70) printf("  %28s...%s\n", "", v + n - 34);
    }
}

/* Cosa manca per partire: foto, voce e (per Qwen3-TTS) il testo del campione. */
static const char *doppia_mancante(const doppia_conf *c) {
    if (!doppia_get(c, "sorgente")[0]) return "sorgente";
    if (!doppia_get(c, "foto")[0]) return "foto";
    if (!doppia_get(c, "voce_campione")[0]) return "voce_campione";
    if (doppia_is(c, "voce_motore", "qwen") && !doppia_get(c, "voce_testo")[0]) return "voce_testo";
    if (!doppia_get(c, "whisper_modello")[0]) return "whisper_modello";
    return NULL;
}

bool doppia_wizard(doppia_conf *c, const char *titoli_path) {
    char line[4096], e[512];
    for (;;) {
        doppia_mostra(c);
        printf("\n  numero = modifica, t = righe del titolo, p = salva come predefiniti,\n"
               "  Invio = parti, q = esci: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return false;
        line[strcspn(line, "\r\n")] = '\0';
        if (!strcmp(line, "q")) return false;
        if (!line[0]) {
            const char *m = doppia_mancante(c);
            if (!m) return true;
            printf("\n  manca: %s (voce %d)\n", DOPPIA_VOCI[doppia_idx(m)].etichetta, doppia_idx(m) + 1);
            continue;
        }
        if (!strcmp(line, "t")) {
            if (titoli_path && access(titoli_path, F_OK) == 0) doppia_editor(titoli_path);
            else printf("\n  titoli.txt non c'e' ancora: nasce dai dati del video\n");
            continue;
        }
        if (!strcmp(line, "p")) {
            char *home = doppia_home_conf();
            printf("\n  %s\n", doppia_conf_save(c, home, e, sizeof(e)) ? "salvati come predefiniti" : e);
            free(home);
            continue;
        }
        char *end;
        long k = strtol(line, &end, 10);
        if (*end || k < 1 || k > DOPPIA_N_VOCI) { printf("\n  scelta non valida: %s\n", line); continue; }
        const doppia_voce *d = &DOPPIA_VOCI[k - 1];
        printf("\n  %s: %s\n", d->etichetta, d->aiuto);
        if (d->scelte) printf("  valori: %s\n", d->scelte);
        printf("  attuale: %s\n  nuovo (Invio = tieni, - = vuoto): ", c->val[k - 1]);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return false;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        if (!doppia_set(c, d->chiave, strcmp(line, "-") ? line : "", e, sizeof(e))) printf("\n  %s\n", e);
    }
}
