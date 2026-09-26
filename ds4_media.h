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

typedef enum { DS4_MEDIA_INT8 = 0, DS4_MEDIA_BF16 = 1 } ds4_media_weights;

typedef struct {
    const char *host;          /* ComfyUI, default "127.0.0.1" */
    int port;                  /* default 8188 */
    const char *media_dir;     /* cartella di uscita; default ~/.ds4/media */
    ds4_media_weights weights; /* default int8 */
    int idle_free_sec;         /* dopo N s senza lavori, /free; 0 = mai */
    bool no_gate;              /* salta il controllo di memoria (per i test) */
    long avail_override_kib;   /* >0: usa questo al posto di /proc/meminfo (test) */
    ds4_media_log_fn log;
    void *log_privdata;
    ds4_media_cancel_fn cancel;
    void *cancel_privdata;
} ds4_media_config;

typedef struct ds4_media ds4_media;

/* Una richiesta di immagine da testo (o modifica, se n_refs>0). Le dimensioni sono
 * multipli di 32; seed<0 = casuale; steps 0 = default (25). refs = percorsi di file
 * immagine locali, al massimo 10, il primo e' il bersaglio della modifica. */
typedef struct {
    const char *prompt;
    const char *negative;      /* opzionale */
    int width, height;         /* 0,0 = 1024x1024 */
    int steps;                 /* 0 = 25 */
    double cfg;                /* <=0 = 1.0 */
    long seed;                 /* <0 = casuale */
    ds4_media_weights weights; /* override della config per questo lavoro */
    const char *const *refs;   /* percorsi dei file di riferimento */
    int n_refs;
    bool transparent;          /* avvolge il prompt nella formula RGBA, PNG con alfa */
} ds4_media_image_req;

/* Esito: percorsi dei PNG scritti (n_files), primo file per comodita', metadati. */
typedef struct {
    char **files;
    int n_files;
    int width, height;
    long seed;
    long ms;                   /* durata totale in millisecondi */
    char prompt_id[64];
} ds4_media_result;

void ds4_media_result_free(ds4_media_result *r);

ds4_media *ds4_media_create(const ds4_media_config *cfg);
void ds4_media_free(ds4_media *m);

/* Genera una o piu' immagini. Su true, *out e' riempito (da liberare con
 * ds4_media_result_free); su false, err spiega causa e rimedio. */
bool ds4_media_image(ds4_media *m, const ds4_media_image_req *req,
                     ds4_media_result *out, char *err, size_t err_len);

/* ComfyUI risponde? Riempie model_names con "sì"/"no" per Qwen-Image se richiesto. */
bool ds4_media_health(ds4_media *m, char *err, size_t err_len);

/* Scarica i modelli da ComfyUI (POST /free): restituisce memoria al modello di chat. */
bool ds4_media_free_models(ds4_media *m, char *err, size_t err_len);

#endif
