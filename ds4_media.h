#ifndef DS4_MEDIA_H
#define DS4_MEDIA_H

/* ds4_media - genera immagini (e, in seguito, video) chiamando ComfyUI via HTTP.
 *
 * Il modulo non tocca la GPU: fa da regista verso ComfyUI (Qwen-Image 2.1 per le
 * immagini, MiniMax H3 per i video). I grafi sono fissi nel modulo; dall'esterno
 * arrivano solo parametri validati. Spento per default: senza un ds4_media creato
 * non apre socket e non ha costo.
 *
 * Stile: come ds4_web.h. Struct opaca, _create/_free, funzioni che tornano bool
 * con (err, err_len); i risultati sono malloc'd e li libera il chiamante. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Log di una riga (senza newline finale) su un canale scelto dal chiamante
 * (la CLI su stderr, il server via server_log). */
typedef void (*ds4_media_log_fn)(void *privdata, const char *message);
/* true = il chiamante chiede di annullare il lavoro in corso (Ctrl+C, disconnessione). */
typedef bool (*ds4_media_cancel_fn)(void *privdata);
/* Passo `step` di `total` del campionamento (dagli eventi websocket di ComfyUI). */
typedef void (*ds4_media_progress_fn)(void *privdata, int step, int total);

typedef enum { DS4_MEDIA_INT8 = 0, DS4_MEDIA_BF16 = 1 } ds4_media_weights;

typedef struct {
    const char *host;          /* ComfyUI, default "127.0.0.1" */
    int port;                  /* default 8188 */
    const char *media_dir;     /* cartella di uscita; default ~/.ds4/media */
    const char *comfy_output_dir; /* cartella output di ComfyUI (host locale): dopo aver scaricato
                                   * il file, ne rimuove la copia doppia lasciata da ComfyUI. NULL =
                                   * non toccare (host remoto, o si vuole tenere anche quella copia). */
    ds4_media_weights weights; /* default int8 */
    int idle_free_sec;         /* dopo N s senza lavori, /free; 0 = mai */
    bool no_gate;              /* salta il controllo di memoria (per i test) */
    long avail_override_kib;   /* >0: usa questo al posto di /proc/meminfo (test) */
    ds4_media_log_fn log;
    void *log_privdata;
    ds4_media_cancel_fn cancel;
    void *cancel_privdata;
    ds4_media_progress_fn progress;   /* opzionale */
    void *progress_privdata;
} ds4_media_config;

typedef struct ds4_media ds4_media;

/* Una richiesta di immagine da testo (o modifica, se n_refs>0). Le dimensioni sono
 * multipli di 32, lato <= DS4_MEDIA_MAX_SIDE, area <= DS4_MEDIA_MAX_AREA; seed<0 =
 * casuale; steps 0 = default (25). refs = percorsi di file immagine locali, al massimo
 * 10, il primo e' il bersaglio della modifica: senza width/height l'uscita prende le
 * proporzioni e (entro i limiti) l'area del primo riferimento. */
typedef struct {
    const char *prompt;
    const char *negative;      /* opzionale; con cfg 1 ComfyUI lo ignora, quindi se c'e'
                                * e cfg non e' dato si usa DS4_MEDIA_NEG_CFG */
    int width, height;         /* 0,0 = 1024x1024 (o dal primo riferimento) */
    int steps;                 /* 0 = 25 */
    double cfg;                /* <=0 = 1.0 (DS4_MEDIA_NEG_CFG con un negativo) */
    int n;                     /* immagini nello stesso lavoro, 0 = 1, max DS4_MEDIA_MAX_N */
    long seed;                 /* <0 = casuale */
    ds4_media_weights weights; /* pesi di questo lavoro (il chiamante parte da
                                * ds4_media_default_weights se non ha un override) */
    const char *const *refs;   /* percorsi dei file di riferimento */
    int n_refs;
    bool transparent;          /* avvolge il prompt nella formula RGBA, PNG con alfa */
    ds4_media_cancel_fn cancel;   /* opzionale, per questo lavoro (oltre a quella della config) */
    void *cancel_privdata;
} ds4_media_image_req;

#define DS4_MEDIA_MAX_SIDE 2752
#define DS4_MEDIA_MAX_AREA (2752L * 1536)
#define DS4_MEDIA_MAX_N 4
#define DS4_MEDIA_NEG_CFG 2.5
#define DS4_MEDIA_VIDEO_NEED_GIB 100

/* Esito: percorsi dei file scritti (n_files), img-<data>-<ora>-<prompt_id8>-NNN.png
 * (vid-...mp4 per i video), primo file per comodita', metadati. I lavori sono
 * serializzati da un mutex interno: piu' thread possono chiamare queste funzioni. */
typedef struct {
    char **files;
    int n_files;
    int width, height;
    long seed;
    long ms;                   /* durata totale in millisecondi */
    long ms_upload, ms_queue, ms_run, ms_fetch;   /* fasi: riferimenti, coda, generazione, scarico */
    bool invalid;              /* su false: la richiesta stessa non era valida (errore del chiamante) */
    char prompt_id[64];
} ds4_media_result;

void ds4_media_result_free(ds4_media_result *r);

ds4_media *ds4_media_create(const ds4_media_config *cfg);
void ds4_media_free(ds4_media *m);

/* Video MiniMax H3 (image->video): dall'immagine di riferimento (primo fotogramma)
 * genera una clip breve con audio del modello. ref e' obbligatorio. */
typedef struct {
    const char *prompt;
    const char *ref;           /* file immagine: primo fotogramma (obbligatorio) */
    double seconds;            /* 0 = 5 s; max ~15 (griglia 17k+5) */
    int width, height;         /* 0,0 = 864x480 (0,4 MP 16:9) */
    int steps;                 /* 0 = 20 */
    long seed;                 /* <0 = casuale */
    ds4_media_cancel_fn cancel;   /* opzionale, come per le immagini */
    void *cancel_privdata;
} ds4_media_video_req;

/* Genera una o piu' immagini. Su true, *out e' riempito (da liberare con
 * ds4_media_result_free); su false, err spiega causa e rimedio. */
bool ds4_media_image(ds4_media *m, const ds4_media_image_req *req,
                     ds4_media_result *out, char *err, size_t err_len);

/* Genera un video H3. Occupa la GPU per intero (~110 GiB) e dura minuti: da usare a
 * GPU libera. Su true out->files[0] e' l'MP4. */
bool ds4_media_video(ds4_media *m, const ds4_media_video_req *req,
                     ds4_media_result *out, char *err, size_t err_len);

/* ComfyUI risponde? Riempie model_names con "sì"/"no" per Qwen-Image se richiesto. */
bool ds4_media_health(ds4_media *m, char *err, size_t err_len);

/* Scarica i modelli da ComfyUI (POST /free): restituisce memoria al modello di chat. */
bool ds4_media_free_models(ds4_media *m, char *err, size_t err_len);

/* Da chiamare periodicamente (il serve lo fa ogni 500 ms): se idle_free_sec > 0 e
 * l'ultimo lavoro e' finito da almeno idle_free_sec, esegue /free. true se ha liberato.
 * Non blocca: se un lavoro e' in corso non fa nulla. */
bool ds4_media_idle_tick(ds4_media *m);

/* Pesi di default della configurazione (per le richieste che non li specificano). */
ds4_media_weights ds4_media_default_weights(const ds4_media *m);

/* MemAvailable in GiB (da /proc/meminfo), -1 se non leggibile. */
long ds4_media_avail_gib(void);

/* Memoria a disposizione di ComfyUI in GiB: MemAvailable piu' quella che ComfyUI tiene
 * gia' (torch_vram_total di /system_stats), che riusa per il prossimo lavoro. -1 se non
 * leggibile. E' la stessa misura del gate interno. */
long ds4_media_comfy_avail_gib(ds4_media *m);

/* Fabbisogno stimato in GiB (margine compreso) di un'immagine: pesi, dimensioni, n. */
long ds4_media_image_need_gib(ds4_media_weights w, int width, int height, int n);

/* "1024x1024" -> width,height (multipli di 32, lato e area nei limiti). false se
 * malformato o fuori limite. */
bool ds4_media_parse_size(const char *s, int *width, int *height);

/* Cartella di uscita configurata (per servire i file con response_format=url). */
const char *ds4_media_dir(const ds4_media *m);

/* Server HTTP compatibile con l'API Immagini di OpenAI (POST /v1/images/generations,
 * GET /v1/media/files/<nome>, GET /v1/models): bersaglio fisso per Open WebUI. Blocca
 * finche' *stop diventa non-zero, poi aspetta (fino a 60 s) le connessioni in corso.
 * false se non parte, o se allo scadere qualche connessione usa ancora m: in quel
 * caso m non va liberata. */
bool ds4_media_serve(ds4_media *m, int port, volatile int *stop, char *err, size_t err_len);

#endif
