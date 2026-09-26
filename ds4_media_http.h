#ifndef DS4_MEDIA_HTTP_H
#define DS4_MEDIA_HTTP_H

/* Client HTTP minimo per ds4_media: POST/GET verso host:porta con Content-Length e
 * chunked, multipart per /upload/image. Interno al modulo (prefisso media_http_).
 * Modellato su web_tcp_connect/web_read_some di ds4_web.c, ma con host+porta e POST. */

#include <stdbool.h>
#include <stddef.h>

/* Buffer che cresce, condiviso dai file del modulo. */
typedef struct { char *ptr; size_t len, cap; } media_buf;
void media_buf_append(media_buf *b, const char *s, size_t n);
void media_buf_puts(media_buf *b, const char *s);
char *media_buf_take(media_buf *b);   /* restituisce il buffer (sempre \0-terminato) e lo azzera */

void *media_xmalloc(size_t n);
char *media_xstrdup(const char *s);
void media_set_err(char *err, size_t err_len, const char *fmt, ...);

/* Connessione TCP con timeout di connect (3 s); -1 con err che suggerisce di accendere
 * ComfyUI. Scrittura completa (EINTR e scritture parziali) su socket o file, 0 o -1.
 * Usate anche dal client websocket, dal serve e per salvare i file. */
int media_tcp_connect(const char *host, int port, char *err, size_t err_len);
int media_write_all(int fd, const void *buf, size_t len);

/* Annullamento per le richieste fatte da questo thread: se fn(privdata) diventa true
 * durante un'attesa, la richiesta fallisce con "annullato" entro ~250 ms. NULL lo
 * toglie. ds4_media lo imposta per la durata di un lavoro. */
typedef bool (*media_http_cancel_fn)(void *privdata);
void media_http_set_cancel(media_http_cancel_fn fn, void *privdata);

/* Risposta HTTP: stato, corpo (malloc'd, \0-terminato ma binario-safe con body_len). */
typedef struct { int status; char *body; size_t body_len; } media_http_response;
void media_http_response_free(media_http_response *r);

/* GET e POST. `content_type` e `body` per il POST (body puo' essere binario, body_len>0).
 * timeout_ms per l'intera risposta. Su true, *resp e' riempito; su false err spiega. */
bool media_http_get(const char *host, int port, const char *path, int timeout_ms,
                    media_http_response *resp, char *err, size_t err_len);
bool media_http_post(const char *host, int port, const char *path,
                     const char *content_type, const void *body, size_t body_len,
                     int timeout_ms, media_http_response *resp, char *err, size_t err_len);

/* POST multipart con un solo campo file (per /upload/image) piu' campi testo.
 * fields = coppie {nome, valore} terminate da NULL; file_field/file_name/file_bytes/file_len
 * il file. */
bool media_http_post_image(const char *host, int port, const char *path,
                           const char *const *fields, const char *file_field,
                           const char *file_name, const void *file_bytes, size_t file_len,
                           int timeout_ms, media_http_response *resp, char *err, size_t err_len);

/* Decodifica la stringa JSON che inizia in p (sulla virgoletta): malloc'd, NULL se p
 * non e' una virgoletta. *end (se non NULL) punta dopo la virgoletta di chiusura, o al
 * punto in cui la stringa si e' interrotta (troncata o malformata). */
char *media_json_parse_string(const char *p, const char **end);
/* Estrae il valore stringa di "key" dal JSON (prima occorrenza), malloc'd o NULL. */
char *media_json_str(const char *json, const char *key);
/* Valore intero di "key" (found=false se assente). */
long media_json_int(const char *json, const char *key, bool *found);
/* Cita una stringa come letterale JSON tra virgolette (malloc'd). */
char *media_json_quote(const char *s);
/* Percent-encoding per un valore di query (malloc'd). */
char *media_url_encode(const char *s);
/* base64 standard (malloc'd, \0-terminato). */
char *media_base64_encode(const unsigned char *data, size_t len);

#endif
