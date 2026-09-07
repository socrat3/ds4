/* Prefill and decode must produce the same logits for the same prefix.
 *
 * The qwen35 graph runs one code path for both: `qwen35_graph_forward_tokens`
 * with n > 1 is prefill, with n == 1 is decode.  What differs between them is
 * everything that carries state forward -- the Gated DeltaNet convolution and
 * recurrent state, and the KV cache -- and that carry is the only part of the
 * graph a kernel-level test cannot check, because it is not inside any kernel.
 *
 * So this is the test that closes the gap.  The same token sequence is fed two
 * ways:
 *
 *   A:  sync(seq[0..N])                          -- every token through prefill
 *   B:  sync(seq[0..N-1]) then eval(seq[N-1])    -- the last one through decode
 *
 * Both leave the model looking at the same prefix, so the logits must agree.
 * They will not agree bit for bit: the two paths reduce in different orders and
 * use different kernels, which is worth a few 1e-3 on a 27B model in three-bit
 * quantization.  What must agree is the DISTRIBUTION -- the same argmax, the
 * same top of the list, and a distance in the noise rather than in the units.
 *
 * A real disagreement here means the state a decode step inherits is not the
 * state prefill would have left, and that is exactly the shape of a defect that
 * keeps the first tokens right (they come from prefill) and corrupts what
 * follows.
 *
 * Run with:
 *   DS4_TEST_MODEL=/path/to/model.gguf make test-qwen35-prefill-decode
 */

#include "ds4.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CTX 2048


/* Long enough that the recurrent state has really been driven, and prose
 * rather than a list so the distribution is not dominated by one token. */
static const char *TESTO =
    "L'articolo 109 del Testo unico delle imposte sui redditi stabilisce i "
    "criteri generali di imputazione temporale dei componenti positivi e "
    "negativi del reddito d'impresa. I ricavi delle cessioni di beni mobili si "
    "considerano conseguiti alla data della consegna o della spedizione, "
    "mentre per gli immobili e per le aziende rileva la data di stipulazione "
    "dell'atto. Le prestazioni di servizi si considerano conseguite alla data "
    "in cui le prestazioni sono ultimate. Per le prestazioni da cui derivano "
    "corrispettivi periodici rileva invece la data di maturazione dei "
    "corrispettivi. La stessa disposizione fissa il principio secondo cui i "
    "componenti concorrono a formare il reddito nell'esercizio di competenza, "
    "sempre che la loro esistenza sia certa e il loro ammontare determinabile "
    "in modo obiettivo; altrimenti concorrono nell'esercizio in cui tali "
    "condizioni si verificano. La deducibilita' dei costi resta poi "
    "subordinata alla loro imputazione al conto economico, salvo i casi in cui";

typedef struct {
    double max_diff;
    double rms;
    int argmax_a;
    int argmax_b;
    int top5_uguali;
} confronto;

static void indici_top(const float *l, int n, int *out, int k) {
    for (int i = 0; i < k; i++) {
        int best = -1;
        for (int j = 0; j < n; j++) {
            bool preso = false;
            for (int t = 0; t < i; t++) if (out[t] == j) { preso = true; break; }
            if (preso) continue;
            if (best < 0 || l[j] > l[best]) best = j;
        }
        out[i] = best;
    }
}

static confronto confronta(const float *a, const float *b, int n) {
    confronto c = {0};
    double somma = 0.0;
    for (int i = 0; i < n; i++) {
        const double d = fabs((double)a[i] - (double)b[i]);
        if (d > c.max_diff) c.max_diff = d;
        somma += d * d;
    }
    c.rms = sqrt(somma / (double)n);
    int ta[5], tb[5];
    indici_top(a, n, ta, 5);
    indici_top(b, n, tb, 5);
    c.argmax_a = ta[0];
    c.argmax_b = tb[0];
    for (int i = 0; i < 5; i++) if (ta[i] == tb[i]) c.top5_uguali++;
    return c;
}

int main(void) {
    const char *model = getenv("DS4_TEST_MODEL");
    if (!model || !model[0]) {
        fprintf(stderr, "SKIP: serve DS4_TEST_MODEL=/percorso/modello.gguf\n");
        return 0;
    }

    char err[512] = {0};
    ds4_engine_options opt = {
        .model_path = model,
        .backend = DS4_BACKEND_CUDA,
        .n_threads = 1,
        .placement_ctx_hint = TEST_CTX,
    };
    ds4_engine *engine = NULL;
    if (ds4_engine_open(&engine, &opt) != 0) {
        fprintf(stderr, "FAIL: apertura del motore\n");
        return 1;
    }
    if (!ds4_engine_is_qwen35(engine)) {
        fprintf(stderr, "SKIP: il modello caricato non e' qwen35\n");
        ds4_engine_close(engine);
        return 0;
    }

    ds4_tokens tutti = {0};
    ds4_tokenize_text(engine, TESTO, &tutti);
    if (tutti.len < 8) {
        fprintf(stderr, "FAIL: il testo di prova produce %d token\n", tutti.len);
        return 1;
    }
    const int n = tutti.len;
    printf("prova su %d token\n", n);

    const int vocab = ds4_engine_vocab_size(engine);
    float *la = (float *)malloc((size_t)vocab * sizeof(float));
    float *lb = (float *)malloc((size_t)vocab * sizeof(float));
    if (!la || !lb) {
        fprintf(stderr, "FAIL: allocazione\n");
        return 1;
    }

    /* A: tutto dal prefill. */
    ds4_session *a = NULL;
    if (ds4_session_create(&a, engine, TEST_CTX) != 0 ||
        ds4_session_sync(a, &tutti, err, sizeof(err)) != 0) {
        fprintf(stderr, "FAIL: sessione A: %s\n", err);
        return 1;
    }
    if (ds4_session_copy_logits(a, la, vocab) != vocab) {
        fprintf(stderr, "FAIL: logit di A\n");
        return 1;
    }

    /* B: tutto tranne l'ultimo dal prefill, l'ultimo dal decode. */
    ds4_tokens testa = {0};
    for (int i = 0; i < n - 1; i++) ds4_tokens_push(&testa, tutti.v[i]);
    ds4_session *b = NULL;
    if (ds4_session_create(&b, engine, TEST_CTX) != 0 ||
        ds4_session_sync(b, &testa, err, sizeof(err)) != 0) {
        fprintf(stderr, "FAIL: sessione B: %s\n", err);
        return 1;
    }
    if (ds4_session_eval(b, tutti.v[n - 1], err, sizeof(err)) != 0) {
        fprintf(stderr, "FAIL: decode di B: %s\n", err);
        return 1;
    }
    if (ds4_session_copy_logits(b, lb, vocab) != vocab) {
        fprintf(stderr, "FAIL: logit di B\n");
        return 1;
    }

    const confronto c = confronta(la, lb, vocab);
    printf("un passo di decode contro prefill: max %.6g  rms %.6g  "
           "argmax %d/%d  top5 in comune %d/5\n",
           c.max_diff, c.rms, c.argmax_a, c.argmax_b, c.top5_uguali);

    /* E con PIU' passi di decode, che e' la condizione vera della generazione:
     * un errore che si accumula non si vede in un passo solo. */
    ds4_tokens testa2 = {0};
    const int passi = n > 24 ? 8 : 2;
    for (int i = 0; i < n - passi; i++) ds4_tokens_push(&testa2, tutti.v[i]);
    ds4_session *d = NULL;
    if (ds4_session_create(&d, engine, TEST_CTX) != 0 ||
        ds4_session_sync(d, &testa2, err, sizeof(err)) != 0) {
        fprintf(stderr, "FAIL: sessione D: %s\n", err);
        return 1;
    }
    for (int i = n - passi; i < n; i++) {
        if (ds4_session_eval(d, tutti.v[i], err, sizeof(err)) != 0) {
            fprintf(stderr, "FAIL: decode %d di D: %s\n", i, err);
            return 1;
        }
    }
    if (ds4_session_copy_logits(d, lb, vocab) != vocab) {
        fprintf(stderr, "FAIL: logit di D\n");
        return 1;
    }
    const confronto c2 = confronta(la, lb, vocab);
    printf("%d passi di decode contro prefill: max %.6g  rms %.6g  "
           "argmax %d/%d  top5 in comune %d/5\n",
           passi, c2.max_diff, c2.rms, c2.argmax_a, c2.argmax_b, c2.top5_uguali);

    /* La soglia non e' zero e non e' arbitraria: mmq e mmvq sui pesi di questo
     * modello si discostano di ~4e-4 RMS (misurato con DS4_QWEN35_MM_CHECK), e
     * prefill e decode usano proprio quei due.  Un ordine di grandezza sopra
     * quel rumore resta rumore; due ordini sopra e' un difetto. */
    const double soglia_rms = 0.05;
    int esito = 0;
    if (c.argmax_a != c.argmax_b || c2.argmax_a != c2.argmax_b) {
        fprintf(stderr, "FAIL: prefill e decode non scelgono lo stesso token\n");
        esito = 1;
    }
    if (c.rms > soglia_rms || c2.rms > soglia_rms) {
        fprintf(stderr, "FAIL: distribuzioni distanti (rms %.6g e %.6g, soglia %.3g)\n",
                c.rms, c2.rms, soglia_rms);
        esito = 1;
    }
    if (c2.top5_uguali < 4) {
        fprintf(stderr, "FAIL: il vertice della distribuzione si riordina "
                        "(%d/5 in comune dopo %d passi)\n", c2.top5_uguali, passi);
        esito = 1;
    }

    free(la);
    free(lb);
    ds4_tokens_free(&tutti);
    ds4_tokens_free(&testa);
    ds4_tokens_free(&testa2);
    ds4_session_free(d);
    ds4_session_free(b);
    ds4_session_free(a);
    ds4_engine_close(engine);
    puts(esito ? "prefill contro decode: FALLITO" : "prefill contro decode: PASS");
    return esito;
}
