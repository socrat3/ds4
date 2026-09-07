/* Qwen3.5 grouped-query attention on CUDA, against a scalar host reference.
 *
 * The reference is the plain definition, in double precision: causal softmax
 * over the visible cache rows, scaled by 1/sqrt(qk_dim), with query head h
 * reading cache head h / (n_head / n_head_kv).  The kernel computes the same
 * thing with a streaming maximum, which is where the two could disagree
 * without either looking wrong.
 *
 * The grouping is the point of this file.  A wrong divisor still produces a
 * proper probability distribution over the right number of rows, so the output
 * stays finite and plausible; only a reference that groups correctly can tell
 * the difference.  Every head therefore gets its own distinguishable cache
 * content, and the test runs the real 24-over-4 shape.
 *
 * Both cache precisions are checked: fp16 is what the model will actually use,
 * because one cache head per query head in fp32 would not fit beside 10.2 GiB
 * of weights.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    N_HEAD = 24,          /* the real shape of Qwen3.8-27B */
    N_HEAD_KV = 4,
    GROUP = N_HEAD / N_HEAD_KV,
    QK_DIM = 256,
    VALUE_DIM = 256,
    TOKENS = 5,
    CACHE_CAP = 48,
    POS0 = 11,            /* not zero: the causal mask must depend on it */
    CACHE_LEN = POS0 + TOKENS,
};

/* Half precision, written out rather than pulled from a CUDA header so the
 * test states exactly what it feeds the kernel. */
static uint16_t f32_to_f16(float value) {
    union { float f; uint32_t u; } in;
    in.f = value;
    const uint32_t sign = (in.u >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((in.u >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = in.u & 0x7fffffu;
    if (exponent <= 0) return (uint16_t)sign;
    if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    /* round to nearest even */
    const uint32_t round = mantissa & 0x1fffu;
    mantissa >>= 13;
    if (round > 0x1000u || (round == 0x1000u && (mantissa & 1u))) {
        mantissa++;
        if (mantissa == 0x400u) { mantissa = 0; exponent++; }
        if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    }
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | mantissa);
}

static float f16_to_f32(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exponent = (h >> 10) & 0x1fu;
    const uint32_t mantissa = h & 0x3ffu;
    union { float f; uint32_t u; } out;
    if (exponent == 0) {
        out.u = sign;
        return out.f;
    }
    if (exponent == 31) {
        out.u = sign | 0x7f800000u | (mantissa << 13);
        return out.f;
    }
    out.u = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    return out.f;
}

static float q_all[TOKENS][N_HEAD][QK_DIM];
static float k_all[CACHE_CAP][N_HEAD_KV][QK_DIM];
static float v_all[CACHE_CAP][N_HEAD_KV][VALUE_DIM];
static double expected[TOKENS][N_HEAD][VALUE_DIM];
static float actual[TOKENS][N_HEAD][VALUE_DIM];
static uint16_t k_f16[CACHE_CAP][N_HEAD_KV][QK_DIM];
static uint16_t v_f16[CACHE_CAP][N_HEAD_KV][VALUE_DIM];
static double weights[CACHE_CAP];

/* The specification, in double. `rounded` replays what fp16 storage does to the
 * cache, so the same reference serves both precisions. */
static void host_attention(bool rounded) {
    const double scale = 1.0 / sqrt((double)QK_DIM);
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t h = 0; h < N_HEAD; h++) {
            const uint32_t kv = h / GROUP;
            const uint32_t visible = POS0 + t + 1u;
            double max_score = -1.0e300;
            for (uint32_t row = 0; row < visible; row++) {
                double dot = 0.0;
                for (uint32_t d = 0; d < QK_DIM; d++) {
                    const double kd = rounded
                        ? (double)f16_to_f32(k_f16[row][kv][d])
                        : (double)k_all[row][kv][d];
                    dot += (double)q_all[t][h][d] * kd;
                }
                weights[row] = dot * scale;
                if (weights[row] > max_score) max_score = weights[row];
            }
            double sum = 0.0;
            for (uint32_t row = 0; row < visible; row++) {
                weights[row] = exp(weights[row] - max_score);
                sum += weights[row];
            }
            for (uint32_t d = 0; d < VALUE_DIM; d++) {
                double acc = 0.0;
                for (uint32_t row = 0; row < visible; row++) {
                    const double vd = rounded
                        ? (double)f16_to_f32(v_f16[row][kv][d])
                        : (double)v_all[row][kv][d];
                    acc += weights[row] * vd;
                }
                expected[t][h][d] = acc / sum;
            }
        }
    }
}

int main(void) {
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t h = 0; h < N_HEAD; h++) {
            for (uint32_t d = 0; d < QK_DIM; d++) {
                q_all[t][h][d] = 0.05f + 0.003f * (float)((d + 7u * h + 3u * t) % 31u)
                               - 0.04f * (float)(h % 5u);
            }
        }
    }
    /* Each cache head gets clearly different content: with identical heads a
     * wrong group divisor would agree with the reference by accident. */
    for (uint32_t r = 0; r < CACHE_CAP; r++) {
        for (uint32_t kv = 0; kv < N_HEAD_KV; kv++) {
            for (uint32_t d = 0; d < QK_DIM; d++) {
                k_all[r][kv][d] = 0.02f * (float)(kv + 1u)
                                + 0.004f * (float)((d + r) % 23u) - 0.05f;
                v_all[r][kv][d] = 0.3f * (float)(kv + 1u)
                                - 0.006f * (float)((d + 5u * r) % 19u);
                k_f16[r][kv][d] = f32_to_f16(k_all[r][kv][d]);
                v_f16[r][kv][d] = f32_to_f16(v_all[r][kv][d]);
            }
        }
    }

    require_ok(ds4_gpu_init(), "CUDA init");

    ds4_gpu_tensor *g_q = ds4_gpu_tensor_alloc(sizeof(q_all));
    ds4_gpu_tensor *g_out = ds4_gpu_tensor_alloc(sizeof(actual));
    ds4_gpu_tensor *g_k32 = ds4_gpu_tensor_alloc(sizeof(k_all));
    ds4_gpu_tensor *g_v32 = ds4_gpu_tensor_alloc(sizeof(v_all));
    ds4_gpu_tensor *g_k16 = ds4_gpu_tensor_alloc(sizeof(k_f16));
    ds4_gpu_tensor *g_v16 = ds4_gpu_tensor_alloc(sizeof(v_f16));
    require_ok(g_q && g_out && g_k32 && g_v32 && g_k16 && g_v16, "allocation");
    require_ok(ds4_gpu_tensor_write(g_q, 0, q_all, sizeof(q_all)), "Q write");
    require_ok(ds4_gpu_tensor_write(g_k32, 0, k_all, sizeof(k_all)), "K write");
    require_ok(ds4_gpu_tensor_write(g_v32, 0, v_all, sizeof(v_all)), "V write");
    require_ok(ds4_gpu_tensor_write(g_k16, 0, k_f16, sizeof(k_f16)), "K16 write");
    require_ok(ds4_gpu_tensor_write(g_v16, 0, v_f16, sizeof(v_f16)), "V16 write");

    host_attention(false);
    require_ok(ds4_gpu_qwen35_attention_gqa_tensor(
        g_out, g_q, g_k32, g_v32, POS0, TOKENS, CACHE_LEN, CACHE_CAP,
        N_HEAD, N_HEAD_KV, QK_DIM, VALUE_DIM, false), "GQA attention fp32");
    require_ok(ds4_gpu_tensor_read(g_out, 0, actual, sizeof(actual)), "read fp32");
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t h = 0; h < N_HEAD; h++) {
            for (uint32_t d = 0; d < VALUE_DIM; d++) {
                require_close("fp32 cache", (t * N_HEAD + h) * VALUE_DIM + d,
                              actual[t][h][d], expected[t][h][d], 2.0e-4);
            }
        }
    }
    if (failures) { fprintf(stderr, "%d mismatches (fp32)\n", failures); return 1; }
    printf("GQA attention vs host reference, fp32 cache: PASS\n");

    host_attention(true);
    require_ok(ds4_gpu_qwen35_attention_gqa_tensor(
        g_out, g_q, g_k16, g_v16, POS0, TOKENS, CACHE_LEN, CACHE_CAP,
        N_HEAD, N_HEAD_KV, QK_DIM, VALUE_DIM, true), "GQA attention fp16");
    require_ok(ds4_gpu_tensor_read(g_out, 0, actual, sizeof(actual)), "read fp16");
    for (uint32_t t = 0; t < TOKENS; t++) {
        for (uint32_t h = 0; h < N_HEAD; h++) {
            for (uint32_t d = 0; d < VALUE_DIM; d++) {
                require_close("fp16 cache", (t * N_HEAD + h) * VALUE_DIM + d,
                              actual[t][h][d], expected[t][h][d], 5.0e-4);
            }
        }
    }
    if (failures) { fprintf(stderr, "%d mismatches (fp16)\n", failures); return 1; }
    printf("GQA attention vs host reference, fp16 cache: PASS\n");

    /* Shapes that cannot be served must be refused, not read past. */
    {
        const int bad_group = ds4_gpu_qwen35_attention_gqa_tensor(
            g_out, g_q, g_k32, g_v32, POS0, TOKENS, CACHE_LEN, CACHE_CAP,
            N_HEAD, 5u, QK_DIM, VALUE_DIM, false);
        const int bad_len = ds4_gpu_qwen35_attention_gqa_tensor(
            g_out, g_q, g_k32, g_v32, POS0, TOKENS, CACHE_CAP + 1u, CACHE_CAP,
            N_HEAD, N_HEAD_KV, QK_DIM, VALUE_DIM, false);
        require_ok(!bad_group && !bad_len, "invalid shapes must be refused");
        printf("invalid shapes refused: PASS\n");
    }

    ds4_gpu_tensor_free(g_v16);
    ds4_gpu_tensor_free(g_k16);
    ds4_gpu_tensor_free(g_v32);
    ds4_gpu_tensor_free(g_k32);
    ds4_gpu_tensor_free(g_out);
    ds4_gpu_tensor_free(g_q);
    ds4_gpu_cleanup();
    puts("Qwen3.5 grouped-query attention GPU tests: PASS");
    return 0;
}
