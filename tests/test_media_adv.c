/* Test AVVERSARI di ds4_media (commit a33ef75): non confermano che funzioni, provano a
 * romperlo. Finto ComfyUI ostile (media_adv_fake.h): prompt_id malformati, history con
 * percorsi traversal, /view vuoti o troncati, websocket con frame illegali, ping flood,
 * serve con header/corpi/Host ostili, CLI con opzioni monche e Ctrl+C. Ogni controllo
 * nomina la pretesa (B#/O#) che attacca. Ogni test termina da solo (watchdog).
 *
 *   cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -std=c99 \
 *      -D_GNU_SOURCE -I. -o /tmp/adv tests/test_media_adv.c ds4_media.c ds4_media_job.c \
 *      ds4_media_ref.c ds4_media_video.c ds4_media_ws.c ds4_media_http.c ds4_media_serve.c -lpthread -lm
 *   (la parte CLI vuole ./ds4-media: make ds4-media) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../ds4_media.h"
#include "../ds4_media_int.h"
#include "media_adv_fake.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static int falliti, passati;
#define VERIFICA(c, ...) do { if (!(c)) { falliti++; printf("FALLITO: " __VA_ARGS__); printf("\n"); } \
                              else { passati++; printf("ok: %s\n", #c); } } while (0)

static adv_fake F;
static char DIR_[256], CLI_[4096];
static const char *g_test = "?";

static void watchdog(int s) { (void)s; printf("FALLITO: BLOCCO (hang) nel test %s\n", g_test); _exit(3); }
static void inizio(const char *nome) { g_test = nome; alarm(300); printf("-- %s\n", nome); }

static ds4_media *mk(const char *comfy_out) {
    ds4_media_config c = {.host = "127.0.0.1", .port = F.port, .media_dir = DIR_, .no_gate = true,
                          .comfy_output_dir = comfy_out};
    return ds4_media_create(&c);
}
static void set_pid(const char *pid) {
    snprintf(F.pid, sizeof(F.pid), "%s", pid);
    F.ws_raw_len = 0;
    af_done_event(&F);
}
static void write_bytes(const char *path, const void *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(b, 1, n, f); fclose(f); }
}
static void write_png(const char *path, int w, int h) {
    unsigned char p[64];
    af_png(p, w, h);
    write_bytes(path, p, sizeof(p));
}

/* Lavoro in un thread, con attesa limitata: un blocco e' un esito, non un'attesa infinita. */
typedef struct { ds4_media *m; ds4_media_image_req req; ds4_media_result res; bool ok, fine; char err[256]; long ms; } jrun;
static void *jrun_th(void *a) {
    jrun *j = a;
    long t0 = af_now_ms();
    j->ok = ds4_media_image(j->m, &j->req, &j->res, j->err, sizeof(j->err));
    j->ms = af_now_ms() - t0;
    j->fine = true;
    return NULL;
}
static bool jrun_go(jrun *j, int max_ms) {
    pthread_t th;
    j->fine = false;
    pthread_create(&th, NULL, jrun_th, j);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += max_ms / 1000;
    ts.tv_nsec += (max_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    if (pthread_timedjoin_np(th, NULL, &ts) == 0) return true;
    F.ws_stop = 1;                     /* sblocca il finto, poi aspetta il thread */
    pthread_join(th, NULL);
    return false;
}
static bool job(ds4_media *m, const char *prompt, jrun *j) {
    memset(j, 0, sizeof(*j));
    j->m = m;
    j->req.prompt = prompt;
    j->req.seed = 1;
    return jrun_go(j, 60000) && j->ok;
}

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
        " while True:\n  w,h=sc(rw,rh,res)\n  if ok(w,h) or res<=32: return res,w,h\n  res-=32\n"
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
        bool same = ok && strstr(F.graph, want) && j.res.width == ew && j.res.height == eh;
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    signal(SIGPIPE, SIG_IGN);
    af_start(&F);
    snprintf(DIR_, sizeof(DIR_), "/tmp/ds4madvXXXXXX");
    if (!mkdtemp(DIR_)) { perror("mkdtemp"); return 1; }
    if (!realpath("./ds4-media", CLI_)) snprintf(CLI_, sizeof(CLI_), "./ds4-media");
    bool lenti = argc > 1 && !strcmp(argv[1], "--lenti");
    t_pid();
    t_history();
    t_ws();
    t_dims_fuzz();
    t_edit_math();
    t_limits();
    t_serve();
    t_concurrency();
    t_cli();
    if (lenti) t_history_retry();
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DIR_);
    if (system(cmd) != 0) printf("pulizia non riuscita\n");
    printf("\n%d controlli superati, %d falliti\n", passati, falliti);
    return falliti != 0;
}
