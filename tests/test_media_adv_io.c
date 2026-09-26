/* Test AVVERSARI di ds4_media, parte ingressi: limiti della richiesta, cfg e gate; il
 * serve con header, corpi e Host ostili, traversal sui file, streaming base64; la CLI
 * con opzioni monche, numeri sporchi e Ctrl+C. Il ds4-media vero parla solo con il
 * finto ComfyUI, con HOME temporanea e --tieni-comfy (mai la cartella vera di
 * ComfyUI). Vedi test_media_adv.c.
 *
 *   make test-media-adv */
#include "media_adv_util.h"

static void t_limits(void) {
    inizio("limiti");
    int w, h;
    VERIFICA(!ds4_media_parse_size(" 1024x1024", &w, &h), "B5 parse_size strict: spazio iniziale");
    VERIFICA(!ds4_media_parse_size("+1024x1024", &w, &h), "B5 parse_size strict: segno +");
    VERIFICA(!ds4_media_parse_size("1024x+1024", &w, &h), "B5 parse_size strict: + nell'altezza");
    VERIFICA(!ds4_media_parse_size("1024x 1024", &w, &h), "B5 parse_size strict: spazio interno");
    VERIFICA(!ds4_media_parse_size("0x0", &w, &h) && !ds4_media_parse_size("-32x32", &w, &h), "B5 0x0 e negativi");
    VERIFICA(ds4_media_parse_size("2752x1536", &w, &h) && ds4_media_parse_size("1536x2752", &w, &h) &&
             !ds4_media_parse_size("2752x1568", &w, &h) && !ds4_media_parse_size("2784x32", &w, &h), "B5 estremi esatti");
    VERIFICA(!ds4_media_parse_size("", &w, &h) && !ds4_media_parse_size("x", &w, &h) && !ds4_media_parse_size(NULL, &w, &h), "B5 vuoti");

    ds4_media *m = mk(NULL);
    jrun j;
    af_reset(&F);
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.seed = 1; j.req.cfg = NAN;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid && !strstr(F.graph, "nan"), "B9 cfg NaN rifiutato (ok=%d, grafo con nan: %d) %s", j.ok, F.graph[0] && strstr(F.graph, "nan") != NULL, j.err);
    ds4_media_result_free(&j.res);
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.seed = 1; j.req.cfg = 20.001;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "B9 cfg 20.001 rifiutato");
    af_reset(&F);
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.seed = 1; j.req.cfg = 20.0;
    VERIFICA(jrun_go(&j, 20000) && j.ok && strstr(F.graph, "\"cfg\":20.000"), "B9 cfg 20 accettato");
    ds4_media_result_free(&j.res);
    af_reset(&F);
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.seed = 1; j.req.cfg = -3; j.req.negative = "n";
    VERIFICA(jrun_go(&j, 20000) && j.ok && strstr(F.graph, "\"cfg\":2.500"), "B9 cfg negativo = default, 2.5 col negativo");
    ds4_media_result_free(&j.res);
    static char lungo[4002];
    memset(lungo, 'a', 4001);
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = lungo; j.req.seed = 1;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "prompt di 4001 caratteri rifiutato");
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.negative = lungo;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "negativo di 4001 caratteri rifiutato");
    int bad_n[] = {5, -1};
    for (int i = 0; i < 2; i++) {
        memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.n = bad_n[i];
        jrun_go(&j, 20000);
        VERIFICA(!j.ok && j.res.invalid, "O2 n=%d rifiutato", bad_n[i]);
    }
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.n_refs = 11;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "11 riferimenti rifiutati");
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.width = 2752; j.req.height = 1568;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "B5 2752x1568 (area) rifiutato via API");
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.width = -32; j.req.height = 32;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "B5 larghezza negativa rifiutata");
    memset(&j, 0, sizeof(j)); j.m = m; j.req.prompt = "x"; j.req.steps = 61;
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && j.res.invalid, "steps 61 rifiutato");
    ds4_media_free(m);

    /* B4/B11: gate = MemAvailable + torch_vram_total */
    VERIFICA(ds4_media_image_need_gib(DS4_MEDIA_INT8, 1024, 1024, 4) == 30, "B11 int8 1024^2 n=4 = 30 GiB (%ld)", ds4_media_image_need_gib(DS4_MEDIA_INT8, 1024, 1024, 4));
    VERIFICA(ds4_media_image_need_gib(DS4_MEDIA_BF16, 2752, 1536, 4) == 73, "B11 bf16 2752x1536 n=4 = 73 GiB (%ld)", ds4_media_image_need_gib(DS4_MEDIA_BF16, 2752, 1536, 4));
    ds4_media_config gc = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_, .avail_override_kib = 10L << 20};
    ds4_media *g = ds4_media_create(&gc);
    af_reset(&F);
    snprintf(F.stats, sizeof(F.stats), "{\"devices\":[{\"torch_vram_total\": -99999999999}]}");
    memset(&j, 0, sizeof(j)); j.m = g; j.req.prompt = "x";
    jrun_go(&j, 20000);
    VERIFICA(!j.ok && !j.res.invalid && strstr(j.err, "GiB"), "B4 torch_vram_total negativo ignorato: 10 GiB non bastano (%s)", j.err);
    snprintf(F.stats, sizeof(F.stats), "{\"devices\":[{\"torch_vram_total\": \"abc\", \"torch_vram_total\": %ld}]}", 14L << 30);
    memset(&j, 0, sizeof(j)); j.m = g; j.req.prompt = "x";
    VERIFICA(jrun_go(&j, 20000) && j.ok, "B4 10 + 14 GiB bastano per 24 (%s)", j.err);
    ds4_media_result_free(&j.res);
    VERIFICA(ds4_media_comfy_avail_gib(g) == 24, "B4 comfy_avail_gib = 24 (%ld)", ds4_media_comfy_avail_gib(g));
    ds4_media_free(g);
}

/* ── serve ostile ─────────────────────────────────────────────────────────── */
typedef struct { ds4_media *m; int port; volatile int stop; bool ret; } sarg;
static void *serve_th(void *a) { sarg *s = a; char e[128]; s->ret = ds4_media_serve(s->m, s->port, &s->stop, e, sizeof(e)); return NULL; }
static int SP;
static char *gen(const char *json, const char *host, size_t *n) {
    char req[16384];
    snprintf(req, sizeof(req), "POST /v1/images/generations HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n%s", host, strlen(json), json);
    return af_rawstr(SP, req, n);
}
static bool b64_ok(const char *resp) {   /* Content-Length esatto e base64 uguale al file su disco */
    const char *cl = resp ? strcasestr(resp, "Content-Length:") : NULL, *body = resp ? strstr(resp, "\r\n\r\n") : NULL;
    if (!cl || !body) return false;
    body += 4;
    if ((size_t)strtol(cl + 15, NULL, 10) != strlen(body)) return false;
    const char *b = strstr(body, "\"b64_json\":\"");
    if (!b) return false;
    b += 12;
    const char *e = strchr(b, '"');
    char *want = media_base64_encode(F.view_body, F.view_len);
    bool ok = e && strlen(want) == (size_t)(e - b) && memcmp(want, b, strlen(want)) == 0;
    free(want);
    return ok;
}
static void *par_th(void *a) { size_t n; char *r = gen("{\"prompt\":\"parallelo\"}", "x", &n); *(int *)a = af_status(r); free(r); return NULL; }

static void t_serve(void) {
    inizio("serve");
    af_reset(&F);
    ds4_media *m = mk(NULL);
    SP = af_free_port();
    sarg sa = {.m = m, .port = SP};
    pthread_t th;
    pthread_create(&th, NULL, serve_th, &sa);
    usleep(200000);
    size_t n;
    char *r;
    static char big[100000];
    memset(big, 'a', sizeof(big) - 1);
    char *req = malloc(110000);
    snprintf(req, 110000, "GET /v1/models HTTP/1.1\r\nX: %s\r\n\r\n", big);
    r = af_rawstr(SP, req, &n);
    VERIFICA(af_status(r) == 431, "header di 100 KB -> 431 (%d)", af_status(r));
    free(r);
    big[70000] = '\0';
    snprintf(req, 110000, "GET /v1/models HTTP/1.1\r\nX: %s\r\n\r\n", big);
    r = af_rawstr(SP, req, &n);
    VERIFICA(af_status(r) == 431, "header di 70 KB (oltre 64 KiB) -> 431 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n{", &n);
    VERIFICA(af_status(r) == 413, "corpo dichiarato 2 MB -> 413 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", &n);
    VERIFICA(af_status(r) == 400, "Content-Length negativo -> 400 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", &n);
    VERIFICA(af_status(r) == 400, "Content-Length non numerico -> 400 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n", &n);
    VERIFICA(af_status(r) == 413 || af_status(r) == 400, "Content-Length enorme -> 413/400 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\n\r\n", &n);
    VERIFICA(af_status(r) == 400, "POST senza corpo -> 400 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 500\r\n\r\n{\"a\"}", &n);
    VERIFICA(af_status(r) == 400, "Content-Length doppio -> 400, nessun blocco (%d)", af_status(r));
    free(r);
    r = gen("{prompt: x}", "x", &n);
    VERIFICA(af_status(r) == 400, "JSON invalido -> 400 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "OPTIONS /v1/images/generations HTTP/1.1\r\nHost: x\r\n\r\n", &n);
    VERIFICA(af_status(r) == 204, "OPTIONS -> 204 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "GET /altro HTTP/1.1\r\nHost: x\r\n\r\n", &n);
    VERIFICA(af_status(r) == 404, "rotta ignota -> 404 (%d)", af_status(r));
    free(r);
    r = af_rawstr(SP, "GET /v1/models HTTP/1.1\r\n\r\n", &n);
    VERIFICA(af_status(r) == 200 && strstr(r, "qwen-image-2.1"), "GET /v1/models senza Host -> 200");
    free(r);

    /* parametri al limite */
    F.n_outputs = 4;
    r = gen("{\"prompt\":\"x\",\"n\":1000}", "x", &n);
    VERIFICA(af_status(r) == 200 && strstr(F.graph, "\"batch_size\":4"), "n=1000 -> 4 (%d)", af_status(r));
    free(r);
    F.n_outputs = 1;
    r = gen("{\"prompt\":\"x\",\"n\":0,\"size\":\"auto\"}", "x", &n);
    VERIFICA(af_status(r) == 200 && strstr(F.graph, "\"batch_size\":1") && strstr(F.graph, "\"width\":1024"), "n=0, size auto -> 1 a 1024 (%d)", af_status(r));
    free(r);
    const char *badsz[] = {"0x0", "-32x32", "1024x1024 ", "2752x1568", "1000x1000", "1024"};
    for (int i = 0; i < 6; i++) {
        char js[128];
        snprintf(js, sizeof(js), "{\"prompt\":\"x\",\"size\":\"%s\"}", badsz[i]);
        r = gen(js, "x", &n);
        VERIFICA(af_status(r) == 400, "B5 size '%s' -> 400 (%d)", badsz[i], af_status(r));
        free(r);
    }
    r = gen("{\"prompt\":\"x\",\"steps\":-5}", "x", &n);
    VERIFICA(af_status(r) == 400, "steps -5 -> 400 (%d)", af_status(r));
    free(r);
    r = gen("{\"prompt\":\"x\",\"steps\":1000000000000}", "x", &n);
    VERIFICA(af_status(r) == 400, "steps 10^12 -> 400 (%d)", af_status(r));
    free(r);
    r = gen("{\"prompt\":\"x\",\"seed\":99999999999999999999}", "x", &n);
    VERIFICA(af_status(r) == 200 && strstr(F.graph, "\"seed\":9223372036854775807"), "seed enorme saturato, non negativo (%d)", af_status(r));
    free(r);

    /* B6: Host ostile non finisce nell'URL */
    r = gen("{\"prompt\":\"x\",\"response_format\":\"url\"}", "evil\r\nX-Inj: 1", &n);
    VERIFICA(r && strstr(r, "\"url\":\"http://evil/v1/media/files/img-"), "B6 Host con CRLF: solo la parte pulita");
    free(r);
    const char *hosts[] = {"a b", "\"x\"", "evil.com/path", "e@vil", "x;y", "127.0.0.1:1/../"};
    for (int i = 0; i < 6; i++) {
        r = gen("{\"prompt\":\"x\",\"response_format\":\"url\"}", hosts[i], &n);
        const char *u = r ? strstr(r, "\"url\":\"") : NULL;
        char base[64] = {0};
        snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1/media/files/img-", SP);
        VERIFICA(u && strncmp(u + 7, base, strlen(base)) == 0, "B6 Host [%s] -> URL di ripiego (%.*s)", hosts[i], u ? 60 : 1, u ? u : "-");
        free(r);
    }
    r = gen("{\"prompt\":\"x\",\"response_format\":\"url\"}", "[::1]:8010", &n);
    VERIFICA(r && strstr(r, "\"url\":\"http://[::1]:8010/v1/media/files/img-"), "B6 Host IPv6 accettato");
    free(r);

    /* B7/O5: base64 esatto per lunghezze 1..5, 3000, 262144 e per il PNG intero */
    size_t sizes[] = {1, 2, 3, 4, 5, 3000, 262145, sizeof(F.view_body)};
    for (size_t i = 0; i < 8; i++) {
        F.view_len = sizes[i];
        for (size_t k = 0; k < sizes[i]; k++) F.view_body[k] = (unsigned char)(k * 31 + 7);
        r = gen("{\"prompt\":\"x\"}", "x", &n);
        VERIFICA(af_status(r) == 200 && b64_ok(r), "O5 b64 esatto per %zu byte (%d)", sizes[i], af_status(r));
        free(r);
    }
    af_png(F.view_body, 1024, 1024); F.view_len = 64;

    /* traversal e nomi su /v1/media/files */
    char okf[400];
    snprintf(okf, sizeof(okf), "%s/ok.png", DIR_);
    write_png(okf, 4, 4);
    r = af_rawstr(SP, "GET /v1/media/files/ok.png HTTP/1.1\r\n\r\n", &n);
    VERIFICA(af_status(r) == 200 && n > 64, "controllo: file esistente servito");
    free(r);
    const char *names[] = {"..", "../ok.png", "..%2fok.png", "%2e%2e/ok.png", "a/../ok.png", ".ok.png", ".ds4-media-abc", "/etc/passwd", "co/legit.png", "ok.png/", "..\\ok.png"};
    for (int i = 0; i < 11; i++) {
        char rq[600];
        snprintf(rq, sizeof(rq), "GET /v1/media/files/%s HTTP/1.1\r\n\r\n", names[i]);
        r = af_rawstr(SP, rq, &n);
        VERIFICA(af_status(r) == 400 || af_status(r) == 404, "files/%s -> %d", names[i], af_status(r));
        free(r);
    }
    r = af_rawstr(SP, "GET /v1/media/files/ HTTP/1.1\r\n\r\n", &n);
    VERIFICA(af_status(r) == 400, "files/ vuoto -> 400 (%d)", af_status(r));
    free(r);

    /* client in parallelo: tutti serviti, un lavoro alla volta */
    pthread_t pt[6];
    int st[6] = {0};
    F.prompts = 0;
    for (int i = 0; i < 6; i++) pthread_create(&pt[i], NULL, par_th, &st[i]);
    int ok200 = 0;
    for (int i = 0; i < 6; i++) { pthread_join(pt[i], NULL); ok200 += st[i] == 200; }
    VERIFICA(ok200 == 6 && F.prompts == 6, "6 client paralleli -> 6 x 200, 6 lavori serializzati (%d, %d)", ok200, F.prompts);

    /* stop mentre un lavoro aspetta e il client se ne va: serve torna (true) e il lavoro esce dalla coda */
    af_reset(&F);
    F.ws_mode = AW_SILENT;
    F.hist_pending = -1;
    int fd = af_connect(SP);
    const char *js = "{\"prompt\":\"abbandonato\"}";
    char rq[512];
    int rl = snprintf(rq, sizeof(rq), "POST /v1/images/generations HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s", strlen(js), js);
    af_write(fd, rq, (size_t)rl);
    usleep(600000);
    sa.stop = 1;
    close(fd);
    long t0 = af_now_ms();
    pthread_join(th, NULL);
    VERIFICA(sa.ret && af_now_ms() - t0 < 10000 && F.queue_deletes == 1 && F.interrupts == 1,
             "stop + disconnessione: serve torna true in %ld ms, lavoro tolto (%d/%d)", af_now_ms() - t0, F.queue_deletes, F.interrupts);
    F.ws_stop = 1;
    free(req);
    ds4_media_free(m);
}

/* ── CLI ──────────────────────────────────────────────────────────────────── */
/* Esegue ./ds4-media <sub> --comfy-port <finto> --dir DIR_ ... con HOME=DIR_ (mai la
 * cartella vera di ComfyUI). sig1/sig2 > 0: SIGINT dopo tanti ms. Torna lo stato (-1 = ucciso). */
static int cli(const char *sub, const char *const *extra, int nextra, int sig1, int sig2, long *ms) {
    const char *argv[64];
    int k = 0;
    char port[16];
    snprintf(port, sizeof(port), "%d", F.port);
    argv[k++] = CLI_; argv[k++] = sub;
    argv[k++] = "--comfy-port"; argv[k++] = port; argv[k++] = "--dir"; argv[k++] = DIR_;
    argv[k++] = "--no-free"; argv[k++] = "--no-vista"; argv[k++] = "--no-gate"; argv[k++] = "--tieni-comfy";
    for (int i = 0; i < nextra && k < 62; i++) argv[k++] = extra[i];
    argv[k] = NULL;
    long t0 = af_now_ms();
    pid_t p = fork();
    if (p == 0) {
        setenv("HOME", DIR_, 1);
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
        execv(CLI_, (char *const *)argv);
        _exit(99);
    }
    int st = 0;
    bool s1 = false, s2 = false;
    for (;;) {
        pid_t w = waitpid(p, &st, WNOHANG);
        if (w == p) break;
        long el = af_now_ms() - t0;
        if (sig1 > 0 && !s1 && el >= sig1) { kill(p, SIGINT); s1 = true; }
        if (sig2 > 0 && !s2 && el >= sig2) { kill(p, SIGINT); s2 = true; }
        if (el > 20000) { kill(p, SIGKILL); waitpid(p, &st, 0); *ms = el; return -1; }
        usleep(10000);
    }
    *ms = af_now_ms() - t0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
}
#define CLI(sub, ...) cli1(sub, (const char *[]){__VA_ARGS__}, sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *))
static int cli1(const char *sub, const char *const *extra, int nextra) { long ms; return cli(sub, extra, nextra, 0, 0, &ms); }

static void t_cli(void) {
    inizio("cli");
    if (access(CLI_, X_OK) != 0) { printf("salto i test CLI: manca %s\n", CLI_); return; }
    af_reset(&F);
    const char *valued[] = {"--size", "--seed", "--passi", "--n", "--cfg", "--negativo", "--rif", "--comfy-port", "--dir"};
    for (int i = 0; i < 9; i++) VERIFICA(CLI("img", "gatto", valued[i]) == 2, "B10 %s senza valore -> 2", valued[i]);
    VERIFICA(CLI("video", "--rif", "a", "x", "--sec") == 2, "B10 video --sec senza valore -> 2");
    VERIFICA(CLI("img", "--rif", "a", "--rif", "a", "--rif", "a", "--rif", "a", "--rif", "a", "--rif", "a", "--rif", "a",
                 "--rif", "a", "--rif", "a", "--rif", "a", "--rif", "a", "gatto") == 2, "B10 11 --rif -> 2");
    VERIFICA(CLI("img", "--") == 2, "B10 '--' senza descrizione -> 2");
    af_reset(&F);
    VERIFICA(CLI("img", "--", "--seed", "5") == 0 && strstr(F.graph, "\"--seed 5\""), "B10 dopo '--' anche le opzioni sono parole");
    af_reset(&F);
    VERIFICA(CLI("img", "--", "--", "x") == 0 && strstr(F.graph, "\"-- x\""), "B10 secondo '--' e' una parola");
    static char lungo[9000];
    memset(lungo, 'p', 8500); lungo[8500] = '\0';
    VERIFICA(CLI("img", lungo) == 2, "B10 descrizione oltre 8 KiB -> 2");
    lungo[5000] = '\0';
    int rc = CLI("img", lungo);
    VERIFICA(rc != 0, "descrizione di 5000 caratteri rifiutata (stato %d; 2 sarebbe l'errore d'uso)", rc);
    const char *badnum[][2] = {{"--seed", "1.5"}, {"--seed", ""}, {"--seed", "-2"}, {"--seed", "9223372036854775808"},
        {"--n", "0"}, {"--n", "5"}, {"--passi", "0"}, {"--passi", "61"}, {"--cfg", "20.0001"}, {"--cfg", "0"},
        {"--cfg", "nan"}, {"--cfg", "inf"}, {"--cfg", "1,5"}, {"--comfy-port", "0"}, {"--comfy-port", "70000"}, {"--size", "1024"}};
    for (int i = 0; i < 16; i++) {
        af_reset(&F);
        rc = CLI("img", "gatto", badnum[i][0], badnum[i][1]);
        VERIFICA(rc == 2, "B10 %s '%s' -> 2 (stato %d, grafo: %.40s)", badnum[i][0], badnum[i][1], rc,
                 F.graph[0] && strstr(F.graph, "\"cfg\"") ? strstr(F.graph, "\"cfg\"") : "-");
    }
    af_reset(&F);
    rc = CLI("img", "gatto", "--cfg", "0x10");
    VERIFICA(rc == 2, "B10 --cfg 0x10 (esadecimale) rifiutato (stato %d, %.20s)", rc, F.graph[0] && strstr(F.graph, "\"cfg\"") ? strstr(F.graph, "\"cfg\"") : "-");
    VERIFICA(CLI("free", "x") == 2 && CLI("boh", (const char *)0) == 2, "B10 parole spurie e comando ignoto -> 2");
    af_reset(&F);
    VERIFICA(CLI("img", "--negativo", "sfocato", "--trasparente", "gatto", "--seed", "-1") == 0 &&
             strstr(F.graph, "\"cfg\":2.500") && strstr(F.graph, "RGBA"), "B9/B10 --negativo -> cfg 2.5, --trasparente, --seed -1");
    af_reset(&F);
    VERIFICA(CLI("img", "--rif", "/nonesiste.png", "gatto") == 1, "--rif inesistente -> 1");
    VERIFICA(CLI("health") == 0 && CLI("free") == 0, "health/free contro il finto -> 0");

    /* B3: Ctrl+C annulla (coda + interrupt) ed esce 130 */
    af_reset(&F);
    F.ws_mode = AW_SILENT;
    F.hist_pending = -1;
    long ms;
    rc = cli("img", (const char *[]){"gatto"}, 1, 700, 0, &ms);
    VERIFICA(rc == 130 && ms < 4000 && F.queue_deletes == 1 && F.interrupts == 1 && strstr(F.last_delete, "advpid") && strstr(F.last_interrupt, "advpid"),
             "B3 primo SIGINT: annulla (delete %d, interrupt %d) ed esce 130 in %ld ms (stato %d)", F.queue_deletes, F.interrupts, ms, rc);
    F.ws_stop = 1;
    /* Ctrl+C mentre ComfyUI tarda a rispondere a /prompt: il primo dovrebbe gia' bastare */
    af_reset(&F);
    F.prompt_delay_ms = 4000;
    F.ws_mode = AW_NONE;
    rc = cli("img", (const char *[]){"gatto"}, 1, 500, 0, &ms);
    VERIFICA(rc == 130 && ms < 2500, "B3 SIGINT durante l'attesa di /prompt: esce presto (stato %d, %ld ms)", rc, ms);
    af_reset(&F);
    F.prompt_delay_ms = 4000;
    F.ws_mode = AW_NONE;
    rc = cli("img", (const char *[]){"gatto"}, 1, 500, 900, &ms);
    VERIFICA(rc == 130 && ms < 2000, "B3 secondo SIGINT esce subito (stato %d, %ld ms)", rc, ms);
}

int main(void) {
    if (!adv_setup()) return 1;
    t_limits();
    t_serve();
    t_cli();
    return adv_finish();
}
