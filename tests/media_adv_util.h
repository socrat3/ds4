/* Helper comuni ai test avversari di ds4_media (test_media_adv.c, test_media_adv_io.c):
 * VERIFICA, watchdog, cartella temporanea, lavori in un thread con limite di tempo.
 * Solo per i test: funzioni static in un header, alcune inutilizzate in ciascun file. */
#ifndef MEDIA_ADV_UTIL_H
#define MEDIA_ADV_UTIL_H
#pragma GCC diagnostic ignored "-Wunused-function"

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


/* Preparazione comune: stdout senza buffer, watchdog, finto ComfyUI, cartella. */
static bool adv_setup(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    signal(SIGPIPE, SIG_IGN);
    af_start(&F);
    snprintf(DIR_, sizeof(DIR_), "/tmp/ds4madvXXXXXX");
    if (!mkdtemp(DIR_)) { perror("mkdtemp"); return false; }
    if (!realpath("./ds4-media", CLI_)) snprintf(CLI_, sizeof(CLI_), "./ds4-media");
    return true;
}

static int adv_finish(void) {
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", DIR_);
    if (system(cmd) != 0) printf("pulizia non riuscita\n");
    printf("\n%d controlli superati, %d falliti\n", passati, falliti);
    return falliti != 0;
}

#endif
