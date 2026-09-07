/* Qwen3.5 Gated DeltaNet on CUDA, checked against a scalar host reference.
 *
 * The host model below is the specification, written out in double precision so
 * a disagreement is the kernel's fault and not the reference's.  Each step was
 * taken from llama.cpp, which runs this architecture today and answers
 * correctly, rather than from either of the two community ports: those two
 * disagree with each other in three places, and every variant produces
 * plausible numbers, so reading them would have settled nothing.
 *
 *   1. causal depthwise conv over 4 taps on the concatenated q|k|v projection,
 *      then SiLU                        (llama.cpp: ggml_ssm_conv + ggml_silu)
 *   2. L2 normalisation of q and k      (qwen35.cpp: ggml_l2_norm, eps)
 *   3. a 1/sqrt(D) scale on q           (delta-net-base.cpp:45, and the fused
 *      kernel applies the same factor to the output instead, which is the same
 *      number: (S*q)*s == S*(q*s).  Missing this was the author's own first
 *      mistake, caught by writing this file.)
 *   4. per-value-head decay, ONE scalar per head:
 *          decay = exp(ssm_a[h] * softplus(alpha[h] + dt_bias[h]))
 *      with ssm_a used raw, because the GGUF tensor already holds -exp(A_log)
 *      (qwen35.cpp:379 says so in its comment).  This is where GLM differs:
 *      its decay is per channel.  In ggml the two are already one flag.
 *   5. beta = sigmoid(raw_beta[h])
 *   6. the delta rule:
 *          S *= decay;  u = S.k;  d = (v - u) * beta;  S += k (x) d;  o = S.q
 *   7. per-head RMSNorm with a sigmoid output gate
 *
 * Value head h reads the query and key of head h % n_k_heads: ggml repeats the
 * whole block cyclically (ggml_repeat_4d), so the mapping is interleaved and
 * not contiguous blocks.
 *
 * Checks: one decode step against the reference, a run of tokens against the
 * reference (which also exercises the recurrent state carried across steps),
 * and prefill against the same decode outputs.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "ds4.h"
#include "ds4_gpu.h"

bool ds4_log_is_tty(FILE *fp) {
    (void)fp;
    return false;
}

static int failures;

static void require_ok(int ok, const char *what) {
    if (ok) return;
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
}

/* Reports every mismatch rather than dying on the first: when a port is wrong
 * the pattern of the error says which step is wrong, and one value does not. */
static void require_close(const char *what, uint32_t idx,
                          float actual, double expected, double tolerance) {
    const double diff = fabs((double)actual - expected);
    if (diff <= tolerance) return;
    if (failures < 12) {
        fprintf(stderr, "FAIL: %s[%u]: got %.9g, want %.9g (diff %.3g > %.3g)\n",
                what, idx, (double)actual, expected, diff, tolerance);
    }
    failures++;
}

enum {
    D = 128,             /* head dimension: the kernel is written for 128 */
    K_HEADS = 4,
    V_HEADS = 12,        /* 3 value heads per key head, as 48 over 16 */
    CONV_K = 4,
    CONV_HISTORY = CONV_K - 1,
    QK_DIM = K_HEADS * D,
    V_DIM = V_HEADS * D,
    QKV_DIM = 2 * QK_DIM + V_DIM,
    TOKENS = 9,

    CONV_OFFSET = 0,                        /* [QKV_DIM][CONV_K] */
    SSM_A_OFFSET = 65536,                   /* [V_HEADS] */
    DT_BIAS_OFFSET = 65536 + 4096,          /* [V_HEADS] */
    NORM_OFFSET = 65536 + 8192,             /* [D] */
    MODEL_BYTES = 131072,
};

static double host_silu(double x) {
    return x / (1.0 + exp(-x));
}

static double host_sigmoid(double x) {
    return 1.0 / (1.0 + exp(-x));
}

/* Guarded at both ends exactly as the reference implementations are: without
 * the upper guard exp() overflows before log1p() can bring it back, and the
 * decay becomes inf instead of a number near the argument. */
static double host_softplus(double x) {
    if (x > 20.0) return x;
    if (x < -20.0) return exp(x);
    return log(1.0 + exp(x));
}

typedef struct {
    double conv[QKV_DIM][CONV_K];   /* weights */
    double history[QKV_DIM][CONV_HISTORY];
    double state[V_HEADS][D][D];    /* state[h][i][j], i keyed, j valued */
    double ssm_a[V_HEADS];
    double dt_bias[V_HEADS];
    double norm[D];
} host_gdn;

/* One decode step of the specification. `out` receives V_HEADS * D values. */
static void host_step(host_gdn *g,
                      const float *qkv,
                      const float *raw_alpha,
                      const float *raw_beta,
                      const float *output_gate,
                      double norm_eps,
                      double *out) {
    double mixed[QKV_DIM];
    for (uint32_t c = 0; c < QKV_DIM; c++) {
        double acc = 0.0;
        for (uint32_t w = 0; w < CONV_HISTORY; w++) {
            acc += g->history[c][w] * g->conv[c][w];
        }
        acc += (double)qkv[c] * g->conv[c][CONV_K - 1];
        for (uint32_t w = 0; w + 1 < CONV_HISTORY; w++) {
            g->history[c][w] = g->history[c][w + 1];
        }
        g->history[c][CONV_HISTORY - 1] = (double)qkv[c];
        mixed[c] = host_silu(acc);
    }

    const double *q_all = mixed;
    const double *k_all = mixed + QK_DIM;
    const double *v_all = mixed + 2 * QK_DIM;

    /* L2 normalisation, then the 1/sqrt(D) query scale */
    double qn[K_HEADS][D], kn[K_HEADS][D];
    const double q_scale = 1.0 / sqrt((double)D);
    for (uint32_t kh = 0; kh < K_HEADS; kh++) {
        double q_sumsq = 0.0, k_sumsq = 0.0;
        for (uint32_t d = 0; d < D; d++) {
            const double qv = q_all[kh * D + d];
            const double kv = k_all[kh * D + d];
            q_sumsq += qv * qv;
            k_sumsq += kv * kv;
        }
        const double qi = 1.0 / sqrt(q_sumsq + 1.0e-6);
        const double ki = 1.0 / sqrt(k_sumsq + 1.0e-6);
        for (uint32_t d = 0; d < D; d++) {
            qn[kh][d] = q_all[kh * D + d] * qi * q_scale;
            kn[kh][d] = k_all[kh * D + d] * ki;
        }
    }

    for (uint32_t h = 0; h < V_HEADS; h++) {
        const uint32_t kh = h % K_HEADS;     /* interleaved, not blocked */
        const double decay = exp(g->ssm_a[h] *
            host_softplus((double)raw_alpha[h] + g->dt_bias[h]));
        const double beta = host_sigmoid((double)raw_beta[h]);

        double u[D];
        for (uint32_t j = 0; j < D; j++) {
            double sum = 0.0;
            for (uint32_t i = 0; i < D; i++) {
                g->state[h][i][j] *= decay;
                sum += g->state[h][i][j] * kn[kh][i];
            }
            u[j] = sum;
        }
        double head_out[D];
        for (uint32_t j = 0; j < D; j++) {
            const double delta = (v_all[h * D + j] - u[j]) * beta;
            double sum = 0.0;
            for (uint32_t i = 0; i < D; i++) {
                g->state[h][i][j] += kn[kh][i] * delta;
                sum += g->state[h][i][j] * qn[kh][i];
            }
            head_out[j] = sum;
        }

        /* per-head RMSNorm with a sigmoid output gate */
        double sumsq = 0.0;
        for (uint32_t j = 0; j < D; j++) sumsq += head_out[j] * head_out[j];
        const double inv = 1.0 / sqrt(sumsq / (double)D + norm_eps);
        for (uint32_t j = 0; j < D; j++) {
            out[h * D + j] = head_out[j] * inv * g->norm[j] *
                             host_sigmoid((double)output_gate[h * D + j]);
        }
    }
}

int main(void) {
    uint8_t *model = mmap(NULL, MODEL_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (model == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    float *conv_w = (float *)(model + CONV_OFFSET);
    float *ssm_a = (float *)(model + SSM_A_OFFSET);
    float *dt_bias = (float *)(model + DT_BIAS_OFFSET);
    float *norm_w = (float *)(model + NORM_OFFSET);

    /* Deliberately not a clean identity: an identity convolution hides a wrong
     * tap order, and a flat decay hides a wrong head mapping. */
    for (uint32_t c = 0; c < QKV_DIM; c++) {
        for (uint32_t w = 0; w < CONV_K; w++) {
            conv_w[c * CONV_K + w] =
                0.35f - 0.05f * (float)w + 0.001f * (float)((c + w) % 19u);
        }
    }
    for (uint32_t h = 0; h < V_HEADS; h++) {
        /* ssm_a is already -exp(A_log) in a real checkpoint, so it is negative
         * and differs per head: a wrong head mapping shows up as a wrong decay. */
        ssm_a[h] = -0.6f - 0.11f * (float)h;
        dt_bias[h] = -0.2f + 0.07f * (float)h;
    }
    for (uint32_t d = 0; d < D; d++) {
        norm_w[d] = 0.8f + 0.003f * (float)d;
    }

    host_gdn ref;
    memset(&ref, 0, sizeof(ref));
    for (uint32_t c = 0; c < QKV_DIM; c++) {
        for (uint32_t w = 0; w < CONV_K; w++) ref.conv[c][w] = conv_w[c * CONV_K + w];
    }
    for (uint32_t h = 0; h < V_HEADS; h++) {
        ref.ssm_a[h] = ssm_a[h];
        ref.dt_bias[h] = dt_bias[h];
    }
    for (uint32_t d = 0; d < D; d++) ref.norm[d] = norm_w[d];

    static float qkv[TOKENS][QKV_DIM];
    static float alpha[TOKENS][V_HEADS];
    static float beta[TOKENS][V_HEADS];
    static float ogate[TOKENS][V_DIM];
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t c = 0; c < QKV_DIM; c++) {
            qkv[t][c] = 0.12f - 0.0031f * (float)((c + 5u * t) % 29u)
                      + 0.02f * (float)t;
        }
        for (uint32_t h = 0; h < V_HEADS; h++) {
            alpha[t][h] = -0.3f + 0.09f * (float)h + 0.05f * (float)t;
            beta[t][h]  =  0.4f - 0.07f * (float)h + 0.03f * (float)t;
        }
        for (uint32_t i = 0; i < V_DIM; i++) {
            ogate[t][i] = 0.2f - 0.0017f * (float)(i % 23u) + 0.01f * (float)t;
        }
    }

    const float norm_eps = 1.0e-5f;
    static double expected[TOKENS][V_DIM];
    for (uint32_t t = 0; t < TOKENS; t++) {
        host_step(&ref, qkv[t], alpha[t], beta[t], ogate[t],
                  (double)norm_eps, expected[t]);
    }

    require_ok(ds4_gpu_init(), "CUDA init");

    ds4_gpu_tensor *g_qkv = ds4_gpu_tensor_alloc(QKV_DIM * sizeof(float));
    ds4_gpu_tensor *g_alpha = ds4_gpu_tensor_alloc(V_HEADS * sizeof(float));
    ds4_gpu_tensor *g_beta = ds4_gpu_tensor_alloc(V_HEADS * sizeof(float));
    ds4_gpu_tensor *g_ogate = ds4_gpu_tensor_alloc(V_DIM * sizeof(float));
    ds4_gpu_tensor *g_out = ds4_gpu_tensor_alloc(V_DIM * sizeof(float));
    ds4_gpu_tensor *g_conv = ds4_gpu_tensor_alloc(CONV_HISTORY * QKV_DIM * sizeof(float));
    ds4_gpu_tensor *g_state =
        ds4_gpu_tensor_alloc((uint64_t)V_HEADS * D * D * sizeof(float));
    require_ok(g_qkv && g_alpha && g_beta && g_ogate && g_out && g_conv && g_state,
               "decode tensor allocation");
    require_ok(ds4_gpu_tensor_fill_f32(g_conv, 0.0f, CONV_HISTORY * QKV_DIM), "conv clear");
    require_ok(ds4_gpu_tensor_fill_f32(g_state, 0.0f, (uint64_t)V_HEADS * D * D), "state clear");

    static float decode_out[TOKENS][V_DIM];
    for (uint32_t t = 0; t < TOKENS; t++) {
        require_ok(ds4_gpu_tensor_write(g_qkv, 0, qkv[t], sizeof(qkv[t])), "qkv write");
        require_ok(ds4_gpu_tensor_write(g_alpha, 0, alpha[t], sizeof(alpha[t])), "alpha write");
        require_ok(ds4_gpu_tensor_write(g_beta, 0, beta[t], sizeof(beta[t])), "beta write");
        require_ok(ds4_gpu_tensor_write(g_ogate, 0, ogate[t], sizeof(ogate[t])), "gate write");
        require_ok(ds4_gpu_qwen35_gdn_decode(
            g_out, g_conv, g_state, g_qkv, g_alpha, g_beta, g_ogate,
            model, MODEL_BYTES, CONV_OFFSET, SSM_A_OFFSET, DT_BIAS_OFFSET, NORM_OFFSET,
            K_HEADS, V_HEADS, 1, norm_eps), "qwen35 GDN decode");
        require_ok(ds4_gpu_tensor_read(g_out, 0, decode_out[t], sizeof(decode_out[t])),
                   "decode output read");
        for (uint32_t i = 0; i < V_DIM; i++) {
            require_close(t == 0 ? "first decode" : "decode", i,
                          decode_out[t][i], expected[t][i], 3.0e-4);
        }
        if (failures) {
            fprintf(stderr, "stopping at token %u with %d mismatches\n", t, failures);
            return 1;
        }
    }
    printf("decode vs host reference over %d tokens: PASS\n", TOKENS);

    /* Prefill must agree with the token-by-token path it replaces. */
    ds4_gpu_tensor *p_qkv = ds4_gpu_tensor_alloc(sizeof(qkv));
    ds4_gpu_tensor *p_alpha = ds4_gpu_tensor_alloc(sizeof(alpha));
    ds4_gpu_tensor *p_beta = ds4_gpu_tensor_alloc(sizeof(beta));
    ds4_gpu_tensor *p_ogate = ds4_gpu_tensor_alloc(sizeof(ogate));
    ds4_gpu_tensor *p_out = ds4_gpu_tensor_alloc(sizeof(decode_out));
    ds4_gpu_tensor *p_conv = ds4_gpu_tensor_alloc(CONV_HISTORY * QKV_DIM * sizeof(float));
    ds4_gpu_tensor *p_state =
        ds4_gpu_tensor_alloc((uint64_t)V_HEADS * D * D * sizeof(float));
    require_ok(p_qkv && p_alpha && p_beta && p_ogate && p_out && p_conv && p_state,
               "prefill tensor allocation");
    require_ok(ds4_gpu_tensor_write(p_qkv, 0, qkv, sizeof(qkv)), "prefill qkv write");
    require_ok(ds4_gpu_tensor_write(p_alpha, 0, alpha, sizeof(alpha)), "prefill alpha write");
    require_ok(ds4_gpu_tensor_write(p_beta, 0, beta, sizeof(beta)), "prefill beta write");
    require_ok(ds4_gpu_tensor_write(p_ogate, 0, ogate, sizeof(ogate)), "prefill gate write");
    require_ok(ds4_gpu_tensor_fill_f32(p_conv, 0.0f, CONV_HISTORY * QKV_DIM), "prefill conv clear");
    require_ok(ds4_gpu_tensor_fill_f32(p_state, 0.0f, (uint64_t)V_HEADS * D * D),
               "prefill state clear");
    require_ok(ds4_gpu_qwen35_gdn_prefill(
        p_out, p_conv, p_state, p_qkv, p_alpha, p_beta, p_ogate,
        model, MODEL_BYTES, CONV_OFFSET, SSM_A_OFFSET, DT_BIAS_OFFSET, NORM_OFFSET,
        K_HEADS, V_HEADS, TOKENS, norm_eps), "qwen35 GDN prefill");
    static float prefill_out[TOKENS][V_DIM];
    require_ok(ds4_gpu_tensor_read(p_out, 0, prefill_out, sizeof(prefill_out)),
               "prefill output read");
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t i = 0; i < V_DIM; i++) {
            require_close("prefill vs decode", t * V_DIM + i,
                          prefill_out[t][i], (double)decode_out[t][i], 5.0e-4);
        }
    }
    if (failures) {
        fprintf(stderr, "%d mismatches\n", failures);
        return 1;
    }
    printf("prefill vs decode over %d tokens: PASS\n", TOKENS);

    ds4_gpu_tensor_free(p_state);
    ds4_gpu_tensor_free(p_conv);
    ds4_gpu_tensor_free(p_out);
    ds4_gpu_tensor_free(p_ogate);
    ds4_gpu_tensor_free(p_beta);
    ds4_gpu_tensor_free(p_alpha);
    ds4_gpu_tensor_free(p_qkv);
    ds4_gpu_tensor_free(g_state);
    ds4_gpu_tensor_free(g_conv);
    ds4_gpu_tensor_free(g_out);
    ds4_gpu_tensor_free(g_ogate);
    ds4_gpu_tensor_free(g_beta);
    ds4_gpu_tensor_free(g_alpha);
    ds4_gpu_tensor_free(g_qkv);
    ds4_gpu_cleanup();
    munmap(model, MODEL_BYTES);
    puts("Qwen3.5 Gated DeltaNet GPU tests: PASS");
    return 0;
}
