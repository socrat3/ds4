/* ds4_media_job - il viaggio di un lavoro verso ComfyUI e ritorno:
 * invio del grafo, attesa, scarico dei file. Vedi ds4_media_int.h.
 *
 * L'attesa ascolta il websocket di ComfyUI: la fine arriva subito, e con lei il
 * progresso passo per passo. Il websocket pero' e' un lusso, non una dipendenza: se
 * l'upgrade fallisce o la connessione cade si torna al polling di /history, e anche
 * col websocket attivo /history viene interrogato ogni 5 s, cosi' un evento perso non
 * lascia il lavoro appeso fino al timeout. La verita' sull'esito la dice sempre
 * /history, mai l'evento. */
#include "ds4_media_int.h"
#include "ds4_media_ws.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void media_log(ds4_media *m, const char *fmt, ...) {
    if (!m->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m->log(m->log_privdata, buf);
}

long media_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool media_job_cancelled(ds4_media *m, const media_job *j) {
    if (m->cancel && m->cancel(m->cancel_privdata)) return true;
    return j && j->cancel && j->cancel(j->cancel_privdata);
}

void media_make_stem(char *stem, size_t n, const char *kind) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char date[32];
    strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &tmv);
    snprintf(stem, n, "%s-%s", kind, date);
}

static void media_sleep_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ── attesa ───────────────────────────────────────────────────────────────── */

/* Esito dal corpo di /history/<id>: 1 riuscito, -1 fallito (err spiegato), 0 non ancora. */
static int media_history_status(const char *body, char *err, size_t err_len) {
    char *st = media_json_str(body, "status_str");
    if (!st) return 0;
    int r = strcmp(st, "success") == 0 ? 1 : -1;
    free(st);
    if (r < 0) {
        char *msg = media_json_str(body, "exception_message");
        if (msg) media_set_err(err, err_len, "ComfyUI: %s", msg);
        else if (strstr(body, "execution_interrupted")) media_set_err(err, err_len, "ComfyUI ha interrotto il lavoro");
        else media_set_err(err, err_len, "ComfyUI: errore di esecuzione");
        free(msg);
    }
    return r;
}

/* Toglie il lavoro dalla coda (se non e' ancora partito) e lo interrompe (se e' in
 * esecuzione). Entrambe le richieste sono mirate a questo id: non toccano altri. */
static void media_cancel_remote(ds4_media *m, const char *pid) {
    char body[160], e[64];
    media_http_response r = {0};
    snprintf(body, sizeof(body), "{\"delete\":[\"%s\"]}", pid);
    media_http_post(m->host, m->port, "/queue", "application/json", body, strlen(body), 5000, &r, e, sizeof(e));
    media_http_response_free(&r);
    snprintf(body, sizeof(body), "{\"prompt_id\":\"%s\"}", pid);
    media_http_post(m->host, m->port, "/interrupt", "application/json", body, strlen(body), 5000, &r, e, sizeof(e));
    media_http_response_free(&r);
}

/* Un evento websocket: segna l'inizio, riporta il progresso, e mette *done quando
 * ComfyUI dice di aver finito questo lavoro (bene o male: lo dira' /history). */
static void media_ws_event(ds4_media *m, media_job *j, const char *pid, const char *msg, bool *done) {
    char *type = media_json_str(msg, "type");
    char *id = media_json_str(msg, "prompt_id");
    if (type && id && strcmp(id, pid) == 0) {
        if (!strcmp(type, "execution_start")) {
            if (!j->t_run) j->t_run = media_now_ms();
        } else if (!strcmp(type, "progress")) {
            bool fv = false, fm = false;
            long v = media_json_int(msg, "value", &fv), mx = media_json_int(msg, "max", &fm);
            if (!j->t_run) j->t_run = media_now_ms();
            if (fv && fm && m->progress && v >= 0 && mx > 0 && v <= mx && mx < 100000)
                m->progress(m->progress_privdata, (int)v, (int)mx);
        } else if (!strcmp(type, "execution_success") || !strcmp(type, "execution_error") ||
                   !strcmp(type, "execution_interrupted")) {
            *done = true;
        } else if (!strcmp(type, "executing") &&
                   (strstr(msg, "\"node\": null") || strstr(msg, "\"node\":null"))) {
            *done = true;
        }
    }
    free(type);
    free(id);
}

static bool media_wait(ds4_media *m, media_job *j, media_ws **wsp, const char *pid,
                       long deadline, char **out_history, char *err, size_t err_len) {
    char path[128];
    snprintf(path, sizeof(path), "/history/%s", pid);
    bool done = false;
    long done_at = 0;
    long next_poll = media_now_ms() + (*wsp ? 5000 : 1000);
    for (;;) {
        if (media_job_cancelled(m, j)) {
            media_cancel_remote(m, pid);
            media_set_err(err, err_len, "annullato");
            return false;
        }
        long now = media_now_ms();
        if (now >= deadline) {
            media_cancel_remote(m, pid);   /* non lasciare la GPU occupata da un lavoro orfano */
            media_set_err(err, err_len, "timeout: ComfyUI non ha finito in tempo (lavoro tolto dalla coda)");
            return false;
        }
        if (*wsp && !done) {
            char *msg = NULL;
            size_t len = 0;
            bool binary = false;
            int r = media_ws_read(*wsp, 250, &msg, &len, &binary);
            if (r < 0) {
                media_ws_close(*wsp);
                *wsp = NULL;
                next_poll = now;
                media_log(m, "ds4: media websocket chiuso, continuo con /history");
            } else if (r == 1) {
                if (!binary) media_ws_event(m, j, pid, msg, &done);
                if (done) done_at = media_now_ms();
                free(msg);
            }
        } else if (!done) {
            long nap = next_poll - now;
            media_sleep_ms(nap < 0 ? 0 : nap > 250 ? 250 : nap);
        }
        if (!done && media_now_ms() < next_poll) continue;

        media_http_response resp = {0};
        if (!media_http_get(m->host, m->port, path, 10000, &resp, err, err_len)) return false;
        int st = media_history_status(resp.body, err, err_len);
        if (st != 0) {
            j->t_done = media_now_ms();
            if (st < 0) { media_http_response_free(&resp); return false; }
            *out_history = resp.body;
            resp.body = NULL;
            media_http_response_free(&resp);
            return true;
        }
        media_http_response_free(&resp);
        /* Dopo l'evento di fine /history puo' tardare di un attimo: si riprova presto,
         * ma non per sempre. */
        if (done && media_now_ms() - done_at > 30000) {
            media_set_err(err, err_len, "ComfyUI ha finito ma /history non riporta l'esito");
            return false;
        }
        if (done) media_sleep_ms(100);
        next_poll = media_now_ms() + (done ? 0 : *wsp ? 5000 : 1000);
    }
}

/* ── scarico ──────────────────────────────────────────────────────────────── */

/* Componente di percorso ricevuto da ComfyUI e usato su disco locale: niente "..",
 * niente assoluti. Per il nome file anche niente '/'. */
static bool media_safe_rel(const char *s, bool allow_slash) {
    if (!s || s[0] == '/') return false;
    if (!allow_slash && strchr(s, '/')) return false;
    for (const char *p = s; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == s || p[-1] == '/') && (p[2] == '/' || p[2] == '\0'))
            return false;
    return true;
}

/* Scrive i byte in media_dir/<stem>-NNN<ext> senza mai sovrascrivere: file temporaneo
 * completo, poi link() sul nome definitivo, che fallisce con EEXIST se un altro
 * processo lo ha preso nel frattempo. Chi legge non vede mai un file a meta'. */
static char *media_store(ds4_media *m, const char *stem, int *idx, const char *ext,
                         const char *bytes, size_t len, char *err, size_t err_len) {
    char tmp[1024], dest[1024];
    snprintf(tmp, sizeof(tmp), "%s/.ds4-media-XXXXXX", m->media_dir);
    int fd = mkstemp(tmp);
    if (fd < 0) { media_set_err(err, err_len, "non creo un file in %s: %s", m->media_dir, strerror(errno)); return NULL; }
    fchmod(fd, 0644);
    bool ok = media_write_all(fd, bytes, len) == 0;
    int saved = errno;
    if (close(fd) != 0 && ok) { ok = false; saved = errno; }
    if (!ok) {
        unlink(tmp);
        media_set_err(err, err_len, "scrittura in %s non riuscita: %s", m->media_dir, strerror(saved));
        return NULL;
    }
    for (; *idx < 1000; (*idx)++) {
        snprintf(dest, sizeof(dest), "%s/%s-%03d%s", m->media_dir, stem, *idx, ext);
        if (link(tmp, dest) == 0) break;
        if (errno == EEXIST) continue;
        /* filesystem senza hard link (exFAT...): rename, dopo aver controllato il nome */
        if (access(dest, F_OK) != 0 && rename(tmp, dest) == 0) { (*idx)++; return media_xstrdup(dest); }
        saved = errno;
        unlink(tmp);
        media_set_err(err, err_len, "non salvo %s: %s", dest, strerror(saved));
        return NULL;
    }
    unlink(tmp);
    if (*idx >= 1000) { media_set_err(err, err_len, "troppi file con il nome %s", stem); return NULL; }
    (*idx)++;
    return media_xstrdup(dest);
}

/* Scarica le uscite elencate nel corpo di /history. Ogni immagine e' un oggetto
 * {"filename":..,"subfolder":..,"type":..}: per ogni "filename" isolo l'oggetto (dal
 * '{' precedente al '}' seguente) e ne leggo i tre campi. */
static bool media_fetch_outputs(ds4_media *m, const char *history, const char *stem,
                                ds4_media_result *out, char *err, size_t err_len) {
    const char *p = history, *hit;
    int cap = 0, idx = 0;
    bool ok = true;
    while (ok && (hit = strstr(p, "\"filename\"")) != NULL) {
        const char *beg = hit, *end = strchr(hit, '}');
        while (beg > history && *beg != '{') beg--;
        if (!end) break;
        p = end + 1;
        size_t span = (size_t)(end - beg + 1);
        char *chunk = media_xmalloc(span + 1);
        memcpy(chunk, beg, span);
        chunk[span] = '\0';
        char *fn = media_json_str(chunk, "filename");
        char *sub = media_json_str(chunk, "subfolder");
        char *type = media_json_str(chunk, "type");
        free(chunk);
        if (fn && fn[0] && (!type || strcmp(type, "output") == 0)) {
            char *efn = media_url_encode(fn), *esub = media_url_encode(sub ? sub : "");
            media_buf vp = {0};
            media_buf_puts(&vp, "/view?filename=");
            media_buf_puts(&vp, efn);
            media_buf_puts(&vp, "&subfolder=");
            media_buf_puts(&vp, esub);
            media_buf_puts(&vp, "&type=output");
            free(efn);
            free(esub);
            media_http_response vr = {0};
            if (!media_http_get(m->host, m->port, vp.ptr, 60000, &vr, err, err_len)) ok = false;
            else if (vr.status != 200) { media_set_err(err, err_len, "/view di %s ha risposto %d", fn, vr.status); ok = false; }
            free(vp.ptr);
            char ext[8] = ".png";
            const char *dot = strrchr(fn, '.');
            if (dot && strlen(dot) <= 5 && dot[1]) {
                bool clean = true;
                for (const char *c = dot + 1; *c; c++)
                    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) clean = false;
                if (clean) snprintf(ext, sizeof(ext), "%s", dot);
            }
            char *dest = ok ? media_store(m, stem, &idx, ext, vr.body, vr.body_len, err, err_len) : NULL;
            if (ok && !dest) ok = false;
            if (dest) {
                if (out->n_files == cap) {
                    cap = cap ? cap * 2 : 4;
                    char **nl = realloc(out->files, (size_t)cap * sizeof(char *));
                    if (!nl) abort();
                    out->files = nl;
                }
                out->files[out->n_files++] = dest;
                int w, h;
                if (out->n_files == 1 && media_image_dims((unsigned char *)vr.body, vr.body_len, &w, &h)) {
                    out->width = w;
                    out->height = h;
                }
                /* Togli la copia doppia che ComfyUI ha salvato (host locale): niente file sparsi. */
                if (m->comfy_output_dir && media_safe_rel(fn, false) && (!sub || media_safe_rel(sub, true))) {
                    char copy[2048];
                    if (sub && sub[0]) snprintf(copy, sizeof(copy), "%s/%s/%s", m->comfy_output_dir, sub, fn);
                    else snprintf(copy, sizeof(copy), "%s/%s", m->comfy_output_dir, fn);
                    unlink(copy);
                }
            }
            media_http_response_free(&vr);
        }
        free(fn);
        free(sub);
        free(type);
    }
    if (ok && out->n_files == 0) { media_set_err(err, err_len, "nessuna immagine prodotta da ComfyUI"); ok = false; }
    return ok;
}

/* ── il lavoro intero ─────────────────────────────────────────────────────── */

/* L'id di ComfyUI finisce in URL e nomi di file: si accetta solo [A-Za-z0-9_-]. */
static bool media_pid_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 63) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
    return true;
}

bool media_run(ds4_media *m, media_job *j, const char *nodes, const char *stem_in,
               int timeout_ms, ds4_media_result *out, char *err, size_t err_len) {
    static unsigned seq;
    char cid[64], werr[160];
    snprintf(cid, sizeof(cid), "ds4-media-%d-%u", (int)getpid(), __sync_fetch_and_add(&seq, 1));
    /* Il websocket si apre prima di inviare il grafo: nessun evento va perso. */
    media_ws *ws = media_ws_open(m->host, m->port, cid, werr, sizeof(werr));
    if (!ws) media_log(m, "ds4: media websocket non disponibile (%s): attendo con /history", werr);

    media_buf body = {0};
    media_buf_puts(&body, "{\"prompt\":");
    media_buf_puts(&body, nodes);
    media_buf_puts(&body, ",\"client_id\":\"");
    media_buf_puts(&body, cid);
    media_buf_puts(&body, "\"}");
    j->t_submit = media_now_ms();
    media_http_response resp = {0};
    bool ok = media_http_post(m->host, m->port, "/prompt", "application/json",
                              body.ptr, body.len, 30000, &resp, err, err_len);
    free(body.ptr);
    if (!ok) { media_ws_close(ws); return false; }
    if (resp.status != 200) {
        char *msg = media_json_str(resp.body, "message");
        char *type = media_json_str(resp.body, "type");
        media_set_err(err, err_len, "ComfyUI ha rifiutato il grafo (%d)%s%s%s%s", resp.status,
                      type ? ": " : "", type ? type : "", msg ? " - " : "", msg ? msg : "");
        free(msg);
        free(type);
        media_http_response_free(&resp);
        media_ws_close(ws);
        return false;
    }
    char *pid = media_json_str(resp.body, "prompt_id");
    media_http_response_free(&resp);
    if (!media_pid_ok(pid)) {
        free(pid);
        media_ws_close(ws);
        media_set_err(err, err_len, "ComfyUI non ha dato un prompt_id valido");
        return false;
    }
    snprintf(out->prompt_id, sizeof(out->prompt_id), "%s", pid);
    char stem[160];
    snprintf(stem, sizeof(stem), "%s-%.8s", stem_in, pid);
    media_log(m, "ds4: media inviato a ComfyUI (id %s), attendo la generazione"
                 " (in coda dietro altri lavori GPU puo' richiedere piu' tempo)...", pid);

    char *history = NULL;
    ok = media_wait(m, j, &ws, pid, j->t_submit + timeout_ms, &history, err, err_len);
    media_ws_close(ws);
    free(pid);
    if (!ok) return false;
    ok = media_fetch_outputs(m, history, stem, out, err, err_len);
    free(history);
    if (!ok) { ds4_media_result_free(out); return false; }

    long end = media_now_ms();
    out->ms = end - j->t_start;
    out->ms_upload = j->t_submit - j->t_start;
    out->ms_queue = (j->t_run ? j->t_run : j->t_done) - j->t_submit;
    out->ms_run = j->t_run ? j->t_done - j->t_run : 0;
    out->ms_fetch = end - j->t_done;
    m->last_job = time(NULL);
    return true;
}
