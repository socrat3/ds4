#ifndef DS4_MEDIA_WS_H
#define DS4_MEDIA_WS_H

/* Client WebSocket minimo (RFC 6455) per ascoltare gli eventi di ComfyUI su
 * /ws?clientId=...: progresso passo per passo, inizio e fine di un lavoro, anteprime
 * binarie. Solo lettura, piu' i pong ai ping e il close. Interno al modulo. */

#include <stdbool.h>
#include <stddef.h>

typedef struct media_ws media_ws;

/* Apre /ws?clientId=<client_id>. NULL (con err) se ComfyUI non fa l'upgrade: il
 * chiamante ripiega sul polling di /history. */
media_ws *media_ws_open(const char *host, int port, const char *client_id,
                        char *err, size_t err_len);

/* Aspetta un messaggio completo per al massimo timeout_ms.
 *   1  messaggio in *msg (malloc'd, \0-terminato), lunghezza *len, *binary se binario
 *   0  nessun messaggio entro il timeout
 *  -1  connessione chiusa o protocollo violato: il chiamante chiude e ripiega. */
int media_ws_read(media_ws *ws, int timeout_ms, char **msg, size_t *len, bool *binary);

void media_ws_close(media_ws *ws);

/* Massimo di un messaggio (anche riassemblato dai frammenti): le anteprime di ComfyUI
 * sono JPEG di poche centinaia di KiB, i JSON di eventi pochi KiB. */
#define MEDIA_WS_MAX_MSG (16u * 1024 * 1024)

#endif
