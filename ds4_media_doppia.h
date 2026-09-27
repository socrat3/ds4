#ifndef DS4_MEDIA_DOPPIA_H
#define DS4_MEDIA_DOPPIA_H

/* ds4-media doppia: un video (YouTube o file) doppiato in italiano con il volto e la
 * voce dell'utente. Fasi, ognuna con il suo file nella cartella del lavoro: se il
 * file c'e' la fase si salta, cosi' un lavoro interrotto riparte da dove era.
 *
 *   scarica    sorgente.mp4          yt-dlp, o un file locale in qualsiasi formato
 *                                    (copiato se mp4 lo accetta, se no ricodificato)
 *   lingua     lingua.txt            riconosciuta dall'audio (lingua = auto)
 *   trascrivi  parole.csv            whisper.cpp, una parola per riga con i tempi
 *   frasi      frasi.tsv             parole -> frasi da 3 a 9 s
 *   traduci    frasi.tsv (colonna 4) ds4-server, blocchi da 20 frasi (non se gia' in italiano)
 *   voce       voce_it.wav           conversione della voce originale nella tua (stessi
 *                                    tempi: video gia' in italiano) o sintesi della traduzione
 *   posizione  posizione.txt         inquadrature riconosciute tratto per tratto: webcam
 *                                    tonda o rettangolare, persona intera, nessuno
 *   pezzi      pezzi/pNNN.mp4        teste parlanti H3 da 15 s, ancorate alla foto
 *   monta      testa.mp4, scena.mp4  pezzi uniti; scena = testa nel cerchio del video
 *   titolo     <nome>_testa.mp4 ...  titolo di testa da titoli.txt (se richiesto)
 *
 * Tutte le scelte stanno in una tabella di voci (doppia_voce): il wizard le mostra e
 * le fa cambiare, doppia.conf le conserva. Stile come il resto di ds4_media. */

#include "ds4_media.h"

#include <stdbool.h>
#include <stddef.h>

/* Una voce di configurazione. `scelte` = valori ammessi separati da '|' (NULL = testo
 * libero); `tipo` guida la validazione: 't' testo, 'i' intero, 'd' decimale, 'f' file
 * che deve esistere, 'x' eseguibile, 'u' URL o file. */
typedef struct {
    const char *chiave, *sezione, *etichetta, *aiuto, *scelte;
    char tipo;
    const char *predefinito;
} doppia_voce;

extern const doppia_voce DOPPIA_VOCI[];
extern const int DOPPIA_N_VOCI;

typedef struct {
    char **val;                /* un valore (malloc'd) per voce, nell'ordine di DOPPIA_VOCI */
    char dir[1024];            /* cartella del lavoro */
} doppia_conf;

void doppia_conf_init(doppia_conf *c);            /* predefiniti, poi ~/.ds4/doppia.conf */
void doppia_conf_free(doppia_conf *c);
const char *doppia_get(const doppia_conf *c, const char *chiave);
long doppia_get_long(const doppia_conf *c, const char *chiave);
double doppia_get_double(const doppia_conf *c, const char *chiave);
bool doppia_is(const doppia_conf *c, const char *chiave, const char *valore);
/* Imposta dopo aver validato (tipo e scelte); false con err se il valore non va.
 * doppia_set controlla anche che file ed eseguibili esistano; la lettura di un file
 * no (un disco non montato non deve cancellare una scelta): lo fa doppia_verifica,
 * prima di partire, solo per quello che il lavoro usera'. */
bool doppia_set(doppia_conf *c, const char *chiave, const char *valore, char *err, size_t err_len);
bool doppia_verifica(const doppia_conf *c, char *err, size_t err_len);
/* chiave = valore per riga, '#' commenta; le chiavi sconosciute sono ignorate (con
 * avviso). Il file del lavoro si scrive intero, con l'aiuto di ogni voce. */
bool doppia_conf_load(doppia_conf *c, const char *path, char *err, size_t err_len);
bool doppia_conf_save(const doppia_conf *c, const char *path, char *err, size_t err_len);

/* Il wizard: riepilogo per sezioni, numero = modifica una voce, t = righe del titolo,
 * p = salva come predefiniti, Invio = parti, q = esci. false se l'utente esce. */
bool doppia_wizard(doppia_conf *c, const char *titoli_path);

/* Esegue le fasi mancanti. cancel (Ctrl+C) interrompe tra un passo e l'altro e ferma
 * il programma esterno in corso. */
typedef bool (*doppia_cancel_fn)(void *privdata);
bool doppia_run(doppia_conf *c, doppia_cancel_fn cancel, void *privdata, char *err, size_t err_len);

/* Parti pure, esposte per i test. */
typedef struct { double da, a; char *en, *it; } doppia_frase;
/* parole.csv di whisper (-ml 1 -sow -ocsv) -> frasi: si chiude a una punteggiatura
 * dopo almeno min_s, mai oltre max_s. */
int doppia_frasi_da_csv(const char *csv, double min_s, double max_s, doppia_frase **out);
bool doppia_frasi_save(const char *path, const doppia_frase *f, int n);
int doppia_frasi_load(const char *path, doppia_frase **out);
void doppia_frasi_free(doppia_frase *f, int n);
/* La mappa delle inquadrature (posizione.txt): una riga per tratto,
 *   da a cerchio CX CY R | da a riquadro X Y W H | da a intero | da a vuoto
 * Il formato vecchio di una riga ("cerchio CX CY R" o "intero") vale per tutto il video.
 * Le righe si leggono ordinate, senza buchi ne' sovrapposizioni oltre la durata. */
typedef enum { SEG_VUOTO = 0, SEG_CERCHIO, SEG_RIQUADRO, SEG_INTERO } doppia_tipo_seg;
typedef struct { double da, a; doppia_tipo_seg tipo; int x, y, w, h; } doppia_segmento;
int doppia_posizione_load(const char *path, double durata, doppia_segmento **out);
/* Il filtro ffmpeg della scena: ingressi 0 video originale, 1 testa parlante, 2 voce,
 * poi una maschera PNG per ogni cerchio di raggio diverso (nomi in maschere, lati 2R).
 * Una sovrapposizione per geometria, attiva solo nei suoi tratti. malloc'd. */
char *doppia_filtro_scena(const doppia_segmento *s, int n, int W, int H, double volume,
                          int *raggi, int *n_raggi);
/* Piano dei pezzi H3 su una durata: inizio (s) e fotogrammi (17k+5) di ciascuno. */
typedef struct { double inizio; int fotogrammi; bool ancora_fine; } doppia_pezzo;
int doppia_piano_pezzi(double durata, doppia_pezzo **out);

/* Processi e montaggio (ds4_media_doppia_proc.c). argv termina con NULL; l'uscita del
 * programma va nel log del lavoro. Con cancel il programma si ferma (SIGTERM al suo
 * gruppo) e si torna 130. Restituisce lo stato di uscita, -1 se non parte. */
int doppia_esegui(const char *const *argv, const char *log, doppia_cancel_fn cancel, void *privdata);
/* Gruppo di processi del programma esterno in corso, 0 se nessuno (per i segnali). */
#include <sys/types.h>
extern volatile pid_t doppia_figlio;
/* Uscita (stdout+stderr, malloc'd) di un programma, NULL se non parte. */
char *doppia_cattura(const char *const *argv);
/* Cartella del lavoro, doppia.conf e titoli.txt: prima si caricano il doppia.conf del
 * lavoro (se esiste) e poi `override` (coppie chiave, valore, NULL), cosi' la riga di
 * comando vince sul file. */
bool doppia_prepara(doppia_conf *c, const char *const *override, char *err, size_t err_len);
const char *doppia_titoli_path(const doppia_conf *c);
/* Durata in secondi (da ffmpeg -i), dimensioni se w/h non NULL; <0 se illeggibile. */
double doppia_durata(const char *ffmpeg, const char *file, int *w, int *h);
/* ffmpeg che scrive su out.tmp.mp4 e rinomina solo se riuscito (argv con due posti
 * liberi dopo argc: il file temporaneo e il NULL). */
bool doppia_ffmpeg(const char **argv, int argc, const char *out, const char *log,
                   doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
/* testa.mp4: i pezzi uniti (dal secondo senza il primo fotogramma), ciascuno portato ai
 * suoi fotogrammi previsti, tagliati a durata, con la voce normalizzata. */
bool doppia_monta_testa(const doppia_conf *c, char **pezzi, const int *fotogrammi, int n, double durata,
                        const char *voce, const char *out, const char *log,
                        doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
/* scena.mp4: la testa parlante messa, tratto per tratto, dove la mappa delle
 * inquadrature dice (cerchio, riquadro, tutto schermo; niente nei tratti vuoti), con
 * la voce normalizzata. */
bool doppia_monta_scena(const doppia_conf *c, const char *sorgente, const char *testa,
                        const doppia_segmento *seg, int nseg, const char *voce, const char *out,
                        const char *log, doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
/* Titolo di testa da titoli.txt davanti a `in`, o copia se il file dice mostra: no. */
bool doppia_titolo(const doppia_conf *c, const char *aiuti, const char *in, const char *titoli,
                   const char *out, const char *log, doppia_cancel_fn cancel, void *pd,
                   char *err, size_t err_len);

/* Fasi (ds4_media_doppia_fasi.c), usate da doppia_run. */
const char *doppia_aiuti(const doppia_conf *c);
bool doppia_servizio_su(const char *host, int port, const char *path);
int doppia_traduttore(const doppia_conf *c, char *host, size_t n);
bool doppia_accendi(doppia_conf *c, const char *riga, const char *host, int port, const char *path,
                    const char *cosa, const char *log, doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
/* lingua.txt: il codice della lingua parlata (whisper), da lingua o riconosciuto. */
bool doppia_fase_lingua(doppia_conf *c, const char *a16, const char *log, doppia_cancel_fn cancel, void *pd,
                        char *lingua, size_t n, char *err, size_t err_len);
/* voce_it.wav per conversione (stessa lingua) o sintesi (traduzione). */
bool doppia_fase_voce(doppia_conf *c, const char *lingua, double durata, const char *log,
                      doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
bool doppia_fase_posizione(doppia_conf *c, double durata, const char *log, doppia_cancel_fn cancel, void *pd,
                           doppia_segmento **seg, int *nseg, char *err, size_t err_len);
bool doppia_fase_pezzi(doppia_conf *c, double durata, const char *log, doppia_cancel_fn cancel, void *pd,
                       char *err, size_t err_len);
bool doppia_titolo_se_serve(const doppia_conf *c, const char *in, const char *out, const char *log,
                            doppia_cancel_fn cancel, void *pd, char *err, size_t err_len);
/* Il file ha una traccia audio? (whisper e la voce ne hanno bisogno) */
bool doppia_ha_audio(const char *ffmpeg, const char *file);

/* Traduzione (ds4_media_trad.c): riempie `it` delle frasi che non lo hanno, a blocchi,
 * salvando dopo ogni blocco. */
bool doppia_traduci(doppia_conf *c, doppia_frase *f, int n, const char *salva_in,
                    doppia_cancel_fn cancel, void *privdata, char *err, size_t err_len);
/* "http://host:porta" -> host e porta (80 senza porta); false per https o forme non
 * valide. La usano la traduzione e il driver, cosi' leggono l'URL nello stesso modo. */
bool doppia_url(const char *url, char *host, size_t hn, int *port);
/* Chi parla dice chi e' o che lavoro fa (nome, professione, dove insegna...)? Dal
 * testo originale e dalla traduzione; queste frasi passano in terza persona. */
bool doppia_parla_di_se(const char *en, const char *it);
/* Legge un array JSON di stringhe dentro un testo qualsiasi (tra il primo '[' e
 * l'ultimo ']'). Numero di stringhe, -1 se non e' un array di sole stringhe. */
int doppia_json_array(const char *testo, char ***out);

#endif
