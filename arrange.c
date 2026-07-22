/*
 * arrange.c — pure-C arrange stage with CLI integration.
 *
 * Decodes a video with libav, runs the greedy block solver, extracts features
 * (imgops), matches against the library (match.c), and writes manifests.
 *
 * Usage:
 *   arrange --video in.mp4 --features features.bin --registry registry.bin \
 *          --manifests out_dir [--bits G] [--no-edges] [--scales 32,64,128] \
 *          [--max-block-pct 0.5] [--hero-min-pct 0.0625] [--max-frames 0] \
 *          [--threads N] [--verbose] [--quiet] [--json]
 */
#include "badapple.h"
#include "imgops.h"
#include "video.h"
#include "cli.h"

#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static int g_G = 1, g_has_edges = 1, g_feat_len = 0;
static int g_scales[16] = {32, 64, 128};
static int g_n_scales = 3;
static double g_max_block_pct = 0.5, g_hero_min_pct = 0.0833;

typedef struct {
    int32_t x, y, w, h, op_id, page_idx;
} Inst;

/* ── FNV-1a hash (shared by both caches) ───────────────────────── */
static unsigned fnv1a(const uint8_t *d, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* ── Coarse cache: maps 1KB coarse features → pid (two-stage) ───
 * During the parallel feature-extraction phase this cache is read-only
 * (all threads may look up concurrently). It is populated only in the
 * serial cache_put phase after matching. */
typedef struct { uint8_t *feat; int pid; } CoarseEntry;
static CoarseEntry *g_ccache = NULL; static unsigned *g_ccache_hash = NULL;
static size_t g_ccache_cap = 0, g_ccache_n = 0;

static int coarse_cache_lookup(const uint8_t *feat, int feat_len, int *pid) {
    if (g_ccache_cap == 0) return 0;
    unsigned h = fnv1a(feat, feat_len);
    for (size_t i = 0; i < g_ccache_cap; i++) {
        size_t idx = (h + i) % g_ccache_cap;
        if (g_ccache_hash[idx] == 0) return 0;  /* empty slot — not found */
        if (g_ccache_hash[idx] == h && g_ccache[idx].feat &&
            memcmp(g_ccache[idx].feat, feat, feat_len) == 0) {
            *pid = g_ccache[idx].pid; return 1;
        }
    }
    return 0;
}
static void coarse_cache_put(const uint8_t *feat, int feat_len, int pid) {
    if (g_ccache_n + 1 > g_ccache_cap) {
        size_t ncap = g_ccache_cap ? g_ccache_cap * 2 : 1024;
        CoarseEntry *nc = (CoarseEntry *)calloc(ncap, sizeof(CoarseEntry));
        unsigned *nh = (unsigned *)calloc(ncap, sizeof(unsigned));
        if (!nc || !nh) { free(nc); free(nh); return; }
        for (size_t i = 0; i < g_ccache_cap; i++) {
            if (g_ccache && g_ccache_hash && g_ccache_hash[i] != 0 && g_ccache[i].feat) {
                unsigned hh = g_ccache_hash[i];
                size_t idx = hh % ncap;
                while (nh[idx] != 0) idx = (idx + 1) % ncap;
                nc[idx] = g_ccache[i]; nh[idx] = hh;
            }
        }
        free(g_ccache); free(g_ccache_hash);
        g_ccache = nc; g_ccache_hash = nh; g_ccache_cap = ncap;
    }
    unsigned h = fnv1a(feat, feat_len);
    size_t idx = h % g_ccache_cap;
    while (g_ccache_hash[idx] != 0) idx = (idx + 1) % g_ccache_cap;
    g_ccache[idx].feat = malloc(feat_len); memcpy(g_ccache[idx].feat, feat, feat_len);
    g_ccache[idx].pid = pid; g_ccache_hash[idx] = h; g_ccache_n++;
}

/* ── Full cache: maps full multi-resolution features → pid ─────── */
typedef struct { uint8_t *feat; int pid; } CacheEntry;
static CacheEntry *g_cache = NULL; static unsigned *g_cache_hash = NULL;
static size_t g_cache_cap = 0, g_cache_n = 0;

static int cache_lookup(const uint8_t *feat, int *pid) {
    if (g_cache_cap == 0) return 0;
    unsigned h = fnv1a(feat, g_feat_len);
    for (size_t i = 0; i < g_cache_cap; i++) {
        size_t idx = (h + i) % g_cache_cap;
        if (g_cache_hash[idx] == 0) return 0;  /* empty slot — not found */
        if (g_cache_hash[idx] == h && g_cache[idx].feat &&
            memcmp(g_cache[idx].feat, feat, g_feat_len) == 0) {
            *pid = g_cache[idx].pid; return 1;
        }
    }
    return 0;
}
static void cache_put(const uint8_t *feat, int pid) {
    if (g_cache_n + 1 > g_cache_cap) {
        size_t ncap = g_cache_cap ? g_cache_cap * 2 : 1024;
        CacheEntry *new_cache = (CacheEntry *)calloc(ncap, sizeof(CacheEntry));
        unsigned *new_hash = (unsigned *)calloc(ncap, sizeof(unsigned));
        if (!new_cache || !new_hash) { free(new_cache); free(new_hash); return; }
        for (size_t i = 0; i < g_cache_cap; i++) {
            if (g_cache && g_cache_hash && g_cache_hash[i] != 0 && g_cache[i].feat) {
                unsigned h = g_cache_hash[i];
                size_t idx = h % ncap;
                while (new_hash[idx] != 0) idx = (idx + 1) % ncap;
                new_cache[idx] = g_cache[i]; new_hash[idx] = h;
            }
        }
        free(g_cache); free(g_cache_hash);
        g_cache = new_cache; g_cache_hash = new_hash; g_cache_cap = ncap;
    }
    unsigned h = fnv1a(feat, g_feat_len);
    size_t idx = h % g_cache_cap;
    while (g_cache_hash[idx] != 0) idx = (idx + 1) % g_cache_cap;
    g_cache[idx].feat = malloc(g_feat_len); memcpy(g_cache[idx].feat, feat, g_feat_len);
    g_cache[idx].pid = pid; g_cache_hash[idx] = h; g_cache_n++;
}

typedef struct { double read, gray, solve, feat, match, write; long tiles, hits; } Timings;

/* Forward declaration for solve_full (defined after main). */
void solve_full(const uint8_t *gray, int w, int h,
                const FeatureDB *db, const Registry *reg,
                int pid_white, int pid_black, int max_block, int hero_min,
                Inst *manifest, int *nout, uint8_t *tiles, int *ntiles, Timings *t);

static void print_help(void) {
    fprintf(stderr,
        "videomatch arrange — match video frames to a source library\n"
        "\n"
        "Usage:\n"
        "  arrange --video <file> --features <file> --registry <file> \\\n"
        "         --manifests <dir> [options]\n"
        "\n"
        "Required:\n"
        "  --video <file>          Input video file\n"
        "  --features <file>       Library features.bin\n"
        "  --registry <file>       Library registry.bin\n"
        "  --manifests <dir>       Output directory for manifests\n"
        "\n"
        "Options:\n"
        "  --bits <n>              Bits per cell, 1-8 (default: 1)\n"
        "  --no-edges              Disable edge detection features\n"
        "  --scales <list>         Comma-separated scale levels (default: 32,64,128)\n"
        "  --max-block-pct <f>     Max block size as fraction of width (default: 0.5)\n"
        "  --hero-min-pct <f>      Min hero block as fraction of height (default: 0.0625)\n"
        "  --max-frames <n>        Process only N frames (0 = all)\n"
        "  --threads <n>           Thread count for matching (0 = auto)\n"
        "  --verbose, -v           Verbose output\n"
        "  --quiet, -q             Suppress non-error output\n"
        "  --json                  JSON output mode\n"
        "  --help, -h              Show this help\n"
        "\n"
        "  Note: When using mise, prefix flags with `--` to avoid interception:\n"
        "    mise run arrange -- --help\n"
        "    mise run arrange -- --max-frames 100\n"
        "    mise run arrange -- --threads 4\n"
        "\n"
        "Output:\n"
        "  manifests/<frame>.bin    Binary manifests (24-byte records)\n"
        "  Progress: frame N/M | FPS | cache hit%%\n"
    );
}

int main(int argc, char **argv) {
    cli_init();
    cli_parse(argc, argv);

    const char *video_path = cli_opt_str("video", NULL);
    const char *feat_path  = cli_opt_str("features", "features.bin");
    const char *reg_path   = cli_opt_str("registry", "registry.bin");
    const char *man_dir    = cli_opt_str("manifests", "manifests_greedy");

    if (cli_has("help") || cli_has("h") || !video_path) {
        print_help();
        return video_path ? 0 : 1;
    }

    if (cli_has("bits")) g_G = cli_opt_int("bits", 1);
    if (cli_has("no-edges")) g_has_edges = 0;
    if (cli_has("scales")) {
        /* Parse comma-separated scale list, e.g., "32,64,128" */
        const char *sstr = cli_opt_str("scales", "32,64,128");
        g_n_scales = 0;
        const char *p = sstr;
        while (*p && g_n_scales < 16) {
            while (*p == ' ') p++;
            g_scales[g_n_scales++] = atoi(p);
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
    if (cli_has("max-block-pct")) g_max_block_pct = cli_opt_dbl("max-block-pct", 0.5);
    if (cli_has("hero-min-pct")) g_hero_min_pct = cli_opt_dbl("hero-min-pct", 0.0625);
    int max_frames = cli_opt_int("max-frames", 0);
    int threads = cli_opt_int("threads", 0);
    cli_set_threads(threads);

    g_feat_len = 0;
    for (int i = 0; i < g_n_scales; i++)
        g_feat_len += g_scales[i] * g_scales[i];
    if (g_has_edges) g_feat_len *= 2;

    FeatureDB db; Registry reg;
    if (ba_load_features(feat_path, &db) != 0) {
        cli_die("cannot load features: %s", feat_path);
    }
    if (ba_load_registry(reg_path, &reg) != 0) {
        cli_die("cannot load registry: %s", reg_path);
    }
    /* Override global params from library header to ensure consistency */
    g_G = db.G; g_feat_len = db.feat_len;
    g_n_scales = db.n_scales;
    for (int i = 0; i < db.n_scales; i++) g_scales[i] = db.scales[i];
    g_has_edges = db.has_edges;

    cli_info("library: %d pages | scales=%d G=%d edges=%d | feat_len=%d",
             db.n_pages, db.n_scales, db.G, db.has_edges, db.feat_len);

    VideoDecoder *vd = video_decoder_open(video_path);
    if (!vd) cli_die("cannot open video: %s", video_path);

    int fw = video_decoder_width(vd), fh = video_decoder_height(vd);
    int total_frames = (int)video_decoder_fps(vd) * 300; /* estimate */
    cli_info("video: %dx%d | fps=%.1f | estimating %d frames",
             fw, fh, video_decoder_fps(vd), total_frames);

    /* Hero pages: mean intensity of the coarsest-scale gray feature.
     * The first sub-feature is gray at scales[0]×scales[0]. */
    int hero_feat_len = g_scales[0] * g_scales[0];  /* just the first gray block */
    int pid_white = 0, pid_black = 0;
    double mx = -1, mn = 256;
    for (int i = 0; i < db.n_pages; i++) {
        double s = 0;
        const uint8_t *f = db.data + (size_t)i * db.feat_len;
        for (int j = 0; j < hero_feat_len; j++) s += f[j];
        double m = s / hero_feat_len;
        if (m > mx) { mx = m; pid_white = i; }
        if (m < mn) { mn = m; pid_black = i; }
    }

    int max_block = (int)(fw * g_max_block_pct);
    max_block = (max_block / 8) * 8;
    if (max_block < 8) max_block = 8;
    int hero_min = (int)(fh * g_hero_min_pct);

    if (mkdir(man_dir, 0755) != 0 && errno != EEXIST) {
        cli_die("cannot create output directory: %s", man_dir);
    }

    match_set_threads(g_cli.threads);
    Timings t = {0};
    int frames_done = 0, total_tiles = 0;
    clock_t start = clock();

    Img frame;
    int fi = 0;
    while (video_decoder_next(vd, &frame) == 1 && (max_frames == 0 || fi < max_frames)) {
        clock_t tr = clock();
        Img gray; img_to_gray(&frame, &gray);
        t.gray += (double)(clock() - tr) / CLOCKS_PER_SEC;

        Inst *manifest = malloc(sizeof(Inst) * (fw * fh / 64 + 1));
        uint8_t *tiles = malloc((size_t)db.feat_len * (fw * fh / 64 + 1));
        int n = 0, nt = 0;
        solve_full(gray.pixels, fw, fh, &db, &reg, pid_white, pid_black,
                   max_block, hero_min, manifest, &n, tiles, &nt, &t);

        for (int k = 0; k < n; k++) if (manifest[k].op_id >= 0) total_tiles++;

        char fn[1024]; snprintf(fn, sizeof(fn), "%s/%04d.bin", man_dir, fi);
        FILE *f = fopen(fn, "wb");
        if (f) {
            uint32_t src_w = (uint32_t)fw, src_h = (uint32_t)fh;
            uint32_t nn = (uint32_t)n;
            fwrite(&src_w, 4, 1, f);
            fwrite(&src_h, 4, 1, f);
            fwrite(&nn, 4, 1, f);
            for (int k = 0; k < n; k++) {
                int32_t b[6] = {manifest[k].x, manifest[k].y, manifest[k].w, manifest[k].h,
                                manifest[k].op_id, manifest[k].page_idx};
                fwrite(b, 4, 6, f);
            }
            fclose(f);
        }
        img_free(&gray); img_free(&frame); free(manifest); free(tiles);

        frames_done++; fi++;
        if (!g_cli.quiet) {
            double el = (double)(clock() - start) / CLOCKS_PER_SEC;
            double fps = frames_done / (el > 0 ? el : 1);
            long detail = t.tiles + t.hits;
            double hit = detail ? 100.0 * t.hits / detail : 0;
            cli_progress_frame("arrange", fi, max_frames > 0 ? max_frames : total_frames,
                               fps, hit);
        }
    }
    video_decoder_close(vd);
    ba_free_features(&db);

    double total_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    double overall_fps = frames_done / (total_time > 0.0 ? total_time : 1.0);
    long detail = t.tiles + t.hits;
    double hit_rate = detail ? 100.0 * t.hits / detail : 0.0;

    char summary[256];
    snprintf(summary, sizeof(summary),
             "arrange complete in %.2fs | %.2f fps | %d tiles | cache %.1f%%",
             total_time, overall_fps, total_tiles, hit_rate);
    cli_progress_done(summary);

    if (g_cli.verbose) {
        cli_info("phase breakdown:");
        cli_info("  gray    : %.3fs", t.gray);
        cli_info("  solve   : %.3fs", t.solve);
        cli_info("  feat    : %.3fs", t.feat);
        cli_info("  match   : %.3fs", t.match);
        cli_info("  write   : %.3fs", t.write);
        cli_info("  tiles   : %ld", t.tiles);
        cli_info("  hits    : %ld", t.hits);
    }

    if (g_cli.json) {
        char jbuf[64];
        cli_json_start();
        cli_json_field("stage", "arrange");
        snprintf(jbuf, sizeof(jbuf), "%d", frames_done);
        cli_json_field("frames", jbuf);
        snprintf(jbuf, sizeof(jbuf), "%.2f", overall_fps);
        cli_json_field("fps", jbuf);
        snprintf(jbuf, sizeof(jbuf), "%d", total_tiles);
        cli_json_field("tiles", jbuf);
        cli_json_end();
    }

    return 0;
}

/* ── Pool allocator for tile feature buffers ─────────────────────── */
typedef struct {
    uint8_t *buf;    /* single contiguous buffer */
    size_t   cap;    /* total capacity in bytes */
    size_t   used;   /* current write offset in bytes */
    int      count;  /* number of allocations made */
} TilePool;

static void pool_init(TilePool *p, size_t cap) {
    p->buf = (uint8_t *)malloc(cap);
    p->cap = cap;
    p->used = 0;
    p->count = 0;
}
static uint8_t *pool_alloc(TilePool *p, size_t sz) {
    if (p->used + sz > p->cap) {
        /* grow by 2× or needed amount */
        size_t ncap = p->cap * 2;
        if (ncap < p->used + sz) ncap = p->used + sz;
        uint8_t *nb = (uint8_t *)realloc(p->buf, ncap);
        if (!nb) return NULL;
        p->buf = nb;
        p->cap = ncap;
    }
    uint8_t *ptr = p->buf + p->used;
    p->used += sz;
    p->count++;
    return ptr;
}
static void pool_reset(TilePool *p) {
    p->used = 0;
    p->count = 0;
}
static void pool_free(TilePool *p) {
    free(p->buf);
    p->buf = NULL; p->cap = 0; p->used = 0; p->count = 0;
}

void solve_full(const uint8_t *gray, int w, int h,
                const FeatureDB *db, const Registry *reg,
                int pid_white, int pid_black, int max_block, int hero_min,
                Inst *manifest, int *nout, uint8_t *tiles, int *ntiles, Timings *t) {
    const int CELL = 8;
    /* Build a binary integral image (0/1 after threshold at 127) for exact
     * purity checks, matching the Python reference implementation. */
    uint8_t *binary = (uint8_t *)malloc((size_t)w * h);
    memcpy(binary, gray, (size_t)w * h);
    img_threshold_u8(binary, w * h, 127, 1);
    int64_t *sum = img_integral(binary, w, h);
    free(binary);
    int stride = w + 1;
    int gh = (h + CELL - 1) / CELL, gw = (w + CELL - 1) / CELL;
    uint8_t *visited = (uint8_t *)calloc((size_t)gh * gw, 1);
    int feat_len = db->feat_len;
    int max_cells = max_block / CELL;
    int n = 0, nt = 0;

    /* ── Tile spec array: records position of non-hero tiles ─────── */
    typedef struct { int x, y, w, h, manifest_idx; } TileSpec;
    int spec_cap = 512;
    TileSpec *specs = (TileSpec *)malloc((size_t)spec_cap * sizeof(TileSpec));
    int n_specs = 0;

    clock_t t0 = clock();

    /* ═══════════════════════════════════════════════════════════════
     * Phase 1: Serial greedy solver — identify all tiles
     * ═══════════════════════════════════════════════════════════════ */
    for (int cy = 0; cy < gh; cy++) {
        int y = cy * CELL;
        for (int cx = 0; cx < gw; cx++) {
            int x = cx * CELL;
            if (visited[(size_t)cy * gw + cx]) continue;
            int color = (gray[(size_t)y * w + x] > 127) ? 1 : 0;
            int mcw = 1, mch = 1;

            while (cx + mcw + 1 <= gw && mcw + 1 <= max_cells) {
                int any = 0;
                for (int yy = cy; yy < cy + mch; yy++)
                    if (visited[(size_t)yy * gw + (cx + mcw)]) { any = 1; break; }
                if (any) break;
                int x0 = cx * CELL, y0 = cy * CELL;
                int x1 = (cx + mcw + 1) * CELL, y1 = (cy + mch) * CELL;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                int64_t cnt = sum[y1 * stride + x1] - sum[y0 * stride + x1]
                        - sum[y1 * stride + x0] + sum[y0 * stride + x0];
                int area = (x1 - x0) * (y1 - y0);
                int pure = (color == 1) ? (cnt == area) : (cnt == 0);
                if (pure) mcw++; else break;
            }
            while (cy + mch + 1 <= gh && mch + 1 <= max_cells) {
                int any = 0;
                for (int xx = cx; xx < cx + mcw; xx++)
                    if (visited[(size_t)(cy + mch) * gw + xx]) { any = 1; break; }
                if (any) break;
                int x0 = cx * CELL, y0 = cy * CELL;
                int x1 = (cx + mcw) * CELL, y1 = (cy + mch + 1) * CELL;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                int64_t cnt = sum[y1 * stride + x1] - sum[y0 * stride + x1]
                        - sum[y1 * stride + x0] + sum[y0 * stride + x0];
                int area = (x1 - x0) * (y1 - y0);
                int pure = (color == 1) ? (cnt == area) : (cnt == 0);
                if (pure) mch++; else break;
            }

            int mw = mcw * CELL, mh = mch * CELL;
            if (x + mw > w) mw = w - x;
            if (y + mh > h) mh = h - y;
            for (int yy = cy; yy < cy + mch; yy++)
                for (int xx = cx; xx < cx + mcw; xx++)
                    visited[(size_t)yy * gw + xx] = 1;

            if (mw >= hero_min && mh >= hero_min) {
                /* Hero block: uniform region — fast memset in renderer. */
                manifest[n].x = x; manifest[n].y = y;
                manifest[n].w = mw; manifest[n].h = mh;
                manifest[n].op_id = (color == 1) ? -2 : -1;
                manifest[n].page_idx = -1;
                n++;
            } else {
                /* Non-hero tile: record spec for parallel feature extraction. */
                if (n_specs >= spec_cap) {
                    spec_cap *= 2;
                    specs = (TileSpec *)realloc(specs, (size_t)spec_cap * sizeof(TileSpec));
                }
                int midx = n;  /* manifest index for this tile's placeholder */
                specs[n_specs].x = x;
                specs[n_specs].y = y;
                specs[n_specs].w = mw;
                specs[n_specs].h = mh;
                specs[n_specs].manifest_idx = midx;
                n_specs++;
                /* Placeholder entry in manifest (will be filled after matching). */
                manifest[n].x = x; manifest[n].y = y;
                manifest[n].w = mw; manifest[n].h = mh;
                manifest[n].op_id = -1;
                manifest[n].page_idx = -1;
                n++;
            }
        }
    }
    free(sum);
    free(visited);

    if (n_specs == 0) goto done;

    /* ═══════════════════════════════════════════════════════════════
     * Phase 2: Pre-allocate feature buffers (pool bump-alloc)
     * ═══════════════════════════════════════════════════════════════ */
    TilePool pool;
    pool_init(&pool, (size_t)n_specs * feat_len + 4096);
    uint8_t *feat_bufs = pool_alloc(&pool, (size_t)n_specs * feat_len);

    /* ═══════════════════════════════════════════════════════════════
     * Phase 3: Two-stage parallel feature extraction
     *
     * Stage A (all tiles): compute coarse 1KB feature (32×32 gray,
     *   quantized) → check coarse cache → if hit, record pid directly.
     * Stage B (misses only): compute full multi-resolution 43KB feature.
     *
     * With ~41% cache hit rate, this eliminates ~33% of full feature
     * computation work (3 area resizes + 3 Sobel + quant for each hit).
     * ═══════════════════════════════════════════════════════════════ */
    clock_t tf = clock();
    int coarse_len = g_scales[0] * g_scales[0];  /* e.g. 1024 for 32×32 */
    int *coarse_hit = (int *)malloc((size_t)n_specs * sizeof(int));
    for (int i = 0; i < n_specs; i++) coarse_hit[i] = -1;
    {
#ifdef _OPENMP
        int nthreads = omp_get_max_threads();
#else
        int nthreads = 1;
#endif
        size_t max_crop_sz = (size_t)max_block * max_block;
        uint8_t *crop_bufs = (uint8_t *)malloc((size_t)nthreads * max_crop_sz);

#ifdef _OPENMP
        #pragma omp parallel
#endif
        {
#ifdef _OPENMP
            int tid = omp_get_thread_num();
#else
            int tid = 0;
#endif
            uint8_t *my_crop = crop_bufs + (size_t)tid * max_crop_sz;

#ifdef _OPENMP
            #pragma omp for schedule(static)
#endif
            for (int i = 0; i < n_specs; i++) {
                TileSpec *sp = &specs[i];
                /* Extract grayscale crop (1 channel) */
                for (int yy = 0; yy < sp->h; yy++) {
                    memcpy(my_crop + (size_t)yy * sp->w,
                           gray + (size_t)(sp->y + yy) * w + sp->x,
                           (size_t)sp->w);
                }

                /* ── Stage A: coarse 1KB feature + cache check ────── */
                int N = g_scales[0];
                int sw = sp->w, sh = sp->h;
                int maxv = (1 << db->G) - 1;
                /* Inline area resize + quantize to N×N (no malloc) */
                uint8_t coarse_feat[4096]; /* max coarse_len (64×64) */
                for (int dy = 0; dy < N; dy++) {
                    int sy0 = (int)((long long)dy * sh / N);
                    int sy1 = (int)((long long)(dy + 1) * sh / N);
                    if (sy1 > sh) sy1 = sh;
                    for (int dx = 0; dx < N; dx++) {
                        int sx0 = (int)((long long)dx * sw / N);
                        int sx1 = (int)((long long)(dx + 1) * sw / N);
                        if (sx1 > sw) sx1 = sw;
                        unsigned long sum = 0;
                        for (int sy = sy0; sy < sy1; sy++)
                            for (int sx = sx0; sx < sx1; sx++)
                                sum += my_crop[(size_t)sy * sw + sx];
                        int area = (sy1 - sy0) * (sx1 - sx0);
                        int v = (area > 0) ? (int)(sum / area) : 0;
                        int q = (db->G >= 8) ? v : (v * maxv + 127) / 255;
                        if (q > maxv) q = maxv;
                        coarse_feat[dy * N + dx] = (uint8_t)q;
                    }
                }

                int pid = -1;
                if (coarse_cache_lookup(coarse_feat, coarse_len, &pid)) {
                    coarse_hit[i] = pid;  /* skip full feature extraction */
                    continue;
                }

                /* ── Stage B: full multi-resolution feature ───────── */
                Img crop;
                crop.w = sp->w; crop.h = sp->h; crop.channels = 1;
                crop.stride = sp->w;
                crop.pixels = my_crop;
                img_compute_feature_multires(&crop, g_scales, g_n_scales,
                                              db->G, db->has_edges,
                                              feat_bufs + (size_t)i * feat_len);
            }
        }
        free(crop_bufs);
    }

    /* Process coarse-cache hits (fast path — no full feature needed) */
    for (int i = 0; i < n_specs; i++) {
        if (coarse_hit[i] >= 0) {
            t->hits++;
            int midx = specs[i].manifest_idx;
            manifest[midx].op_id = coarse_hit[i];
            manifest[midx].page_idx = reg->entries[coarse_hit[i]].page_idx;
        }
    }
    t->feat += (double)(clock() - tf) / CLOCKS_PER_SEC;

    /* ═══════════════════════════════════════════════════════════════
     * Phase 4: Full-cache lookup for remaining (non-coarse-hit) tiles
     * ═══════════════════════════════════════════════════════════════ */
    int *miss_idx = (int *)malloc((size_t)n_specs * sizeof(int));
    nt = 0;
    for (int i = 0; i < n_specs; i++) {
        if (coarse_hit[i] >= 0) continue;  /* already resolved */
        const uint8_t *feat = feat_bufs + (size_t)i * feat_len;
        int pid = -1;
        if (cache_lookup(feat, &pid)) {
            t->hits++;
            int midx = specs[i].manifest_idx;
            manifest[midx].op_id = pid;
            manifest[midx].page_idx = reg->entries[pid].page_idx;
        } else {
            miss_idx[nt] = i;
            memcpy(tiles + (size_t)nt * feat_len, feat, (size_t)feat_len);
            nt++;
        }
    }

    /* ═══════════════════════════════════════════════════════════════
     * Phase 5: Batch match + cache_put (both coarse + full)
     * ═══════════════════════════════════════════════════════════════ */
    if (nt > 0) {
        clock_t tm = clock();
        int *results = (int *)malloc((size_t)nt * sizeof(int));
        int coarse_for_match = g_scales[0] * g_scales[0];
        match_batch_coarse(db->data, tiles, db->n_pages, nt, feat_len, coarse_for_match, results);
        for (int i = 0; i < nt; i++) {
            int pid = results[i];
            int midx = specs[miss_idx[i]].manifest_idx;
            manifest[midx].op_id = pid;
            manifest[midx].page_idx = reg->entries[pid].page_idx;
            /* Populate BOTH caches for future hits */
            cache_put(tiles + (size_t)i * feat_len, pid);
            coarse_cache_put(tiles + (size_t)i * feat_len, coarse_len, pid);
        }
        free(results);
        t->match += (double)(clock() - tm) / CLOCKS_PER_SEC;
        t->tiles += nt;
    }

    free(coarse_hit);
    free(miss_idx);
    pool_free(&pool);
    free(specs);
done:
    t->solve += (double)(clock() - t0) / CLOCKS_PER_SEC;
    *nout = n;
    *ntiles = nt;
}
