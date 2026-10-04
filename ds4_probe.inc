/* ds4_probe.inc - internal-state analysis (probes), kept out of the maintained code path.
 *
 * Included once by ds4.c, after the dump helpers (metal_graph_debug_prefix_for,
 * write_f32_binary_file, read_f32_binary_file, xmalloc): same flags and guards as ds4.c, and no
 * Makefile object lists touched. ds4.c and ds4_cli.c only carry the one-line call sites.
 *
 *   DS4_PROBE_OUT=FILE   after each layer, for every row (prompt and generated tokens), the mean over
 *                        the HC copies of the residual. Record: int32 pos, int32 layer, int32 width,
 *                        float[width]. For a repeated position (rejected MTP drafts, prefill re-sweeps)
 *                        the last record wins. Qwen 3.8, DeepSeek V4.1, GLM 5.3.
 *   DS4_METAL_GRAPH_DUMP_PREFIX (+ _NAME, _POS)  antirez's dump convention:
 *                        ds4_probe_dump_last_hc_mean writes the HC-mean of the last row, the format
 *                        read by dir-steering/tools/build_direction.py.
 *   --probe-report FILE.md --probe-dirs DIR   the internal report: project each generated token onto
 *                        the concept directions in DIR (rifiuto/verita/sa .f32 with a .stat), and at
 *                        end of answer write question, answer and the internal verdict.
 *
 * Diagnostics only: synchronizes the GPU on every call. The command batch is left in the state it was
 * found in (on Metal, synchronize closes an open batch, and a double begin is not allowed). */

static bool ds4_probe_resume(bool was_active) {
    return !was_active || ds4_gpu_commands_active() || ds4_gpu_begin_commands();
}

static void ds4_probe_hc_mean(const float *row, uint32_t hc, uint32_t width, float *mean) {
    for (uint32_t i = 0; i < width; i++) {
        float acc = 0.0f;
        for (uint32_t h = 0; h < hc; h++) acc += row[(uint64_t)h * width + i];
        mean[i] = acc / (float)hc;
    }
}

/* ---- internal report: concept directions read the residual instead of the output ---- */

#define DS4_PROBE_MAX_CONCEPTS 6
#define DS4_PROBE_TOP_LAYERS 8

typedef struct {
    char name[24];
    float *dir;                 /* n_layer * width, one normalized vector per layer */
    double *m_pos, *m_neg, *sd, *auc; /* per layer, from the .stat file */
    int order[DS4_PROBE_TOP_LAYERS];  /* layers with |auc-0.5| largest */
} ds4_probe_concept;

static struct {
    bool active;
    FILE *md;
    uint32_t n_layer, width;
    int n_concept;
    ds4_probe_concept concept[DS4_PROBE_MAX_CONCEPTS];
    /* projection of the current position's residual onto each concept, per layer */
    double *proj;               /* n_concept * n_layer */
    bool *have;                 /* n_concept * n_layer */
    int cur_pos;
    /* per-concept trajectory: score at each finalized position */
    double *score;              /* n_concept * cap */
    int *score_pos;             /* cap */
    int n_score, cap_score;
} g_report;

static bool ds4_probe_read_stat(const char *path, uint32_t n_layer, ds4_probe_concept *c) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    c->m_pos = xmalloc((size_t)n_layer * sizeof(double));
    c->m_neg = xmalloc((size_t)n_layer * sizeof(double));
    c->sd = xmalloc((size_t)n_layer * sizeof(double));
    c->auc = xmalloc((size_t)n_layer * sizeof(double));
    char line[256];
    uint32_t seen = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') continue;
        int l;
        double a, b, s, u;
        if (sscanf(line, "%d %lf %lf %lf %lf", &l, &a, &b, &s, &u) == 5 &&
            l >= 0 && (uint32_t)l < n_layer) {
            c->m_pos[l] = a; c->m_neg[l] = b; c->sd[l] = s; c->auc[l] = u;
            seen++;
        }
    }
    fclose(fp);
    if (seen != n_layer) return false;
    /* rank layers by separation |auc - 0.5|, keep the top ones */
    int *idx = xmalloc((size_t)n_layer * sizeof(int));
    for (uint32_t i = 0; i < n_layer; i++) idx[i] = (int)i;
    for (uint32_t i = 0; i < n_layer; i++)
        for (uint32_t j = i + 1; j < n_layer; j++)
            if (fabs(c->auc[idx[j]] - 0.5) > fabs(c->auc[idx[i]] - 0.5)) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
    for (int k = 0; k < DS4_PROBE_TOP_LAYERS && (uint32_t)k < n_layer; k++) c->order[k] = idx[k];
    free(idx);
    return true;
}

static bool ds4_probe_load_concept(const char *dir, const char *name, ds4_probe_concept *c) {
    char f32[1024], stat[1100];
    snprintf(f32, sizeof(f32), "%s/%s.f32", dir, name);
    snprintf(stat, sizeof(stat), "%s/%s.f32.stat", dir, name);
    if (access(f32, R_OK) != 0 || access(stat, R_OK) != 0) return false;
    FILE *fp = fopen(f32, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    const long bytes = ftell(fp);
    fclose(fp);
    const uint64_t n = (uint64_t)g_report.n_layer * g_report.width;
    if ((uint64_t)bytes != n * sizeof(float)) {
        fprintf(stderr, "ds4: probe concept %s has the wrong shape, skipped\n", name);
        return false;
    }
    c->dir = xmalloc((size_t)n * sizeof(float));
    if (!read_f32_binary_file(f32, c->dir, n) || !ds4_probe_read_stat(stat, g_report.n_layer, c)) {
        free(c->dir);
        return false;
    }
    snprintf(c->name, sizeof(c->name), "%s", name);
    return true;
}

static bool g_probe_report_tried;

/* Idempotent: load the concept directions once, given the model geometry. Callable eagerly (before
 * generation, from ds4_probe_report_begin) or lazily from the first captured layer. */
static void ds4_probe_report_init(uint32_t n_layer, uint32_t width) {
    if (g_probe_report_tried) return;
    g_probe_report_tried = true;
    const char *dir = getenv("DS4_PROBE_DIRS");
    const char *md = getenv("DS4_PROBE_REPORT");
    if (!dir || !dir[0]) return;
    g_report.n_layer = n_layer;
    g_report.width = width;
    static const char *nomi[] = {"rifiuto", "verita", "sa"};
    for (size_t i = 0; i < sizeof(nomi) / sizeof(nomi[0]) && g_report.n_concept < DS4_PROBE_MAX_CONCEPTS; i++)
        if (ds4_probe_load_concept(dir, nomi[i], &g_report.concept[g_report.n_concept]))
            g_report.n_concept++;
    if (g_report.n_concept == 0) {
        fprintf(stderr, "ds4: probe report: no concept in %s (need rifiuto/verita/sa .f32 + .stat)\n", dir);
        return;
    }
    g_report.proj = xmalloc((size_t)g_report.n_concept * n_layer * sizeof(double));
    g_report.have = xmalloc((size_t)g_report.n_concept * n_layer * sizeof(bool));
    memset(g_report.have, 0, (size_t)g_report.n_concept * n_layer * sizeof(bool));
    g_report.cap_score = 512;
    g_report.score = xmalloc((size_t)g_report.n_concept * g_report.cap_score * sizeof(double));
    g_report.score_pos = xmalloc((size_t)g_report.cap_score * sizeof(int));
    g_report.cur_pos = -1;
    g_report.active = true;
    if (md && md[0] && !(g_report.md = fopen(md, "a")))
        fprintf(stderr, "ds4: cannot open --probe-report %s\n", md);
}

static double ds4_probe_score(const ds4_probe_concept *c, const double *proj_layer, const bool *have_layer) {
    double sum = 0.0;
    int used = 0;
    for (int k = 0; k < DS4_PROBE_TOP_LAYERS && (uint32_t)k < g_report.n_layer; k++) {
        const int l = c->order[k];
        if (!have_layer[l] || c->sd[l] <= 0.0) continue;
        const double mid = (c->m_pos[l] + c->m_neg[l]) / 2.0;
        const double segno = c->m_pos[l] >= c->m_neg[l] ? 1.0 : -1.0;
        sum += segno * (proj_layer[l] - mid) / c->sd[l];
        used++;
    }
    return used ? sum / used : NAN;
}

/* Finalize the previous position: turn its per-layer projections into one score per concept. */
static void ds4_probe_report_flush_pos(void) {
    if (g_report.cur_pos < 0) return;
    if (g_report.n_score >= g_report.cap_score) {
        /* Agent rounds run past 512 positions: grow instead of dropping the rest of the answer.
         * Scores are laid out per concept, so each concept's run is copied to the new stride. */
        const int cap = g_report.cap_score * 2;
        double *score = xmalloc((size_t)g_report.n_concept * cap * sizeof(double));
        for (int c = 0; c < g_report.n_concept; c++)
            memcpy(score + (size_t)c * cap, g_report.score + (size_t)c * g_report.cap_score,
                   (size_t)g_report.n_score * sizeof(double));
        int *pos = xmalloc((size_t)cap * sizeof(int));
        memcpy(pos, g_report.score_pos, (size_t)g_report.n_score * sizeof(int));
        free(g_report.score);
        free(g_report.score_pos);
        g_report.score = score;
        g_report.score_pos = pos;
        g_report.cap_score = cap;
    }
    for (int c = 0; c < g_report.n_concept; c++) {
        const double *proj = g_report.proj + (size_t)c * g_report.n_layer;
        const bool *have = g_report.have + (size_t)c * g_report.n_layer;
        g_report.score[(size_t)c * g_report.cap_score + g_report.n_score] =
            ds4_probe_score(&g_report.concept[c], proj, have);
    }
    g_report.score_pos[g_report.n_score] = g_report.cur_pos;
    g_report.n_score++;
    memset(g_report.have, 0, (size_t)g_report.n_concept * g_report.n_layer * sizeof(bool));
}

/* One layer's residual (last row = current token) projected onto every concept at that layer. */
static void ds4_probe_report_layer(const float *hc_mean, uint32_t il, uint32_t pos) {
    if (!g_report.active || il >= g_report.n_layer) return;
    if ((int)pos != g_report.cur_pos) {
        ds4_probe_report_flush_pos();
        g_report.cur_pos = (int)pos;
    }
    for (int c = 0; c < g_report.n_concept; c++) {
        const float *d = g_report.concept[c].dir + (size_t)il * g_report.width;
        double s = 0.0;
        for (uint32_t i = 0; i < g_report.width; i++) s += (double)d[i] * hc_mean[i];
        g_report.proj[(size_t)c * g_report.n_layer + il] = s;
        g_report.have[(size_t)c * g_report.n_layer + il] = true;
    }
}

/* ---- logit lens: le parole che lo stato interno di uno strato favorisce (top-k id + punteggio) ----
 * Con DS4_PROBE_LOGIT=FILE, per l'ultimo token, ogni strato scrive "pos<TAB>strato<TAB>id:logit ...".
 * Il logit e' della testa di uscita del modello applicata al residuo di quello strato (la stessa testa
 * dell'output finale: all'ultimo strato la lente riproduce i logit reali del token successivo, che e' la
 * verifica di correttezza). Gli id si traducono in parole con la mappa di --probe-vocab. */
static FILE *ds4_probe_logit_fp(void) {
    static bool init;
    static FILE *fp;
    if (!init) {
        init = true;
        const char *p = getenv("DS4_PROBE_LOGIT");
        if (p && p[0] && !(fp = fopen(p, "w")))
            fprintf(stderr, "ds4: cannot open DS4_PROBE_LOGIT %s\n", p);
    }
    return fp;
}

static bool ds4_probe_logit_on(void) {
    const char *p = getenv("DS4_PROBE_LOGIT");
    return p && p[0];
}

/* Scrive i top-k id per logit decrescente della riga `logits` (nvocab valori). top[] resta ordinato
 * in modo decrescente; per ogni v si inserisce solo se batte l'ultimo tenuto. */
static void ds4_probe_logit_topk(const float *logits, int nvocab, uint32_t pos, uint32_t il, int k) {
    FILE *fp = ds4_probe_logit_fp();
    if (!fp || !logits || nvocab <= 0) return;
    if (k > nvocab) k = nvocab;
    int *top = xmalloc((size_t)k * sizeof(int));
    int n = 0;
    for (int v = 0; v < nvocab; v++) {
        if (n == k && logits[v] <= logits[top[n - 1]]) continue;
        int i = n < k ? n : k - 1;        /* posizione da rimpiazzare/estendere */
        while (i > 0 && logits[v] > logits[top[i - 1]]) { top[i] = top[i - 1]; i--; }
        top[i] = v;
        if (n < k) n++;
    }
    fprintf(fp, "%u\t%u", pos, il);
    for (int i = 0; i < n; i++) fprintf(fp, "\t%d:%.3f", top[i], logits[top[i]]);
    fputc('\n', fp);
    fflush(fp);
    free(top);
}

static FILE *ds4_probe_file(void) {
    static bool init;
    static FILE *fp;
    if (!init) {
        init = true;
        const char *p = getenv("DS4_PROBE_OUT");
        if (p && p[0] && !(fp = fopen(p, "wb")))
            fprintf(stderr, "ds4: cannot open DS4_PROBE_OUT %s\n", p);
    }
    return fp;
}

/* Accumulate rows [0, rows) of residual `t` (rows x hc x width floats) at positions pos0.. Feeds
 * both the DS4_PROBE_OUT capture and the internal report; reads the tensor once for both. */
static void ds4_probe_rows(ds4_gpu_tensor *t, uint32_t il, uint32_t pos0, uint32_t rows,
                           uint32_t hc, uint32_t width) {
    /* Trunk layers only, as the concept files are built: the NextN/MTP layer is not part of them
     * (GLM 5.3: 46 layers, 45 in the files; Qwen 3.8: 49 and 48; V4.1 has none). */
    ds4_probe_report_init(directional_steering_layer_count(), width);   /* idempotente */
    FILE *fp = ds4_probe_file();
    if ((!fp && !g_report.active) || !t || rows == 0 || hc == 0 || width == 0) return;
    const bool was_active = ds4_gpu_commands_active() != 0;
    if (ds4_gpu_synchronize() == 0) { (void)ds4_probe_resume(was_active); return; }
    const uint64_t n = (uint64_t)rows * hc * width;
    float *buf = xmalloc((size_t)n * sizeof(buf[0]));
    float *mean = xmalloc((size_t)width * sizeof(mean[0]));
    bool ok = ds4_gpu_tensor_read(t, 0, buf, n * sizeof(buf[0])) != 0;
    for (uint32_t r = 0; ok && fp && r < rows; r++) {
        ds4_probe_hc_mean(buf + (uint64_t)r * hc * width, hc, width, mean);
        const int32_t hdr[3] = {(int32_t)(pos0 + r), (int32_t)il, (int32_t)width};
        ok = fwrite(hdr, sizeof(hdr), 1, fp) == 1 &&
             fwrite(mean, sizeof(mean[0]), width, fp) == width;
    }
    if (ok && fp) ok = fflush(fp) == 0;
    if (ok && g_report.active) {   /* report tracks only the last row of the batch (current token) */
        ds4_probe_hc_mean(buf + (uint64_t)(rows - 1u) * hc * width, hc, width, mean);
        ds4_probe_report_layer(mean, il, pos0 + rows - 1u);
    }
    if (!ok) fprintf(stderr, "ds4: probe capture failed at layer %u\n", il);
    free(mean);
    free(buf);
    (void)ds4_probe_resume(was_active);
}

/* Verdict for one concept from its score trajectory: value at the last prompt token, mean over the
 * answer, and the first answer token whose score stays above `soglia` to the end (the decision). */
static void ds4_probe_verdict(const ds4_probe_concept *c, int ci, int prompt_tokens, double soglia,
                              char *out, size_t cap) {
    const double *sc = g_report.score + (size_t)ci * g_report.cap_score;
    double at_prompt = NAN, sum = 0.0, mx = -1e30;
    int nans = 0, stable = -1;
    for (int i = 0; i < g_report.n_score; i++) {
        const int pos = g_report.score_pos[i];
        const double v = sc[i];
        if (isnan(v)) continue;
        if (pos == prompt_tokens - 1) at_prompt = v;
        if (pos >= prompt_tokens) { sum += v; nans++; if (v > mx) mx = v; }
    }
    /* decision: first position (prompt first, then answer) that stays above threshold to the end */
    for (int fin = 0; fin < 2 && stable < 0; fin++) {
        for (int i = 0; i < g_report.n_score; i++) {
            const int pos = g_report.score_pos[i];
            const bool zone = fin ? pos >= prompt_tokens : pos < prompt_tokens;
            if (!zone || isnan(sc[i])) continue;
            if (sc[i] > soglia) { if (stable < 0) stable = pos; }
            else stable = -1;
        }
    }
    char quando[96];
    if (stable < 0) snprintf(quando, sizeof(quando), "mai oltre soglia");
    else if (stable < prompt_tokens) snprintf(quando, sizeof(quando), "gia' nel prompt (token %d)", stable);
    else snprintf(quando, sizeof(quando), "dal %d° token generato", stable - prompt_tokens + 1);
    snprintf(out, cap, "%s: z fine prompt %.2f, media risposta %.2f, max %.2f; decisione %s",
             c->name, at_prompt, nans ? sum / nans : NAN, nans ? mx : NAN, quando);
}

/* Write question, answer and verdict, to screen and to the .md, then reset for the next turn. */
static void ds4_probe_report_finish(const char *domanda, const char *risposta, int prompt_tokens,
                                    const char *etichetta) {
    if (!g_report.active) return;
    ds4_probe_report_flush_pos();
    const double soglia = 1.0;
    fprintf(stderr, "\n--- resoconto interno%s%s ---\n", etichetta ? " " : "", etichetta ? etichetta : "");
    if (g_report.md) fprintf(g_report.md, "\n## %s\n\n**Domanda:** %s\n\n**Risposta:** %s\n\n",
                             etichetta ? etichetta : "turno", domanda ? domanda : "", risposta ? risposta : "");
    for (int c = 0; c < g_report.n_concept; c++) {
        char riga[256];
        ds4_probe_verdict(&g_report.concept[c], c, prompt_tokens, soglia, riga, sizeof(riga));
        fprintf(stderr, "  %s\n", riga);
        if (g_report.md) fprintf(g_report.md, "- %s\n", riga);
    }
    if (g_report.md) fflush(g_report.md);
    /* reset the trajectory for the next answer; keep the loaded concepts */
    g_report.n_score = 0;
    g_report.cur_pos = -1;
    memset(g_report.have, 0, (size_t)g_report.n_concept * g_report.n_layer * sizeof(bool));
}

static bool ds4_probe_report_on(void) { return g_report.active; }

/* HC-mean of the last of `rows` rows of `t`, into PREFIX_NAME-il_posPOS.bin (only if antirez's dump
 * filter accepts name/il/pos). */
static void ds4_probe_dump_last_hc_mean(ds4_gpu_tensor *t, uint32_t rows, uint32_t hc, uint32_t width,
                                        const char *name, uint32_t il, uint32_t pos) {
    const char *prefix = metal_graph_debug_prefix_for(name, il, pos);
    if (!t || rows == 0 || hc == 0 || width == 0 || !prefix) return;
    const bool was_active = ds4_gpu_commands_active() != 0;
    if (ds4_gpu_synchronize() == 0) { (void)ds4_probe_resume(was_active); return; }
    float *row = xmalloc((size_t)hc * width * sizeof(row[0]));
    float *mean = xmalloc((size_t)width * sizeof(mean[0]));
    const uint64_t off = (uint64_t)(rows - 1u) * hc * width * sizeof(float);
    if (ds4_gpu_tensor_read(t, off, row, (uint64_t)hc * width * sizeof(float)) != 0) {
        ds4_probe_hc_mean(row, hc, width, mean);
        char path[1024];
        snprintf(path, sizeof(path), "%s_%s-%u_pos%u.bin", prefix, name, il, pos);
        if (write_f32_binary_file(path, mean, width))
            fprintf(stderr, "ds4: dumped %s layer %u last-token HC-mean to %s\n", name, il, path);
    }
    free(mean);
    free(row);
    if (!ds4_probe_resume(was_active))
        fprintf(stderr, "ds4: failed to resume command batch after dumping %s layer %u\n", name, il);
}
