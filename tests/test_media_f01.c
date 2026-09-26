/* Regressioni di ds4_media per F0 (bug B1-B12) e F1 (O1-O7), senza GPU, contro il
 * finto ComfyUI di media_fake.h. Ogni test nomina il bug o l'ottimizzazione che copre.
 *
 *   make test-media-f01 */
#include "../ds4_media.h"
#include "../ds4_media_int.h"
#include "media_fake.h"

#include <sys/stat.h>
#include <sys/wait.h>

static int falliti;
#define VERIFICA(c, ...) do { if (!(c)) { falliti++; printf("FALLITO: " __VA_ARGS__); printf("\n"); } \
                              else printf("ok: %s\n", #c); } while (0)

static fake_comfy F;
static char DIR_[256];

static ds4_media *mk(bool gate, long avail_kib) {
    ds4_media_config c = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_,
                          .no_gate = !gate, .avail_override_kib = avail_kib};
    return ds4_media_create(&c);
}

static int g_steps, g_total;
static void on_progress(void *pd, int s, int t) { (void)pd; (void)s; g_steps++; g_total = t; }

static void write_png(const char *path, int w, int h) {
    unsigned char p[64];
    fk_png(p, w, h);
    FILE *f = fopen(path, "wb");
    fwrite(p, 1, sizeof(p), f);
    fclose(f);
}

/* O1: eventi websocket (con ping, frammenti, binario, eventi di altri lavori). */
static void t_websocket(void) {
    fk_reset(&F);
    ds4_media_config c = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_, .no_gate = true,
                          .progress = on_progress};
    ds4_media *m = ds4_media_create(&c);
    ds4_media_image_req r = {.prompt = "un faro", .seed = 1};
    ds4_media_result res;
    char err[256] = {0};
    g_steps = g_total = 0;
    bool ok = ds4_media_image(m, &r, &res, err, sizeof(err));
    VERIFICA(ok, "O1 generazione via websocket: %s", err);
    VERIFICA(g_steps == 3 && g_total == 3, "O1 progresso 3/3 solo del nostro lavoro (%d, %d)", g_steps, g_total);
    VERIFICA(F.got_pong == 1, "O1 pong al ping (%d)", F.got_pong);
    VERIFICA(strstr(F.graph, F.client_id) != NULL, "O1 client_id del ws uguale a quello del /prompt");
    VERIFICA(F.history_gets <= 2, "O1 niente polling ogni secondo (%d GET)", F.history_gets);
    VERIFICA(ok && res.width == 1024 && res.height == 1024, "dimensioni dal PNG");
    ds4_media_result_free(&res);
    ds4_media_free(m);
}

/* O1: senza websocket, o con websocket che chiude subito, si ripiega su /history. */
static void t_ws_fallback(int mode) {
    fk_reset(&F);
    F.ws_mode = mode;
    F.hist_pending = 1;
    ds4_media *m = mk(false, 0);
    ds4_media_image_req r = {.prompt = "ripiego", .seed = 1};
    ds4_media_result res;
    char err[256] = {0};
    VERIFICA(ds4_media_image(m, &r, &res, err, sizeof(err)), "O1 ripiego su /history (ws %d): %s", mode, err);
    ds4_media_result_free(&res);
    ds4_media_free(m);
}

/* B1 + O3: modifica senza size = latente del nodo 4, risoluzione dall'area del
 * riferimento, cache dei riferimenti se ComfyUI ce l'ha. */
static void t_edit_latent(void) {
    fk_reset(&F);
    F.cache_node = true;
    F.view_w = 1536; F.view_h = 1024;
    char ref[512];
    snprintf(ref, sizeof(ref), "%s/foto.png", DIR_);
    write_png(ref, 1536, 1024);
    const char *refs[] = {ref};
    ds4_media *m = mk(false, 0);
    ds4_media_image_req r = {.prompt = "aggiungi un cappello", .seed = 1, .refs = refs, .n_refs = 1};
    ds4_media_result res;
    char err[256] = {0};
    bool ok = ds4_media_image(m, &r, &res, err, sizeof(err));
    VERIFICA(ok, "B1 modifica: %s", err);
    VERIFICA(strstr(F.graph, "\"latent_image\":[\"4\",2]") != NULL, "B1 KSampler sul latente del nodo 4");
    VERIFICA(strstr(F.graph, "EmptyLatentImage") == NULL, "B1 niente EmptyLatentImage nella modifica");
    VERIFICA(strstr(F.graph, "\"resolution\":1248") != NULL, "B1 resolution 1248 per 1536x1024");
    VERIFICA(strstr(F.graph, "QwenImage21Cache") && strstr(F.graph, "\"model\":[\"9\",0]"), "O3 cache dei riferimenti");
    VERIFICA(ok && res.width == 1536 && res.height == 1024, "B1 uscita 1536x1024 (%dx%d)", res.width, res.height);
    ds4_media_result_free(&res);

    /* n=2: il latente del riferimento ripetuto */
    ds4_media_image_req r2 = r;
    r2.n = 2;
    F.n_outputs = 2;
    ok = ds4_media_image(m, &r2, &res, err, sizeof(err));
    VERIFICA(ok && strstr(F.graph, "RepeatLatentBatch") && strstr(F.graph, "\"amount\":2"), "O2 modifica n=2: %s", err);
    ds4_media_result_free(&res);

    /* size esplicita: la si rispetta */
    ds4_media_image_req r3 = r;
    r3.width = 1024; r3.height = 1024;
    F.n_outputs = 1;
    ok = ds4_media_image(m, &r3, &res, err, sizeof(err));
    VERIFICA(ok && strstr(F.graph, "EmptyLatentImage") && strstr(F.graph, "\"width\":1024"), "B1 size esplicita rispettata");
    ds4_media_result_free(&res);
    ds4_media_free(m);
}

/* B2: riferimenti omonimi in cartelle diverse restano distinti. */
static void t_ref_names(void) {
    fk_reset(&F);
    char a[512], b[512], da[300], db[300];
    snprintf(da, sizeof(da), "%s/a", DIR_);
    snprintf(db, sizeof(db), "%s/b", DIR_);
    mkdir(da, 0755);
    mkdir(db, 0755);
    snprintf(a, sizeof(a), "%s/foto.png", da);
    snprintf(b, sizeof(b), "%s/foto.png", db);
    write_png(a, 512, 512);
    write_png(b, 640, 512);
    const char *refs[] = {a, b};
    ds4_media *m = mk(false, 0);
    ds4_media_image_req r = {.prompt = "unisci", .seed = 1, .refs = refs, .n_refs = 2};
    ds4_media_result res;
    char err[256] = {0};
    VERIFICA(ds4_media_image(m, &r, &res, err, sizeof(err)), "B2 due riferimenti: %s", err);
    VERIFICA(F.uploads == 2 && strcmp(F.upload_names[0], F.upload_names[1]) != 0,
             "B2 nomi diversi (%s / %s)", F.upload_names[0], F.upload_names[1]);
    VERIFICA(strstr(F.upload_names[0], "foto") == NULL, "B2 nome generato, non il basename");
    ds4_media_result_free(&res);
    ds4_media_free(m);
}

/* B3: la cancellazione toglie il lavoro dalla coda e lo interrompe. */
static long g_cancel_at;
static bool cancel_later(void *pd) { (void)pd; return media_now_ms() >= g_cancel_at; }
static void t_cancel(void) {
    fk_reset(&F);
    F.ws_mode = WS_SILENT;
    F.hist_pending = -1;
    ds4_media *m = mk(false, 0);
    g_cancel_at = media_now_ms() + 400;
    ds4_media_image_req r = {.prompt = "infinito", .seed = 1, .cancel = cancel_later};
    ds4_media_result res;
    char err[256] = {0};
    long t0 = media_now_ms();
    bool ok = ds4_media_image(m, &r, &res, err, sizeof(err));
    VERIFICA(!ok && strstr(err, "annullato"), "B3 annullato: %s", err);
    VERIFICA(media_now_ms() - t0 < 3000, "B3 annullamento entro 3 s");
    VERIFICA(F.queue_deletes == 1 && strstr(F.last_delete, "fakepid"), "B3 /queue delete con l'id (%s)", F.last_delete);
    VERIFICA(F.interrupts == 1, "B3 /interrupt");
    ds4_media_free(m);
}

/* B4: la memoria che ComfyUI tiene gia' conta come disponibile. */
static void t_gate(void) {
    fk_reset(&F);
    F.torch_bytes = 12L << 30;
    ds4_media *m = mk(true, 10L * 1024 * 1024);   /* 10 GiB liberi + 12 = 22 < 24 */
    ds4_media_image_req r = {.prompt = "x", .seed = 1};
    ds4_media_result res;
    char err[256] = {0};
    VERIFICA(!ds4_media_image(m, &r, &res, err, sizeof(err)) && strstr(err, "GiB"), "B4 22 GiB non bastano: %s", err);
    F.torch_bytes = 16L << 30;                     /* 10 + 16 = 26 >= 24 */
    VERIFICA(ds4_media_image(m, &r, &res, err, sizeof(err)), "B4 26 GiB bastano: %s", err);
    ds4_media_result_free(&res);
    VERIFICA(ds4_media_image_need_gib(DS4_MEDIA_INT8, 1024, 1024, 1) == 24, "B11 fabbisogno int8 24 GiB");
    VERIFICA(ds4_media_image_need_gib(DS4_MEDIA_BF16, 1536, 1024, 1) == 43, "B11 fabbisogno bf16 43 GiB");
    ds4_media_free(m);
}

/* B4 su questa macchina: torch_vram_total sottostima con cudaMallocAsync; la memoria
 * vera e' quella del processo in ascolto sulla porta di ComfyUI. Qui il finto ComfyUI
 * e' dentro il test: il pid trovato deve essere il nostro. */
static void t_pid_porta(void) {
    VERIFICA(media_pid_on_port(F.port) == (long)getpid(), "B4 pid in ascolto sulla porta del finto ComfyUI (%ld)", media_pid_on_port(F.port));
    VERIFICA(media_pid_on_port(1) == -1, "B4 nessuno in ascolto sulla porta 1");
    VERIFICA(media_gpu_used_kib(-1) == 0, "B4 pid sconosciuto: nessuna memoria GPU");
}

/* B5: limiti su lato e area, simmetrici. */
static void t_sizes(void) {
    int w, h;
    VERIFICA(ds4_media_parse_size("1024x1792", &w, &h), "B5 1024x1792 verticale");
    VERIFICA(ds4_media_parse_size("1536x2752", &w, &h), "B5 1536x2752");
    VERIFICA(!ds4_media_parse_size("2752x2752", &w, &h), "B5 area oltre il limite");
    VERIFICA(!ds4_media_parse_size("2784x32", &w, &h), "B5 lato oltre 2752");
    VERIFICA(!ds4_media_parse_size("1024x1024abc", &w, &h), "B5 coda spazzatura");
    VERIFICA(!ds4_media_parse_size("x1024", &w, &h) && !ds4_media_parse_size("1024x", &w, &h), "B5 lato mancante");
    VERIFICA(!ds4_media_parse_size("99999999999999999999x32", &w, &h), "B5 overflow");
}

/* B9 + O2: prompt negativo alza cfg; n immagini in un grafo solo. */
static void t_negative_batch(void) {
    fk_reset(&F);
    F.n_outputs = 3;
    ds4_media *m = mk(false, 0);
    ds4_media_image_req r = {.prompt = "prato", .negative = "sfocato", .seed = 1, .n = 3};
    ds4_media_result res;
    char err[256] = {0};
    bool ok = ds4_media_image(m, &r, &res, err, sizeof(err));
    VERIFICA(ok && strstr(F.graph, "\"cfg\":2.500"), "B9 cfg 2.5 con negativo: %s", err);
    VERIFICA(strstr(F.graph, "\"batch_size\":3") && F.prompts == 1, "O2 batch 3 in un solo /prompt");
    VERIFICA(ok && res.n_files == 3, "O2 tre file (%d)", res.n_files);
    if (ok && res.n_files == 3)
        VERIFICA(strcmp(res.files[0], res.files[1]) && strcmp(res.files[1], res.files[2]), "O2 nomi distinti");
    ds4_media_result_free(&res);
    ds4_media_image_req r2 = {.prompt = "prato", .negative = "sfocato", .seed = 1, .cfg = 1.0};
    F.n_outputs = 1;
    ok = ds4_media_image(m, &r2, &res, err, sizeof(err));
    VERIFICA(ok && strstr(F.graph, "\"cfg\":1.000"), "B9 cfg esplicito rispettato");
    ds4_media_result_free(&res);
    ds4_media_free(m);
}

/* B12: cartella non scrivibile = errore, non un successo con file mancanti. */
static void t_unwritable(void) {
    fk_reset(&F);
    char ro[300];
    snprintf(ro, sizeof(ro), "%s/sola-lettura", DIR_);
    mkdir(ro, 0555);
    ds4_media_config c = {.host = "127.0.0.1", .port = F.port, .media_dir = ro, .no_gate = true};
    ds4_media *m = ds4_media_create(&c);
    ds4_media_image_req r = {.prompt = "x", .seed = 1};
    ds4_media_result res;
    char err[256] = {0};
    bool ok = ds4_media_image(m, &r, &res, err, sizeof(err));
    VERIFICA(getuid() == 0 || (!ok && res.n_files == 0), "B12 errore su cartella non scrivibile: %s", err);
    ds4_media_result_free(&res);
    ds4_media_free(m);
    chmod(ro, 0755);
}

/* Intestazioni JPEG e WebP. */
static void t_dims(void) {
    unsigned char jpg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0, 0, 0xFF, 0xC0, 0x00, 0x11, 0x08,
                           0x03, 0x00, 0x04, 0x00, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
    unsigned char webp[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X',
                              10, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x03, 0x00, 0xFF, 0x02, 0x00};
    int w = 0, h = 0;
    VERIFICA(media_image_dims(jpg, sizeof(jpg), &w, &h) && w == 1024 && h == 768, "JPEG 1024x768 (%dx%d)", w, h);
    VERIFICA(media_image_dims(webp, sizeof(webp), &w, &h) && w == 1024 && h == 768, "WebP VP8X 1024x768 (%dx%d)", w, h);
    VERIFICA(!media_image_dims(jpg, 10, &w, &h), "JPEG troncato rifiutato");
}

/* B10: parser della CLI (eseguibile vero contro il finto ComfyUI). */
/* Le opzioni di connessione vanno subito dopo il sottocomando: dopo un "--" sarebbero
 * parole della descrizione e ds4-media parlerebbe con il ComfyUI vero su 8188. */
static int run_cli(const char *args) {
    char sub[16], cmd[2048];
    int k = 0;
    while (args[k] && args[k] != ' ' && k < 15) { sub[k] = args[k]; k++; }
    sub[k] = '\0';
    /* HOME finto e --tieni-comfy: la CLI non deve mai toccare ~/comfy/ComfyUI/output. */
    snprintf(cmd, sizeof(cmd), "HOME=%s ./ds4-media %s --comfy-port %d --dir %s --tieni-comfy --no-free --no-vista "
             "--no-gate %s </dev/null >/dev/null 2>&1", DIR_, sub, F.port, DIR_, args + k);
    int st = system(cmd);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void t_cli(void) {
    if (access("./ds4-media", X_OK) != 0) { printf("salto i test CLI: manca ./ds4-media\n"); return; }
    fk_reset(&F);
    VERIFICA(run_cli("img gatto rosso --seed 3") == 0, "B10 opzioni dopo la descrizione");
    VERIFICA(strstr(F.graph, "\"gatto rosso\"") != NULL, "B10 parole unite nella descrizione");
    VERIFICA(run_cli("img --rif /nonesiste.png") == 2, "B10 --rif senza descrizione e' un errore");
    VERIFICA(run_cli("img --seed abc gatto") == 2, "B10 seed non numerico rifiutato");
    VERIFICA(run_cli("img --boh gatto") == 2, "B10 opzione sconosciuta rifiutata");
    VERIFICA(run_cli("img -- --non-e-un-flag") == 0 && strstr(F.graph, "--non-e-un-flag"), "B10 -- separa la descrizione");
    VERIFICA(run_cli("health extra") == 2, "B10 argomento inatteso rifiutato");
}

/* ── serve ────────────────────────────────────────────────────────────────── */

typedef struct { ds4_media *m; int port; volatile int stop; } serve_arg;
static void *serve_thread(void *a) {
    serve_arg *s = a;
    char err[128];
    ds4_media_serve(s->m, s->port, &s->stop, err, sizeof(err));
    return NULL;
}

/* Richiesta grezza: restituisce tutta la risposta (malloc'd). close_after_ms > 0 chiude
 * il socket prima della risposta, come un client che se ne va. */
static char *raw_req(int port, const char *req, int close_after_ms, size_t *len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return NULL; }
    if (write(fd, req, strlen(req)) < 0) {}
    if (close_after_ms > 0) { usleep(close_after_ms * 1000); close(fd); return NULL; }
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap + 1);
    ssize_t r;
    while ((r = read(fd, b + n, cap - n)) > 0) { n += (size_t)r; if (n == cap) { cap *= 2; b = realloc(b, cap + 1); } }
    b[n] = '\0';
    close(fd);
    *len = n;
    return b;
}

static char *post_gen(int port, const char *json, const char *host, size_t *len) {
    char req[2048];
    snprintf(req, sizeof(req), "POST /v1/images/generations HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n%s", host, strlen(json), json);
    return raw_req(port, req, 0, len);
}

static void t_serve(void) {
    fk_reset(&F);
    F.n_outputs = 2;
    ds4_media *m = mk(false, 0);
    serve_arg sa = {.m = m, .port = 19031};
    pthread_t th;
    pthread_create(&th, NULL, serve_thread, &sa);
    usleep(200000);

    size_t n = 0;
    char *r = post_gen(sa.port, "{\"prompt\":\"due mele\",\"n\":2}", "localhost:19031", &n);
    char *body = r ? strstr(r, "\r\n\r\n") : NULL;
    char *cl = r ? strcasestr(r, "Content-Length:") : NULL;
    VERIFICA(r && strstr(r, " 200 ") && body && cl, "O5 serve b64 risponde 200");
    if (body && cl) {
        body += 4;
        VERIFICA((size_t)strtol(cl + 15, NULL, 10) == strlen(body), "B7/O5 Content-Length esatto (%ld vs %zu)",
                 strtol(cl + 15, NULL, 10), strlen(body));
        unsigned char png[64];
        fk_png(png, 1024, 1024);
        char *b64 = media_base64_encode(png, sizeof(png));
        char *second = strstr(body, "},{\"b64_json\":\"");
        VERIFICA(strstr(body, b64) && second && strstr(second, b64), "O5 due immagini, base64 corretto");
        free(b64);
    }
    VERIFICA(F.prompts == 1 && strstr(F.graph, "\"batch_size\":2"), "O2 serve n=2 in un solo lavoro");
    free(r);

    F.n_outputs = 1;
    r = post_gen(sa.port, "{\"prompt\":\"una pera\",\"response_format\":\"url\"}", "example.test:19031", &n);
    char *u = r ? strstr(r, "\"url\":\"http://example.test:19031/v1/media/files/img-") : NULL;
    VERIFICA(u != NULL, "B6 URL assoluto dall'header Host");
    if (u) {
        char name[256] = {0}, req[512];
        sscanf(strstr(u, "/files/") + 7, "%255[^\"]", name);
        snprintf(req, sizeof(req), "GET /v1/media/files/%s HTTP/1.1\r\nHost: x\r\n\r\n", name);
        size_t fl = 0;
        char *f = raw_req(sa.port, req, 0, &fl);
        VERIFICA(f && strstr(f, " 200 ") && strstr(f, "image/png"), "B6 il file dell'URL si scarica");
        free(f);
    }
    free(r);

    r = post_gen(sa.port, "{\"prompt\":\"x\",\"steps\":99}", "x", &n);
    VERIFICA(r && strstr(r, " 400 "), "400 per passi fuori limite (errore del chiamante)");
    free(r);
    r = post_gen(sa.port, "{\"prompt\":\"x\",\"size\":\"1024x1792\"}", "x", &n);
    VERIFICA(r && strstr(r, " 200 ") && strstr(F.graph, "\"height\":1792"), "B5 serve accetta 1024x1792");
    free(r);

    /* il client se ne va: il lavoro esce dalla coda di ComfyUI */
    F.ws_mode = WS_SILENT;
    F.hist_pending = -1;
    char req[512];
    const char *js = "{\"prompt\":\"abbandonato\"}";
    snprintf(req, sizeof(req), "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s", strlen(js), js);
    raw_req(sa.port, req, 500, &n);
    for (int i = 0; i < 40 && F.queue_deletes == 0; i++) usleep(100000);
    VERIFICA(F.queue_deletes == 1 && F.interrupts == 1, "B3 disconnessione: lavoro tolto dalla coda (%d)", F.queue_deletes);

    sa.stop = 1;
    pthread_join(th, NULL);
    ds4_media_free(m);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fk_start(&F);
    snprintf(DIR_, sizeof(DIR_), "/tmp/ds4mf01XXXXXX");
    if (!mkdtemp(DIR_)) { perror("mkdtemp"); return 1; }
    t_websocket();
    t_ws_fallback(WS_NONE);
    t_ws_fallback(WS_CLOSE);
    t_edit_latent();
    t_ref_names();
    t_cancel();
    t_gate();
    t_pid_porta();
    t_sizes();
    t_negative_batch();
    t_unwritable();
    t_dims();
    t_cli();
    t_serve();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DIR_);
    if (system(cmd) != 0) printf("pulizia non riuscita\n");
    printf(falliti ? "\n%d FALLITI\n" : "\ntutti i test superati\n", falliti);
    return falliti != 0;
}
