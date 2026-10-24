/* ds4_media_storia - da una sceneggiatura a un video: per ogni scena il primo fotogramma
 * con Qwen-Image, la clip con MiniMax H3, poi le clip unite con ffmpeg. */
#ifndef DS4_MEDIA_STORIA_H
#define DS4_MEDIA_STORIA_H

#include "ds4_media.h"

#include <stdbool.h>
#include <stddef.h>

#define STORIA_MAX_SCENE 20
#define STORIA_MAX_RIF 10

typedef struct {
    double secondi;            /* 0.1..15 */
    char *fotogramma;          /* descrizione del primo fotogramma (Qwen-Image) */
    char *movimento;           /* cosa succede e cosa si sente (H3) */
    char *riferimento[STORIA_MAX_RIF];   /* immagini di riferimento del fotogramma (come img --rif) */
    int n_rif;
    char *audio;               /* wav che guida il labiale della clip (H3 con guida audio), o NULL */
} storia_scena;

typedef struct {
    storia_scena v[STORIA_MAX_SCENE];
    int n;
} storia;

/* Formato:
 *   [scena 1]
 *   secondi = 8
 *   fotogramma = ...
 *   movimento = ...
 *   riferimento = ritratto.png     (facoltativo, ripetibile fino a 10)
 *   audio = battuta.wav            (facoltativo, uno per scena)
 * '#' commenta. Le scene vanno numerate di seguito da 1; gli errori nominano scena e chiave.
 * I percorsi relativi di riferimento e audio partono dalla cartella della sceneggiatura e i
 * file devono esistere gia' alla lettura (cosi' --controlla li trova prima della GPU). */
bool storia_leggi(const char *path, storia *s, char *err, size_t err_len);
void storia_libera(storia *s);

typedef struct {
    const char *dir;           /* cartella del lavoro: scena-NN.png, scena-NN.mp4, storia.mp4 */
    int width, height;         /* 0,0 = 864x480, uguali per fotogramma e clip */
    int steps;                 /* 0 = predefiniti di img e video */
    long seed;                 /* <0 = casuale; altrimenti seed+N per la scena N */
    int da;                    /* prima scena da fare (1-based); le precedenti devono esserci */
    const char *ffmpeg;
    bool (*cancel)(void *privdata);
    void *cancel_privdata;
} storia_opzioni;

/* 0 riuscito, 1 errore, 130 annullato. Riprende: una scena il cui file esiste non si rifa'.
 * Su successo out = percorso di storia.mp4. */
int storia_esegui(ds4_media *m, const storia *s, const storia_opzioni *o,
                  char *out, size_t out_len, char *err, size_t err_len);

/* Comando "ds4-media storia": argv[0] = "storia". */
int ds4_media_storia_cli(int argc, char **argv);

#endif
