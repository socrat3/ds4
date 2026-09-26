/* Test AVVERSARI di ds4_media, parte modulo: non confermano che funzioni, provano a
 * romperlo. Finto ComfyUI ostile (media_adv_fake.h): prompt_id malformati, history con
 * percorsi traversal, /view vuoti o troncati, websocket con frame illegali, ping flood,
 * intestazioni immagine casuali, matematica della modifica contro Python, concorrenza.
 * Serve e CLI sono in test_media_adv_io.c. Ogni controllo nomina la pretesa (B#/O#)
 * che attacca. Ogni test termina da solo (watchdog). --lenti aggiunge il controllo di
 * 30 s sull'attesa di /history dopo l'evento di fine.
 *
 *   make test-media-adv */
#include "media_adv_util.h"

/* ── prompt_id ostile ─────────────────────────────────────────────────────── */
static void t_pid(void) {
    inizio("prompt_id");
    ds4_media *m = mk(NULL);
    const char *bad[] = {"../x", "a\\\"b", "x y", "", "a/b", "1234567890123456789012345678901234567890123456789012345678901234"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        af_reset(&F);
        set_pid(bad[i]);
        jrun j;
        bool ok = job(m, "x", &j);
        VERIFICA(!ok && strstr(j.err, "prompt_id") && F.history_gets == 0, "prompt_id ostile [%s] rifiutato senza /history (%s)", bad[i], j.err);
        ds4_media_result_free(&j.res);
    }
    af_reset(&F);
    set_pid("123456789012345678901234567890123456789012345678901234567890abc");   /* 63: valido */
    jrun j;
    VERIFICA(job(m, "x", &j) && strlen(j.res.prompt_id) == 63, "prompt_id di 63 caratteri accettato: %s", j.err);
    ds4_media_result_free(&j.res);
    af_reset(&F);
    snprintf(F.prompt_body, sizeof(F.prompt_body), "{\"prompt_id\":12345}");
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "prompt_id"), "prompt_id numerico rifiutato: %s", j.err);
    ds4_media_free(m);
}

/* ── history e /view ostili ───────────────────────────────────────────────── */
static void hist(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(F.history, sizeof(F.history), fmt, ap);
    va_end(ap);
}
#define HIST_OK(imgs) "{\"advpid\":{\"outputs\":{\"8\":{\"images\":[" imgs "]}},\"status\":{\"status_str\":\"success\"}}}"
static bool dentro(const char *path) { return strncmp(path, DIR_, strlen(DIR_)) == 0 && !strstr(path, ".."); }

static void t_history(void) {
    inizio("history");
    char co[400], decoy[400], decoy2[400], legit[512];
    snprintf(co, sizeof(co), "%s/co/inner", DIR_);
    mkdir(co, 0755);
    char sub[420];
    snprintf(sub, sizeof(sub), "%s/ds4", co);
    char cod[300];
    snprintf(cod, sizeof(cod), "%s/co", DIR_);
    mkdir(cod, 0755); mkdir(co, 0755); mkdir(sub, 0755);
    snprintf(decoy, sizeof(decoy), "%s/decoy.png", DIR_);       /* = co/inner/../../decoy.png */
    snprintf(decoy2, sizeof(decoy2), "%s/co/decoy2.png", DIR_); /* = co/inner/../decoy2.png */
    snprintf(legit, sizeof(legit), "%s/legit.png", sub);
    write_png(decoy, 8, 8); write_png(decoy2, 8, 8); write_png(legit, 8, 8);
    ds4_media *m = mk(co);
    jrun j;

    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"legit.png\",\"subfolder\":\"ds4\",\"type\":\"output\"}"));
    VERIFICA(job(m, "x", &j) && access(legit, F_OK) != 0, "controllo: la copia legittima in comfy_output_dir viene tolta");
    VERIFICA(j.ok && j.res.n_files == 1 && dentro(j.res.files[0]), "uscita dentro media_dir (%s)", j.res.n_files ? j.res.files[0] : "-");
    ds4_media_result_free(&j.res);

    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"../../decoy.png\",\"subfolder\":\"ds4\",\"type\":\"output\"}"));
    job(m, "x", &j);
    VERIFICA(access(decoy, F_OK) == 0, "traversal nel filename: decoy fuori da comfy_output_dir intatto");
    VERIFICA(j.res.n_files == 0 || dentro(j.res.files[0]), "traversal nel filename: uscita locale dentro media_dir");
    ds4_media_result_free(&j.res);

    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"decoy.png\",\"subfolder\":\"../..\",\"type\":\"output\"}"));
    job(m, "x", &j);
    VERIFICA(access(decoy, F_OK) == 0, "subfolder ../..: decoy intatto");
    ds4_media_result_free(&j.res);
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"decoy2.png\",\"subfolder\":\"ds4/../..\",\"type\":\"output\"}"));
    job(m, "x", &j);
    VERIFICA(access(decoy2, F_OK) == 0, "subfolder ds4/../..: decoy2 intatto");
    ds4_media_result_free(&j.res);
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"decoy.png\",\"subfolder\":\"/%s\",\"type\":\"output\"}"), DIR_);
    job(m, "x", &j);
    VERIFICA(access(decoy, F_OK) == 0, "subfolder assoluto: decoy intatto");
    ds4_media_result_free(&j.res);

    /* estensione: solo alfanumerica corta, altrimenti .png */
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"x.png/../..\",\"subfolder\":\"ds4\",\"type\":\"output\"}"));
    VERIFICA(job(m, "x", &j) && strstr(j.res.files[0], ".png") == j.res.files[0] + strlen(j.res.files[0]) - 4 && dentro(j.res.files[0]),
             "ext 'png/../..' -> .png (%s)", j.ok ? j.res.files[0] : j.err);
    ds4_media_result_free(&j.res);
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"x.p;g\",\"subfolder\":\"\",\"type\":\"output\"}"));
    VERIFICA(job(m, "x", &j) && strstr(j.res.files[0], ".png") == j.res.files[0] + strlen(j.res.files[0]) - 4, "ext con ';' -> .png");
    ds4_media_result_free(&j.res);
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"x.html\",\"subfolder\":\"\",\"type\":\"output\"}"));
    job(m, "x", &j);
    printf("nota: filename x.html dal server -> file locale %s\n", j.ok ? j.res.files[0] : j.err);
    ds4_media_result_free(&j.res);

    /* campi mancanti e tipi */
    af_reset(&F);
    hist("{\"advpid\":{\"outputs\":{},\"status\":{\"status_str\":\"success\"}}}");
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "nessuna immagine"), "success senza outputs -> errore: %s", j.err);
    af_reset(&F);
    hist(HIST_OK("{\"filename\":\"t.png\",\"subfolder\":\"\",\"type\":\"temp\"}"));
    VERIFICA(!job(m, "x", &j), "solo uscite temp -> errore: %s", j.err);
    af_reset(&F);
    hist(HIST_OK("{\"subfolder\":\"ds4\",\"type\":\"output\"},{\"filename\":\"\"}"));
    VERIFICA(!job(m, "x", &j), "filename assente o vuoto -> errore: %s", j.err);
    af_reset(&F);
    hist("{\"advpid\":{\"status\":{\"status_str\":\"error\",\"messages\":[[\"execution_error\",{\"exception_message\":\"CUDA out of memory\"}]]}}}");
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "CUDA out of memory"), "errore con exception_message riportato: %s", j.err);
    af_reset(&F);
    hist("{\"advpid\":{\"status\":{\"status_str\":\"error\",\"messages\":[[\"execution_interrupted\",{}]]}}}");
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "interrotto"), "execution_interrupted riportato: %s", j.err);
    af_reset(&F);
    hist("{\"advpid\":{\"status\":{\"status_str\":\"error\"}}}");
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "errore di esecuzione"), "errore senza dettagli riportato: %s", j.err);

    /* /view: vuoto, troncato, 500, chunked history, filename lunghissimo */
    af_reset(&F);
    F.view_len = 0;
    job(m, "x", &j);
    VERIFICA(!j.ok, "B12/O1 /view a zero byte non e' un'immagine valida (ok=%d, file %s)", j.ok, j.res.n_files ? j.res.files[0] : "-");
    ds4_media_result_free(&j.res);
    af_reset(&F);
    F.view_cl = 5000;   /* dichiara 5000, manda 64 e chiude */
    job(m, "x", &j);
    VERIFICA(!j.ok, "scarico troncato (Content-Length 5000, 64 byte) rifiutato (ok=%d)", j.ok);
    ds4_media_result_free(&j.res);
    af_reset(&F);
    F.view_status = 500;
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "500"), "/view 500 -> errore: %s", j.err);
    af_reset(&F);
    F.hist_chunked = true;
    VERIFICA(job(m, "x", &j), "history chunked decodificata: %s", j.err);
    ds4_media_result_free(&j.res);
    af_reset(&F);
    F.view_len = 64; af_png(F.view_body, 2000, 1000);
    VERIFICA(job(m, "x", &j) && j.res.width == 2000 && j.res.height == 1000, "B1 dimensioni dal PNG scaricato (%dx%d)", j.res.width, j.res.height);
    ds4_media_result_free(&j.res);
    af_reset(&F);
    char longname[1300];
    memset(longname, 'a', 1200); memcpy(longname + 1200, ".png", 5);
    hist(HIST_OK("{\"filename\":\"%s\",\"subfolder\":\"ds4\",\"type\":\"output\"}"), longname);
    job(m, "x", &j);
    VERIFICA(F.view_malformed == 0, "GET /view con filename di 1200 caratteri e' una richiesta HTTP completa (troncata: %d, ok=%d, %s)", F.view_malformed, j.ok, j.err);
    ds4_media_result_free(&j.res);
    ds4_media_free(m);
}

/* ── websocket ostile ─────────────────────────────────────────────────────── */
static int g_prog, g_prog_bad;
static long g_cancel_at;
static bool cancel_at(void *pd) { (void)pd; return af_now_ms() >= g_cancel_at; }
static void on_prog(void *pd, int s, int t) { (void)pd; g_prog++; if (s < 0 || t <= 0 || s > t) g_prog_bad++; }

/* Esegue un lavoro con il ws programmato; deve riuscire (ripiegando su /history se
 * il ws e' rotto) entro max_ms. */
static void ws_case(ds4_media *m, const char *nome, int max_ms) {
    jrun j;
    memset(&j, 0, sizeof(j));
    j.m = m; j.req.prompt = nome; j.req.seed = 1;
    bool fine = jrun_go(&j, max_ms);
    VERIFICA(fine && j.ok, "O1 ws %s: riesce (ripiego) entro %d ms (fine=%d ok=%d %ld ms: %s)", nome, max_ms, fine, j.ok, j.ms, j.err);
    ds4_media_result_free(&j.res);
}

static void t_ws(void) {
    inizio("websocket");
    ds4_media_config c = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_, .no_gate = true, .progress = on_prog};
    ds4_media *m = ds4_media_create(&c);
    const char ev[] = "{\"type\":\"executing\",\"data\":{\"node\":null,\"prompt_id\":\"advpid\"}}";
    unsigned char big[200];
    memset(big, 'p', sizeof(big));

    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, ev, strlen(ev), false, 0, 4);      /* RSV1 acceso */
    ws_case(m, "RSV", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, ev, strlen(ev), true, 0, 0);       /* mascherato */
    ws_case(m, "mascherato", 3000);
    VERIFICA(F.history_gets <= 2, "ws mascherato smascherato: evento visto, niente polling (%d GET)", F.history_gets);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, ev, strlen(ev), false, 8, 0);      /* lunghezza 64 bit */
    ws_case(m, "len64", 3000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, ev, strlen(ev), false, 2, 0);      /* lunghezza 16 bit */
    ws_case(m, "len16", 3000);
    af_reset(&F); F.ws_raw_len = 0;
    {   /* header che dichiara 16 MiB + 1 */
        unsigned char h[10] = {0x81, 127, 0, 0, 0, 0, 0x01, 0, 0, 1};
        memcpy(F.ws_raw, h, 10); F.ws_raw_len = 10;
    }
    ws_case(m, "oltre16MiB", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 9, true, big, 126, false, 2, 0);            /* ping di 126 byte */
    af_done_event(&F);
    ws_case(m, "ping126", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 9, false, "hi", 2, false, 0, 0);            /* ping frammentato */
    af_done_event(&F);
    ws_case(m, "pingfrag", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 0, true, ev, strlen(ev), false, 0, 0);      /* continuazione orfana */
    ws_case(m, "cont-orfana", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, false, ev, 10, false, 0, 0);
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, ev, strlen(ev), false, 0, 0);      /* nuovo testo dentro un frammentato */
    ws_case(m, "testo-in-frammento", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 3, true, "x", 1, false, 0, 0);              /* opcode riservato */
    ws_case(m, "opcode3", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 8, true, "\x03\xe8", 2, false, 0, 0);       /* close */
    F.ws_after = AW_AFTER_CLOSE;
    ws_case(m, "close", 8000);
    af_reset(&F); F.ws_raw_len = 0;
    af_frame(F.ws_raw, &F.ws_raw_len, 1, true, "", 0, false, 0, 0);               /* testo vuoto */
    af_frame(F.ws_raw, &F.ws_raw_len, 2, true, big, 200, false, 8, 0);            /* binario con len 64 bit non minima */
    af_done_event(&F);
    ws_case(m, "vuoto+bin64", 3000);
    af_reset(&F); F.ws_raw_len = 0;                                               /* byte a byte, frammentato, ping in mezzo */
    af_frame(F.ws_raw, &F.ws_raw_len, 1, false, ev, 20, false, 0, 0);
    af_frame(F.ws_raw, &F.ws_raw_len, 9, true, "hi", 2, false, 0, 0);
    af_frame(F.ws_raw, &F.ws_raw_len, 0, false, ev + 20, 20, false, 0, 0);
    af_frame(F.ws_raw, &F.ws_raw_len, 0, true, ev + 40, strlen(ev) - 40, false, 0, 0);
    F.ws_chunk = 1; F.ws_chunk_us = 300;
    ws_case(m, "byte-a-byte", 3000);
    VERIFICA(F.got_pong == 1 && F.history_gets <= 2, "frammenti byte a byte + ping: pong %d, GET %d", F.got_pong, F.history_gets);
    af_reset(&F);
    snprintf(F.ws_handshake, sizeof(F.ws_handshake), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    ws_case(m, "handshake200", 8000);
    af_reset(&F);
    snprintf(F.ws_handshake, sizeof(F.ws_handshake), "XYZ garbage\r\n\r\n");
    ws_case(m, "handshake-garbage", 8000);
    af_reset(&F);
    snprintf(F.ws_handshake, sizeof(F.ws_handshake), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n");
    F.ws_glue = true;                                                              /* frame incollati all'handshake */
    ws_case(m, "frame-incollati", 3000);
    VERIFICA(F.history_gets <= 2, "frame incollati all'handshake letti (%d GET)", F.history_gets);
    af_reset(&F); F.ws_raw_len = 0;                                               /* solo eventi di altri lavori */
    af_event(&F, "executing", "altro", "\"node\":null");
    af_event(&F, "execution_success", "altro2", NULL);
    af_event(&F, "progress", "altro", "\"value\":1,\"max\":2");
    g_prog = 0;
    ws_case(m, "altri-pid", 9000);
    VERIFICA(g_prog == 0, "progresso di altri lavori ignorato (%d)", g_prog);
    af_reset(&F); F.ws_raw_len = 0;                                               /* progresso assurdo */
    af_event(&F, "progress", NULL, "\"value\":-5,\"max\":10");
    af_event(&F, "progress", NULL, "\"value\":11,\"max\":10");
    af_event(&F, "progress", NULL, "\"value\":1,\"max\":0");
    af_event(&F, "progress", NULL, "\"value\":1,\"max\":99999999999999999999");
    af_event(&F, "progress", NULL, "\"value\":99999999999999999999,\"max\":5");
    af_event(&F, "progress", NULL, "\"value\":\"x\",\"max\":5");
    af_event(&F, "progress", NULL, "\"value\":2,\"max\":5");
    af_done_event(&F);
    g_prog = g_prog_bad = 0;
    ws_case(m, "progresso-assurdo", 3000);
    VERIFICA(g_prog == 1 && g_prog_bad == 0, "solo il progresso sano arriva alla callback (%d, anomali %d)", g_prog, g_prog_bad);
    af_reset(&F); F.ws_raw_len = 0;
    af_event(&F, "execution_error", NULL, NULL);
    hist("{\"advpid\":{\"status\":{\"status_str\":\"error\",\"messages\":[[\"execution_error\",{\"exception_message\":\"boom\"}]]}}}");
    jrun j;
    VERIFICA(!job(m, "x", &j) && strstr(j.err, "boom"), "O1 execution_error via ws poi /history e' la verita': %s", j.err);

    /* ping flood: il server non legge mai i pong */
    af_reset(&F);
    F.ws_mode = AW_PINGFLOOD;
    F.hist_pending = -1;
    memset(&j, 0, sizeof(j));
    j.m = m; j.req.prompt = "flood"; j.req.seed = 1; j.req.cancel = cancel_at;
    g_cancel_at = af_now_ms() + 1500;
    long t0 = af_now_ms();
    bool fine = jrun_go(&j, 6000);
    F.ws_stop = 1;
    printf("nota: ping flood, lavoro finito=%d in %ld ms, pong contati %d\n", fine, af_now_ms() - t0, F.got_pong);
    VERIFICA(fine && j.ms < 4000, "B3 ping flood senza lettura dei pong: l'annullamento a 1,5 s arriva (finito=%d, %ld ms)", fine, af_now_ms() - t0);
    VERIFICA(!j.ok && strstr(j.err, "annullato"), "ping flood: esito 'annullato', non un finto successo (%s)", j.err);
    ds4_media_result_free(&j.res);
    ds4_media_free(m);
}

/* O1: dopo l'evento di fine, /history si riprova per ~30 s e poi si rinuncia. */
static void t_history_retry(void) {
    inizio("history-retry-30s");
    af_reset(&F);
    F.hist_pending = -1;
    ds4_media *m = mk(NULL);
    jrun j;
    long t0 = af_now_ms();
    job(m, "x", &j);
    long ms = af_now_ms() - t0;
    VERIFICA(!j.ok && strstr(j.err, "/history") && ms >= 29000 && ms <= 45000, "O1 rinuncia dopo ~30 s (%ld ms): %s", ms, j.err);
    ds4_media_free(m);
}

/* ── intestazioni immagine: fuzz ──────────────────────────────────────────── */
static void t_dims_fuzz(void) {
    inizio("dims-fuzz");
    static const unsigned char pre[4][16] = {
        {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'},
        {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0, 0xFF, 0xC0, 0, 0x11, 8, 3, 0, 4},
        {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '},
        {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X'}};
    const char *v8[] = {"VP8 ", "VP8L", "VP8X", "ALPH"};
    unsigned s = 12345;
    long riconosciute = 0;
    for (int it = 0; it < 60000; it++) {
        s = s * 1103515245u + 12345u;
        size_t n = (s >> 8) % 96;
        unsigned char *b = malloc(n ? n : 1);
        for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; b[i] = (unsigned char)(s >> 16); }
        int kind = (int)((s >> 4) % 6);
        if (kind < 4 && n) memcpy(b, pre[kind], n < 16 ? n : 16);
        if (kind >= 2 && kind < 4 && n >= 16) memcpy(b + 12, v8[(s >> 20) % 4], 4);
        if (kind == 1 && n > 2) for (size_t i = 2; i < n; i += 4) b[i] = 0xFF;   /* tanti marcatori */
        int w = 0, h = 0;
        if (media_image_dims(b, n, &w, &h)) { riconosciute++; if (w <= 0 || h <= 0 || w > (1 << 20) || h > (1 << 20)) falliti++; }
        free(b);
    }
    VERIFICA(riconosciute > 0, "fuzz intestazioni: nessun accesso fuori dai limiti (ASan), %ld riconosciute", riconosciute);
    /* JPEG con SOF proprio alla fine, e con lunghezza segmento 0xFFFF */
    unsigned char j1[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x03, 0x00, 0x04};
    unsigned char j2[] = {0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 0x00};
    unsigned char j3[] = {0xFF, 0xD8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    int w, h;
    VERIFICA(!media_image_dims(j1, sizeof(j1), &w, &h) && !media_image_dims(j2, sizeof(j2), &w, &h) &&
             !media_image_dims(j3, sizeof(j3), &w, &h), "JPEG troncati rifiutati senza leggere oltre");
    /* file: cartella, inesistente, 1 MiB di zeri */
    VERIFICA(!media_file_dims(DIR_, &w, &h) && !media_file_dims("/nonesiste", &w, &h), "media_file_dims su cartella/inesistente");
}

/* ── matematica della modifica (B1) contro Python ─────────────────────────── */
static void t_edit_math(void) {
    inizio("edit-math");
    static const int pairs[][2] = {{1296, 256}, {1936, 256}, {700, 448}, {784, 576}, {2704, 256}, {3600, 256},
        {2000, 320}, {1008, 448}, {3042, 128}, {3888, 192}, {1, 8000}, {8000, 1}, {1, 1}, {100, 4000}, {5120, 512},
        {2752, 1536}, {3000, 3000}, {7000, 300}, {4096, 4096}, {640, 480}, {1920, 1080}, {1080, 1920}, {333, 777},
        {1023, 1025}, {1536, 1024}, {512, 512}, {31, 33}, {2048, 8192}, {8192, 2048}, {1, 7397}};
    int np = (int)(sizeof(pairs) / sizeof(pairs[0]));
    char script[4096];
    int sl = snprintf(script, sizeof(script),
        "python3 -c \"import math\n"
        "def sc(rw,rh,res):\n ratio=rw/rh\n w=round(math.sqrt(res*res*ratio)/32)*32\n h=round(math.sqrt(res*res/ratio)/32)*32\n return max(32,w),max(32,h)\n"
        "def ok(w,h): return w%%32==0 and h%%32==0 and w<=2752 and h<=2752 and w*h<=2752*1536\n"
        "def plan(rw,rh):\n res=max(512,round(math.sqrt(rw*rh)/32)*32)\n"
        " while True:\n  w,h=sc(rw,rh,res)\n  if ok(w,h): return res,w,h\n  if res<=32: return 0,0,0\n  res-=32\n"
        "for rw,rh in [");
    for (int i = 0; i < np; i++) sl += snprintf(script + sl, sizeof(script) - (size_t)sl, "(%d,%d),", pairs[i][0], pairs[i][1]);
    snprintf(script + sl, sizeof(script) - (size_t)sl, "]: print(*plan(rw,rh))\" 2>/dev/null");
    FILE *py = popen(script, "r");
    if (!py) { printf("salto edit-math: niente python3\n"); return; }
    ds4_media *m = mk(NULL);
    char ref[400];
    snprintf(ref, sizeof(ref), "%s/ref.png", DIR_);
    const char *refs[] = {ref};
    int viol = 0;
    for (int i = 0; i < np; i++) {
        int er, ew, eh;
        if (fscanf(py, "%d %d %d", &er, &ew, &eh) != 3) { printf("python3 non risponde\n"); break; }
        write_png(ref, pairs[i][0], pairs[i][1]);
        af_reset(&F);
        memset(F.view_body, 'z', 64);   /* /view non-PNG: width/height del risultato = piano del modulo */
        jrun j;
        memset(&j, 0, sizeof(j));
        j.m = m; j.req.prompt = "modifica"; j.req.seed = 1; j.req.refs = refs; j.req.n_refs = 1;
        bool ok = jrun_go(&j, 20000) && j.ok;
        char want[64];
        snprintf(want, sizeof(want), "\"resolution\":%d,", er);
        /* er == 0: nessuna risoluzione rientra nei limiti, il modulo deve rifiutare (ADV-3) */
        bool same = er == 0 ? (!ok && j.res.invalid && strstr(j.err, "estreme"))
                            : (ok && strstr(F.graph, want) && j.res.width == ew && j.res.height == eh);
        VERIFICA(same, "B1 %dx%d: atteso res %d -> %dx%d, modulo %s %dx%d (%s)", pairs[i][0], pairs[i][1], er, ew, eh,
                 strstr(F.graph, "\"resolution\":") ? strstr(F.graph, "\"resolution\":") + 13 : "?", j.res.width, j.res.height, j.err);
        if (ok && (j.res.width > DS4_MEDIA_MAX_SIDE || j.res.height > DS4_MEDIA_MAX_SIDE ||
                   (long)j.res.width * j.res.height > DS4_MEDIA_MAX_AREA)) {
            viol++;
            printf("nota: riferimento %dx%d -> uscita %dx%d fuori dai limiti\n", pairs[i][0], pairs[i][1], j.res.width, j.res.height);
        }
        if (ok) VERIFICA(strstr(F.graph, "\"latent_image\":[\"4\",2]") != NULL, "B1 %dx%d campiona su [4,2]", pairs[i][0], pairs[i][1]);
        ds4_media_result_free(&j.res);
    }
    pclose(py);
    VERIFICA(viol == 0, "B5 ogni modifica automatica resta nei limiti (lato 2752, area 2752x1536): %d violazioni", viol);
    /* riferimento illeggibile: 1024 */
    write_bytes(ref, "nonimmagine", 11);
    af_reset(&F);
    jrun j;
    memset(&j, 0, sizeof(j));
    j.m = m; j.req.prompt = "modifica"; j.req.seed = 1; j.req.refs = refs; j.req.n_refs = 1;
    VERIFICA(jrun_go(&j, 20000) && j.ok && strstr(F.graph, "\"resolution\":1024,"), "B1 riferimento illeggibile -> resolution 1024");
    ds4_media_result_free(&j.res);
    ds4_media_free(m);
}

/* ── limiti, cfg, gate ────────────────────────────────────────────────────── */
/* ── concorrenza sul modulo ───────────────────────────────────────────────── */
static void t_concurrency(void) {
    inizio("concorrenza");
    af_reset(&F);
    ds4_media *m = mk(NULL);
    jrun j[8];
    pthread_t th[8];
    for (int i = 0; i < 8; i++) { memset(&j[i], 0, sizeof(j[i])); j[i].m = m; j[i].req.prompt = "insieme"; j[i].req.seed = i; pthread_create(&th[i], NULL, jrun_th, &j[i]); }
    int ok = 0, distinti = 1;
    for (int i = 0; i < 8; i++) { pthread_join(th[i], NULL); ok += j[i].ok; }
    for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++)
        if (j[a].ok && j[b].ok && !strcmp(j[a].res.files[0], j[b].res.files[0])) distinti = 0;
    VERIFICA(ok == 8 && distinti && F.prompts == 8, "8 thread sulla stessa istanza: tutti ok, file distinti (%d ok, %d prompt)", ok, F.prompts);
    for (int i = 0; i < 8; i++) ds4_media_result_free(&j[i].res);
    ds4_media_free(m);
}


int main(int argc, char **argv) {
    if (!adv_setup()) return 1;
    bool lenti = argc > 1 && !strcmp(argv[1], "--lenti");
    t_pid();
    t_history();
    t_ws();
    t_dims_fuzz();
    t_edit_math();
    t_concurrency();
    if (lenti) t_history_retry();
    return adv_finish();
}
