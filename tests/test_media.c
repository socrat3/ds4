/* Test di ds4_media senza GPU: un finto ComfyUI in un thread risponde a
 * /prompt, /history, /view, /upload/image, /system_stats, /free; il modulo genera
 * un'immagine e si verifica che il file scritto contenga i byte del finto /view.
 * Piu' controlli sugli helper JSON, sul gate di memoria e sull'annullamento.
 *
 *   make test-media   (oppure: cc -I.. test_media.c ../ds4_media.c ../ds4_media_http.c -lpthread) */
#include "../ds4_media.h"
#include "../ds4_media_http.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int falliti;
#define VERIFICA(c, ...) do { if (!(c)) { falliti++; printf("FALLITO: " __VA_ARGS__); printf("\n"); } \
                              else printf("ok: %s\n", #c); } while (0)

static const char PNG_MAGIC[8] = {(char)0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

/* ── finto ComfyUI ────────────────────────────────────────────────────────── */

typedef struct { int port; int uploads; int frees; int prompts_bf16; int prompts; } fake_state;

static void send_response(int fd, const char *status, const char *ctype,
                          const void *body, size_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, ctype, blen);
    if (write(fd, hdr, (size_t)n) < 0) { close(fd); return; }
    if (blen && write(fd, body, blen) < 0) return;
}

static void *fake_comfy(void *arg) {
    fake_state *st = arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return NULL; }
    socklen_t al = sizeof(a);
    getsockname(srv, (struct sockaddr *)&a, &al);
    st->port = ntohs(a.sin_port);
    listen(srv, 8);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) break;
        char req[8192];
        ssize_t r = read(fd, req, sizeof(req) - 1);
        if (r <= 0) { close(fd); continue; }
        req[r] = '\0';
        if (strncmp(req, "STOP", 4) == 0) { close(fd); break; }

        if (strstr(req, "POST /prompt")) {
            st->prompts++;
            if (strstr(req, "qwen_image_2.1_bf16")) st->prompts_bf16++;
            const char *b = strstr(req, "MiniMaxH3ImageToVideo")
                ? "{\"prompt_id\":\"vidpid\",\"number\":1,\"node_errors\":{}}"
                : "{\"prompt_id\":\"testpid\",\"number\":1,\"node_errors\":{}}";
            send_response(fd, "200 OK", "application/json", b, strlen(b));
        } else if (strstr(req, "GET /history/vidpid")) {
            const char *b = "{\"vidpid\":{\"outputs\":{\"92\":{\"images\":[{\"filename\":\"ds4_clip.mp4\","
                            "\"subfolder\":\"ds4\",\"type\":\"output\"}]}},"
                            "\"status\":{\"status_str\":\"success\",\"completed\":true}}}";
            send_response(fd, "200 OK", "application/json", b, strlen(b));
        } else if (strstr(req, "GET /history/testpid")) {
            const char *b = "{\"testpid\":{\"outputs\":{\"8\":{\"images\":[{\"filename\":\"ds4_out_001_.png\","
                            "\"subfolder\":\"ds4\",\"type\":\"output\"}]}},"
                            "\"status\":{\"status_str\":\"success\",\"completed\":true}}}";
            send_response(fd, "200 OK", "application/json", b, strlen(b));
        } else if (strstr(req, "GET /view")) {
            VERIFICA(strstr(req, "subfolder=ds4") != NULL, "view passa il subfolder");
            send_response(fd, "200 OK", "application/octet-stream", PNG_MAGIC, sizeof(PNG_MAGIC));
        } else if (strstr(req, "POST /upload/image")) {
            st->uploads++;
            VERIFICA(strstr(req, "multipart/form-data") != NULL, "upload e' multipart");
            VERIFICA(strstr(req, "name=\"image\"") != NULL, "upload ha il campo image");
            const char *b = "{\"name\":\"rif.png\",\"subfolder\":\"ds4\",\"type\":\"input\"}";
            send_response(fd, "200 OK", "application/json", b, strlen(b));
        } else if (strstr(req, "POST /free")) {
            st->frees++;
            send_response(fd, "200 OK", "application/json", "{}", 2);
        } else if (strstr(req, "GET /system_stats")) {
            send_response(fd, "200 OK", "application/json", "{\"system\":{}}", 13);
        } else {
            send_response(fd, "404 Not Found", "text/plain", "no", 2);
        }
        close(fd);
    }
    close(srv);
    return NULL;
}

static void stop_fake(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) { if (write(fd, "STOP", 4) < 0) {} }
    close(fd);
}

/* ── helper e grafo ───────────────────────────────────────────────────────── */

static void test_helpers(void) {
    char *s = media_json_str("{\"a\":\"x\",\"prompt_id\":\"abc\\n\"}", "prompt_id");
    VERIFICA(s && strcmp(s, "abc\n") == 0, "media_json_str decodifica \\n");
    free(s);
    bool found = false;
    long v = media_json_int("{\"steps\": 25, \"seed\":-1}", "seed", &found);
    VERIFICA(found && v == -1, "media_json_int legge un negativo");
    char *q = media_json_quote("a\"b\nc");
    VERIFICA(q && strcmp(q, "\"a\\\"b\\nc\"") == 0, "media_json_quote cita");
    free(q);
    char *e = media_url_encode("a b/c");
    VERIFICA(e && strcmp(e, "a%20b%2Fc") == 0, "media_url_encode");
    free(e);

    /* \uXXXX -> UTF-8: come json.dumps di Python (ensure_ascii) manda accenti ed emoji */
    char *u = media_json_str("{\"prompt\":\"citt\\u00e0 \\ud83c\\udf19 \\u20ac\"}", "prompt");
    VERIFICA(u && strcmp(u, "citt\xc3\xa0 \xf0\x9f\x8c\x99 \xe2\x82\xac") == 0,
             "media_json_str decodifica \\u in UTF-8 (accenti, coppia surrogata, 3 byte): [%s]", u ? u : "(null)");
    free(u);
    /* \u troncata o malformata: si ferma senza leggere oltre il terminatore */
    char *tr = strdup("{\"prompt\":\"abc\\u");
    char *t = media_json_str(tr, "prompt");
    VERIFICA(t && strcmp(t, "abc") == 0, "\\u troncata chiude la stringa: [%s]", t ? t : "(null)");
    free(t); free(tr);
    char *t2 = media_json_str("{\"prompt\":\"ab\\u12x4cd\"}", "prompt");
    VERIFICA(t2 && strcmp(t2, "ab") == 0, "\\u malformata chiude la stringa: [%s]", t2 ? t2 : "(null)");
    free(t2);
    char *t3 = media_json_str("{\"prompt\":\"x\\ud83cy\"}", "prompt");
    VERIFICA(t3 && strcmp(t3, "x\xef\xbf\xbdy") == 0, "surrogato spaiato -> U+FFFD: [%s]", t3 ? t3 : "(null)");
    free(t3);
}

/* ── lavori concorrenti ───────────────────────────────────────────────────── */

typedef struct { ds4_media *m; ds4_media_result res; bool ok; char err[256]; } job_arg;
static void *job_thread(void *a) {
    job_arg *j = a;
    ds4_media_image_req req = {.prompt = "due lavori insieme", .seed = 3};
    j->ok = ds4_media_image(j->m, &req, &j->res, j->err, sizeof(j->err));
    return NULL;
}

/* Due thread chiedono un'immagine nello stesso istante: il mutex li serializza e i
 * file di uscita restano distinti (prompt_id nel nome). */
static void test_concorrenza(ds4_media *m) {
    job_arg a = {.m = m}, b = {.m = m};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, job_thread, &a);
    pthread_create(&tb, NULL, job_thread, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    VERIFICA(a.ok && b.ok, "entrambi i lavori riescono (%s / %s)", a.err, b.err);
    if (a.ok && b.ok && a.res.n_files == 1 && b.res.n_files == 1) {
        VERIFICA(strcmp(a.res.files[0], b.res.files[0]) != 0, "i due file hanno nomi diversi (%s)", a.res.files[0]);
        VERIFICA(strstr(a.res.files[0], "-testpid-") != NULL, "il nome contiene il prompt_id (%s)", a.res.files[0]);
        VERIFICA(access(a.res.files[0], R_OK) == 0 && access(b.res.files[0], R_OK) == 0, "entrambi i file esistono");
    }
    ds4_media_result_free(&a.res);
    ds4_media_result_free(&b.res);
}

/* ── server OpenAI ────────────────────────────────────────────────────────── */

typedef struct { ds4_media *m; int port; volatile int stop; } serve_arg;
static void *serve_thread(void *a) {
    serve_arg *s = a;
    char err[128] = {0};
    ds4_media_serve(s->m, s->port, &s->stop, err, sizeof(err));
    return NULL;
}

/* Avvia il server su una porta di test, gli manda una richiesta e controlla la risposta.
 * Con st: verifica che senza "weights" nel corpo il serve usi i pesi della config. */
static void test_serve(ds4_media *m, int port, fake_state *st, bool expect_bf16) {
    int bf16_before = st->prompts_bf16, prompts_before = st->prompts;
    serve_arg sa = {.m = m, .port = port, .stop = 0};
    pthread_t th;
    pthread_create(&th, NULL, serve_thread, &sa);
    usleep(200000);   /* lascia fare bind/listen */

    media_http_response resp = {0};
    char err[128] = {0};
    const char *body = "{\"prompt\":\"una rosa\",\"n\":1,\"size\":\"512x512\",\"response_format\":\"b64_json\"}";
    bool ok = media_http_post("127.0.0.1", port, "/v1/images/generations",
                              "application/json", body, strlen(body), 20000, &resp, err, sizeof(err));
    VERIFICA(ok, "serve: server raggiungibile sulla porta %d: %s", port, err);
    if (ok) {
        VERIFICA(resp.status == 200, "serve: 200 su /v1/images/generations (%d)", resp.status);
        VERIFICA(resp.body && strstr(resp.body, "\"b64_json\""), "serve: risposta con b64_json");
        VERIFICA(st->prompts == prompts_before + 1, "serve: un grafo inviato a ComfyUI");
        VERIFICA((st->prompts_bf16 - bf16_before) == (expect_bf16 ? 1 : 0),
                 "serve: pesi %s dalla config quando il corpo non li indica", expect_bf16 ? "bf16" : "int8");
    }
    media_http_response_free(&resp);
    sa.stop = 1;
    pthread_join(th, NULL);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    test_helpers();

    fake_state st = {0};
    pthread_t th;
    pthread_create(&th, NULL, fake_comfy, &st);
    for (int i = 0; i < 200 && st.port == 0; i++) usleep(1000);
    VERIFICA(st.port > 0, "finto ComfyUI in ascolto (porta %d)", st.port);

    char tmpl[] = "/tmp/ds4mediaXXXXXX";
    char *dir = mkdtemp(tmpl);
    VERIFICA(dir != NULL, "cartella temporanea");

    char err[256] = {0};
    ds4_media_config cfg = {.host = "127.0.0.1", .port = st.port, .media_dir = dir,
                            .weights = DS4_MEDIA_INT8, .no_gate = true};
    ds4_media *m = ds4_media_create(&cfg);
    VERIFICA(m != NULL, "ds4_media_create");

    VERIFICA(ds4_media_health(m, err, sizeof(err)), "health: %s", err);

    ds4_media_image_req req = {.prompt = "una rosa rossa e blu", .seed = 7};
    ds4_media_result res = {0};
    bool ok = ds4_media_image(m, &req, &res, err, sizeof(err));
    VERIFICA(ok, "genera immagine: %s", err);
    if (ok) {
        VERIFICA(res.n_files == 1, "un file prodotto (%d)", res.n_files);
        VERIFICA(res.seed == 7, "seed conservato");
        VERIFICA(strcmp(res.prompt_id, "testpid") == 0, "prompt_id");
        if (res.n_files == 1) {
            FILE *f = fopen(res.files[0], "rb");
            char buf[8] = {0};
            size_t rd = f ? fread(buf, 1, 8, f) : 0;
            if (f) fclose(f);
            VERIFICA(rd == 8 && memcmp(buf, PNG_MAGIC, 8) == 0, "il PNG contiene i byte del finto /view");
        }
    }
    ds4_media_result_free(&res);

    /* modifica con un riferimento: verifica l'upload */
    char refpath[1024];
    snprintf(refpath, sizeof(refpath), "%s/soggetto.png", dir);
    FILE *rf = fopen(refpath, "wb");
    fwrite(PNG_MAGIC, 1, 8, rf);
    fclose(rf);
    const char *refs[] = {refpath};
    ds4_media_image_req ereq = {.prompt = "metti a <image1> un cappello", .seed = 1,
                                .refs = refs, .n_refs = 1};
    ds4_media_result eres = {0};
    ok = ds4_media_image(m, &ereq, &eres, err, sizeof(err));
    VERIFICA(ok, "modifica con riferimento: %s", err);
    VERIFICA(st.uploads == 1, "un riferimento caricato (%d)", st.uploads);
    ds4_media_result_free(&eres);

    VERIFICA(ds4_media_free_models(m, err, sizeof(err)) && st.frees == 1, "free_models");

    /* gate: 2048 bf16 con soli 5 GiB liberi deve rifiutare */
    ds4_media_config gcfg = cfg;
    gcfg.no_gate = false;
    gcfg.avail_override_kib = 5L * 1024 * 1024;
    ds4_media *mg = ds4_media_create(&gcfg);
    ds4_media_image_req breq = {.prompt = "x", .width = 1024, .height = 1024, .weights = DS4_MEDIA_BF16};
    ds4_media_result bres = {0};
    ok = ds4_media_image(mg, &breq, &bres, err, sizeof(err));
    VERIFICA(!ok && strstr(err, "GiB"), "il gate rifiuta senza memoria: %s", err);
    ds4_media_result_free(&bres);
    ds4_media_free(mg);

    /* dimensioni non multiple di 32 */
    ds4_media_image_req wreq = {.prompt = "x", .width = 1000, .height = 1024};
    ds4_media_result wres = {0};
    ok = ds4_media_image(m, &wreq, &wres, err, sizeof(err));
    VERIFICA(!ok && strstr(err, "32"), "rifiuta dimensioni non multiple di 32");
    ds4_media_result_free(&wres);

    /* video H3: serve un riferimento, produce un .mp4 */
    char refv[1024];
    snprintf(refv, sizeof(refv), "%s/ref.png", dir);
    FILE *vf = fopen(refv, "wb");
    fwrite(PNG_MAGIC, 1, 8, vf);
    fclose(vf);
    ds4_media_video_req vreq = {.prompt = "un gatto che si muove", .ref = refv, .seconds = 5};
    ds4_media_result vres = {0};
    ok = ds4_media_video(m, &vreq, &vres, err, sizeof(err));
    VERIFICA(ok, "genera video: %s", err);
    if (ok) {
        VERIFICA(vres.n_files == 1, "un file video prodotto (%d)", vres.n_files);
        VERIFICA(strstr(vres.files[0], ".mp4") != NULL, "il file e' un .mp4 (%s)",
                 vres.n_files ? vres.files[0] : "-");
        VERIFICA(strcmp(vres.prompt_id, "vidpid") == 0, "prompt_id del video");
    }
    ds4_media_result_free(&vres);

    /* video senza riferimento -> rifiutato */
    ds4_media_video_req vbad = {.prompt = "x"};
    ds4_media_result vbadr = {0};
    ok = ds4_media_video(m, &vbad, &vbadr, err, sizeof(err));
    VERIFICA(!ok && strstr(err, "riferimento"), "video senza --rif rifiutato");
    ds4_media_result_free(&vbadr);

    /* due lavori concorrenti sulla stessa istanza */
    test_concorrenza(m);

    /* free automatico dopo inattivita': idle_free_sec=1, un lavoro, poi il tick */
    ds4_media_config icfg = cfg;
    icfg.idle_free_sec = 1;
    ds4_media *mi = ds4_media_create(&icfg);
    VERIFICA(!ds4_media_idle_tick(mi), "idle: niente da liberare senza lavori");
    ds4_media_image_req ireq = {.prompt = "x", .seed = 1};
    ds4_media_result ires = {0};
    VERIFICA(ds4_media_image(mi, &ireq, &ires, err, sizeof(err)), "idle: lavoro: %s", err);
    ds4_media_result_free(&ires);
    int frees_before = st.frees;
    VERIFICA(!ds4_media_idle_tick(mi), "idle: subito dopo il lavoro non libera");
    sleep(2);
    VERIFICA(ds4_media_idle_tick(mi) && st.frees == frees_before + 1, "idle: dopo 2 s libera (frees %d)", st.frees);
    VERIFICA(!ds4_media_idle_tick(mi), "idle: non libera due volte");
    ds4_media_free(mi);

    /* server OpenAI: avvia ds4_media_serve in un thread e chiama /v1/images/generations;
     * poi la stessa cosa con una config bf16 per verificare che serve la rispetti */
    test_serve(m, 19010, &st, false);
    ds4_media_config bcfg = cfg;
    bcfg.weights = DS4_MEDIA_BF16;
    ds4_media *mb = ds4_media_create(&bcfg);
    test_serve(mb, 19011, &st, true);
    ds4_media_free(mb);

    ds4_media_free(m);
    stop_fake(st.port);
    pthread_join(th, NULL);

    /* pulizia */
    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("pulizia di %s non riuscita\n", dir);

    printf(falliti ? "\n%d FALLITI\n" : "\ntutti i test superati\n", falliti);
    return falliti != 0;
}
