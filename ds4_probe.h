/* ds4_probe.h - analisi interna del modello, lato CLI (la cattura del residuo sta in ds4_probe.inc).
 *
 *   DS4_PROBE_TOKENS=FILE  una riga "pos<TAB>testo" per ogni token del prompt e generato, con le
 *                          stesse posizioni dei record di DS4_PROBE_OUT; \\ \n \t \r protetti.
 *   Con DS4_PROBE_OUT la CLI stampa su stderr "ds4: probe prompt_tokens=N".
 * Incluso solo da ds4_cli.c. */
#ifndef DS4_PROBE_H
#define DS4_PROBE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *ds4_probe_tok_fp;
static int ds4_probe_tok_pos;

static inline void ds4_probe_tokens_open(void) {
    const char *p = getenv("DS4_PROBE_TOKENS");
    if (!ds4_probe_tok_fp && p && p[0] && !(ds4_probe_tok_fp = fopen(p, "w")))
        fprintf(stderr, "ds4: cannot open DS4_PROBE_TOKENS %s\n", p);
}

static inline bool ds4_probe_tokens_active(void) { return ds4_probe_tok_fp != NULL; }

static inline void ds4_probe_tokens_write(const char *text, size_t len) {
    if (!ds4_probe_tok_fp) return;
    fprintf(ds4_probe_tok_fp, "%d\t", ds4_probe_tok_pos++);
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if (c == '\\') fputs("\\\\", ds4_probe_tok_fp);
        else if (c == '\n') fputs("\\n", ds4_probe_tok_fp);
        else if (c == '\t') fputs("\\t", ds4_probe_tok_fp);
        else if (c == '\r') fputs("\\r", ds4_probe_tok_fp);
        else fputc(c, ds4_probe_tok_fp);
    }
    fputc('\n', ds4_probe_tok_fp);
    fflush(ds4_probe_tok_fp);
}

static inline void ds4_probe_prompt_tokens(int n) {
    if (getenv("DS4_PROBE_OUT")) fprintf(stderr, "ds4: probe prompt_tokens=%d\n", n);
}

#endif
