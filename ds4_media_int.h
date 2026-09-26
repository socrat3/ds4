#ifndef DS4_MEDIA_INT_H
#define DS4_MEDIA_INT_H

/* Parti interne di ds4_media condivise dai suoi file:
 *   ds4_media.c        ciclo di vita, memoria, grafo e lavoro delle immagini
 *   ds4_media_ref.c    riferimenti: dimensioni dalle intestazioni, upload
 *   ds4_media_job.c    invio a ComfyUI, attesa, scarico dei file
 *   ds4_media_video.c  video MiniMax H3
 * Non fa parte dell'API: chi usa il modulo include solo ds4_media.h. */

#include "ds4_media.h"
#include "ds4_media_http.h"

#include <pthread.h>
#include <time.h>

struct ds4_media {
    char *host, *media_dir, *comfy_output_dir;
    int port;
    ds4_media_weights weights;
    int idle_free_sec;
    bool no_gate;
    long avail_override_kib;
    ds4_media_log_fn log;
    void *log_privdata;
    ds4_media_cancel_fn cancel;
    void *cancel_privdata;
    ds4_media_progress_fn progress;
    void *progress_privdata;
    int cache_node;            /* QwenImage21Cache in ComfyUI: -1 non so, 0 no, 1 si' */
    time_t last_job;           /* fine dell'ultimo lavoro riuscito; 0 = nessuno da /free */
    pthread_mutex_t lock;      /* un lavoro ComfyUI alla volta (il serve ha un thread per connessione) */
};

/* Un lavoro: la cancellazione del chiamante, e i tempi delle fasi in ms monotoni. */
typedef struct {
    ds4_media *m;              /* impostato da media_job_bind */
    ds4_media_cancel_fn cancel;
    void *cancel_privdata;
    long t_start;              /* ingresso nel lavoro (dopo il mutex) */
    long t_submit;             /* riferimenti caricati, grafo in invio */
    long t_run;                /* ComfyUI ha iniziato a eseguire (0 = non visto) */
    long t_done;               /* ComfyUI ha finito */
} media_job;

void media_log(ds4_media *m, const char *fmt, ...);
long media_now_ms(void);
bool media_job_cancelled(ds4_media *m, const media_job *j);
/* Lega (j != NULL) o slega (NULL) la cancellazione del lavoro alle richieste HTTP di
 * questo thread: anche un invio o uno scarico lento si fermano al Ctrl+C. */
void media_job_bind(ds4_media *m, media_job *j);

/* Memoria per ComfyUI in KiB (vedi ds4_media_comfy_avail_gib), -1 se ignota; e il
 * gate: false con err se `need_kib` non ci sta, con `hint` come rimedio suggerito. */
long media_comfy_avail_kib(ds4_media *m);
/* ds4_media_mem.c: il pid in ascolto su una porta TCP locale (-1 se nessuno o non
 * leggibile) e la memoria GPU che quel processo usa secondo nvidia-smi (0 se ignota). */
long media_pid_on_port(int port);
long media_gpu_used_kib(long pid);
bool media_mem_gate(ds4_media *m, long need_kib, const char *hint, char *err, size_t err_len);

/* Carica il riferimento `idx` su ComfyUI con un nome unico generato dal modulo
 * (ref-<pid>-<idx>-<hash del contenuto>.<ext>); restituisce il nome da dare a LoadImage. */
char *media_upload_ref(ds4_media *m, const char *path, int idx, char *err, size_t err_len);

/* Dimensioni di un'immagine dalle sue intestazioni (PNG, JPEG, WebP): dai byte o dal
 * file (letto fino a 1 MiB). false se il formato non e' riconosciuto. */
bool media_image_dims(const unsigned char *b, size_t n, int *w, int *h);
bool media_file_dims(const char *path, int *w, int *h);

/* "img-AAAAMMGG-HHMMSS" (o "vid-...") in stem. */
void media_make_stem(char *stem, size_t n, const char *kind);

/* Invia `nodes` (l'oggetto JSON dei nodi del grafo) a ComfyUI, aspetta la fine
 * (websocket, con /history come ripiego) e scarica le uscite in media_dir con nome
 * <stem>-<id8>-NNN. Cancellazione e timeout tolgono il lavoro dalla coda di ComfyUI.
 * Su true out ha file, prompt_id, tempi; width/height dal primo PNG se leggibili. */
bool media_run(ds4_media *m, media_job *j, const char *nodes, const char *stem,
               int timeout_ms, ds4_media_result *out, char *err, size_t err_len);

#endif
