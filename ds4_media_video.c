/* ds4_media_video - video MiniMax H3 (immagine -> video) via ComfyUI. Vedi ds4_media.h.
 * Il grafo e' il template in ds4_media_h3.inc con segnaposto __NOME__ sostituiti da
 * valori gia' validati; invio, attesa e scarico sono quelli delle immagini. */
#include "ds4_media_int.h"
#include "ds4_media_h3.inc"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Sostituisce tutte le occorrenze di `needle` con `repl` (malloc'd). */
static char *media_replace(const char *s, const char *needle, const char *repl) {
    media_buf b = {0};
    size_t nl = strlen(needle);
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, needle);
        if (!hit) { media_buf_puts(&b, p); break; }
        media_buf_append(&b, p, (size_t)(hit - p));
        media_buf_puts(&b, repl);
        p = hit + nl;
    }
    return media_buf_take(&b);
}

/* Contenuto di una stringa JSON, senza le virgolette esterne (per i segnaposto tra ""). */
static char *media_json_inner(const char *s) {
    char *q = media_json_quote(s);   /* "...." */
    size_t n = strlen(q);
    if (n >= 2) { memmove(q, q + 1, n - 2); q[n - 2] = '\0'; }
    return q;
}

/* Fotogrammi dal tempo, sulla griglia 17k+5 di H3 (124~5s, 362~15s). */
static int media_h3_length(double seconds) {
    if (!(seconds > 0)) seconds = 5.0;
    if (seconds > 60) seconds = 60;
    long base = (long)(seconds * 24.0 + 0.5);
    long k = (base - 5 + 8) / 17;   /* arrotonda */
    long frames = 5 + k * 17;
    if (frames < 5) frames = 5;
    if (frames > 362) frames = 362;
    return (int)frames;
}

static bool media_video_job(ds4_media *m, const ds4_media_video_req *req, media_job *j,
                            ds4_media_result *out, char *err, size_t err_len) {
    if (!req->prompt || !req->prompt[0]) { media_set_err(err, err_len, "prompt vuoto"); return false; }
    if (strlen(req->prompt) > 4000) { media_set_err(err, err_len, "prompt troppo lungo (max 4000)"); return false; }
    if (!req->ref || !req->ref[0]) { media_set_err(err, err_len, "serve un'immagine di riferimento (--rif): H3 parte dal primo fotogramma"); return false; }
    int width = req->width > 0 ? req->width : 864;
    int height = req->height > 0 ? req->height : 480;
    if (width % 32 || height % 32) { media_set_err(err, err_len, "dimensioni multiple di 32"); return false; }
    if (width > DS4_MEDIA_MAX_SIDE || height > DS4_MEDIA_MAX_SIDE || (long)width * height > DS4_MEDIA_MAX_AREA) {
        media_set_err(err, err_len, "dimensioni oltre i limiti");
        return false;
    }
    if (req->steps < 0 || req->steps > 60) { media_set_err(err, err_len, "passi da 1 a 60"); return false; }
    int steps = req->steps > 0 ? req->steps : 20;
    int length = media_h3_length(req->seconds);
    long seed = req->seed >= 0 ? req->seed : (long)(time(NULL) ^ ((long)getpid() << 8)) & 0x7fffffff;

    /* H3 occupa la GPU per intero (~110 GiB in uso): a macchina scarica la memoria per
     * ComfyUI e' ~111-118 GiB. Si chiedono 100 GiB: passa a vuoto, blocca se un LLM e'
     * ancora caricato (quello che ComfyUI stesso tiene conta come disponibile). */
    if (!media_mem_gate(m, (long)DS4_MEDIA_VIDEO_NEED_GIB * 1024 * 1024,
                        "H3 vuole la GPU quasi intera: spegni ogni altro modello prima del video", err, err_len))
        return false;
    if (media_job_cancelled(m, j)) { media_set_err(err, err_len, "annullato"); return false; }

    char *refname = media_upload_ref(m, req->ref, 0, err, err_len);
    if (!refname) return false;

    char stem[64], prefix[160];
    media_make_stem(stem, sizeof(stem), "vid");
    snprintf(prefix, sizeof(prefix), "ds4/%s-%ld", stem, seed);
    char *qprompt = media_json_inner(req->prompt);
    char *qimage = media_json_inner(refname);
    char nums[5][24];
    snprintf(nums[0], 24, "%d", width);
    snprintf(nums[1], 24, "%d", height);
    snprintf(nums[2], 24, "%d", length);
    snprintf(nums[3], 24, "%ld", seed);
    snprintf(nums[4], 24, "%d", steps);
    /* __PROMPT__ per ultimo: il testo dell'utente puo' contenere un altro segnaposto. */
    const char *subs[][2] = {{"__IMAGE__", qimage}, {"__PREFIX__", prefix},
                             {"__WIDTH__", nums[0]}, {"__HEIGHT__", nums[1]}, {"__LENGTH__", nums[2]},
                             {"__SEED__", nums[3]}, {"__STEPS__", nums[4]}, {"__PROMPT__", qprompt}};
    char *g = media_xstrdup(DS4_MEDIA_H3_TEMPLATE);
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        char *n = media_replace(g, subs[i][0], subs[i][1]);
        free(g);
        g = n;
    }
    free(qprompt);
    free(qimage);

    media_log(m, "ds4: media video %dx%d %d fotogrammi (~%.1f s), attendo H3 (minuti)...",
              width, height, length, length / 24.0);
    bool ok = media_run(m, j, g, stem, 1800000, out, err, err_len);   /* fino a 30 min */
    free(g);
    media_forget_upload(m, refname);
    free(refname);
    if (!ok) return false;
    out->width = width;
    out->height = height;
    out->seed = seed;
    media_log(m, "ds4: media video %s %ld ms -> %s", out->prompt_id, out->ms, out->files[0]);
    return true;
}

/* Sostituisce `needle` con `repl` pretendendo che compaia esattamente una volta: il
 * grafo della testa parlante nasce modificando il template, e un template cambiato
 * deve dare un errore chiaro, non un grafo sbagliato. */
static bool media_replace_once(char **g, const char *needle, const char *repl) {
    const char *hit = strstr(*g, needle);
    if (!hit || strstr(hit + 1, needle)) return false;
    char *n = media_replace(*g, needle, repl);
    free(*g);
    *g = n;
    return true;
}

static bool media_talk_job(ds4_media *m, const ds4_media_talk_req *req, media_job *j,
                           ds4_media_result *out, char *err, size_t err_len) {
    if (!req->prompt || !req->prompt[0]) { media_set_err(err, err_len, "prompt vuoto"); return false; }
    if (strlen(req->prompt) > 8000) { media_set_err(err, err_len, "prompt troppo lungo (max 8000)"); return false; }
    if (!req->face || !req->face[0] || !req->audio || !req->audio[0]) {
        media_set_err(err, err_len, "servono la foto e la traccia audio");
        return false;
    }
    int width = req->width > 0 ? req->width : 576;
    int height = req->height > 0 ? req->height : 576;
    if (width % 32 || height % 32 || width > DS4_MEDIA_MAX_SIDE || height > DS4_MEDIA_MAX_SIDE ||
        (long)width * height > DS4_MEDIA_MAX_AREA) {
        media_set_err(err, err_len, "dimensioni: multipli di 32 entro i limiti");
        return false;
    }
    if (req->frames < 5 || req->frames > DS4_MEDIA_TALK_MAX_FRAMES || (req->frames - 5) % 17) {
        media_set_err(err, err_len, "fotogrammi sulla griglia 17k+5, da 5 a %d", DS4_MEDIA_TALK_MAX_FRAMES);
        return false;
    }
    if (req->steps < 0 || req->steps > 60) { media_set_err(err, err_len, "passi da 1 a 60"); return false; }
    int steps = req->steps > 0 ? req->steps : 20;
    long seed = req->seed >= 0 ? req->seed : (long)(time(NULL) ^ ((long)getpid() << 8)) & 0x7fffffff;
    if (!media_mem_gate(m, (long)DS4_MEDIA_VIDEO_NEED_GIB * 1024 * 1024,
                        "H3 vuole la GPU quasi intera: spegni ogni altro modello", err, err_len))
        return false;
    if (media_job_cancelled(m, j)) { media_set_err(err, err_len, "annullato"); return false; }

    char *face = media_upload_ref(m, req->face, 0, err, err_len);
    if (!face) return false;
    char *audio = media_upload_ref(m, req->audio, 1, err, err_len);
    if (!audio) { free(face); return false; }

    char stem[64], prefix[160], nums[5][24];
    media_make_stem(stem, sizeof(stem), "talk");
    snprintf(prefix, sizeof(prefix), "ds4/%s-%ld", stem, seed);
    char *qprompt = media_json_inner(req->prompt), *qface = media_json_inner(face);
    char *qaudio = media_json_quote(audio);
    snprintf(nums[0], 24, "%d", width);
    snprintf(nums[1], 24, "%d", height);
    snprintf(nums[2], 24, "%d", req->frames);
    snprintf(nums[3], 24, "%ld", seed);
    snprintf(nums[4], 24, "%d", steps);
    const char *subs[][2] = {{"__IMAGE__", qface}, {"__PREFIX__", prefix}, {"__WIDTH__", nums[0]},
                             {"__HEIGHT__", nums[1]}, {"__LENGTH__", nums[2]}, {"__SEED__", nums[3]},
                             {"__STEPS__", nums[4]}, {"__PROMPT__", qprompt}};
    char *g = media_xstrdup(DS4_MEDIA_H3_TEMPLATE);
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        char *n = media_replace(g, subs[i][0], subs[i][1]);
        free(g);
        g = n;
    }
    free(qprompt);
    free(qface);

    /* Dal template image->video: la guida audio tra il condizionamento e il guider,
     * e (con anchor_end) la stessa foto anche come ultimo fotogramma. */
    media_buf tail = {0};
    media_buf_puts(&tail, ", \"200\": {\"class_type\": \"LoadAudio\", \"inputs\": {\"audio\": ");
    media_buf_puts(&tail, qaudio);
    media_buf_puts(&tail, "}}, \"201\": {\"class_type\": \"MiniMaxH3AddGuide\", \"inputs\": {\"positive\": [\"104\", 0], "
                          "\"latent\": [\"104\", 1], \"audio_vae\": [\"24\", 0], \"audio\": [\"200\", 0], \"frame_idx\": 0}}}");
    free(qaudio);
    size_t gl = strlen(g);
    bool ok = gl > 0 && g[gl - 1] == '}' &&
              media_replace_once(&g, "\"conditioning\": [\"104\", 0]", "\"conditioning\": [\"201\", 0]") &&
              (!req->anchor_end ||
               media_replace_once(&g, "\"first_frame\": [\"105\", 0]",
                                  "\"first_frame\": [\"105\", 0], \"last_frame\": [\"105\", 0]"));
    if (!ok) {
        free(g);
        free(tail.ptr);
        media_forget_upload(m, face);
        media_forget_upload(m, audio);
        free(face);
        free(audio);
        media_set_err(err, err_len, "template H3 inatteso: non so dove agganciare l'audio");
        return false;
    }
    g[strlen(g) - 1] = '\0';   /* toglie la } finale: i nodi nuovi chiudono l'oggetto */
    media_buf full = {0};
    media_buf_puts(&full, g);
    media_buf_puts(&full, tail.ptr);
    free(g);
    free(tail.ptr);

    media_log(m, "ds4: media testa parlante %dx%d %d fotogrammi (%.2f s), attendo H3 (minuti)...",
              width, height, req->frames, req->frames / 24.0);
    ok = media_run(m, j, full.ptr, stem, 3600000, out, err, err_len);   /* fino a 60 min: la GPU puo' essere in coda */
    free(full.ptr);
    media_forget_upload(m, face);
    media_forget_upload(m, audio);
    free(face);
    free(audio);
    if (!ok) return false;
    out->width = width;
    out->height = height;
    out->seed = seed;
    media_log(m, "ds4: media testa parlante %s %ld ms -> %s", out->prompt_id, out->ms, out->files[0]);
    return true;
}

bool ds4_media_talk(ds4_media *m, const ds4_media_talk_req *req,
                    ds4_media_result *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&m->lock);
    media_job j = {.cancel = req->cancel, .cancel_privdata = req->cancel_privdata, .t_start = media_now_ms()};
    media_job_bind(m, &j);
    bool ok = media_talk_job(m, req, &j, out, err, err_len);
    media_job_bind(m, NULL);
    pthread_mutex_unlock(&m->lock);
    return ok;
}

bool ds4_media_video(ds4_media *m, const ds4_media_video_req *req,
                     ds4_media_result *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&m->lock);
    media_job j = {.cancel = req->cancel, .cancel_privdata = req->cancel_privdata, .t_start = media_now_ms()};
    media_job_bind(m, &j);
    bool ok = media_video_job(m, req, &j, out, err, err_len);
    media_job_bind(m, NULL);
    pthread_mutex_unlock(&m->lock);
    return ok;
}
