#ifndef DS4_MEDIA_HELP_H
#define DS4_MEDIA_HELP_H

/* Aiuto di ds4-media (ds4_media_help.c): generale con cmd NULL o "", altrimenti del
 * comando. 0 se l'argomento esiste, 2 (con un messaggio su stderr) se no. */

#include <stdio.h>

int ds4_media_help(FILE *o, const char *cmd);

#endif
