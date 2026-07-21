/*
 * arrange.c — pure-C arrange stage with CLI integration.
 *
 * Decodes a video with libav, runs the greedy block solver, extracts features
 * (imgops), matches against the library (match.c), and writes manifests.
 *
 * Usage:
 *   arrange --video in.mp4 --features features.bin --registry registry.bin \
 *          --manifests out_dir [--feat N] [--bits G] [--color] \
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

static int g_N = 64, g_G = 1, g_channels = 1, g_feat_len = 64;
static double g_max_block_pct = 0.5, g_hero_min_pct = 0.0625;

typedef struct {
    int32_t x, y, w, h, op_id, page_idx;
} Inst;

/* cross-frame tile cache */
typedef struct { uint8_t *feat; int pid; } CacheEntry;
static CacheEntry *g_cache = NULL; static unsigned *g_cache_hash = NULL;
static size_t g_cache_cap = 0, g_cache_n = 0;

static unsigned fnv1a(const uint8_t *d, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}
static int cache_lookup(const uint8_t *feat, int *pid) {
    unsigned h = fnv1a(feat, g_feat_len);
    for (size_t i = 0; i < g_cache_n; i++) {
        size_t idx = (h + i) % g_cache_cap;
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
        CacheEntry *new_cache = realloc(g_cache, ncap * sizeof(CacheEntry));
        unsigned *new_hash = realloc(g_cache_hash, ncap * sizeof(unsigned));
        if (!new_cache || !new_hash) { free(new_cache); free(new_hash); return; }
        g_cache = new_cache;
        g_cache_hash = new_hash;
        memset(g_cache_hash, 0, ncap * sizeof(unsigned));
        g_cache_cap = ncap;
    }
    unsigned h = fnv1a(feat, g_feat_len);
    size_t idx = h % g_cache_cap;
    while (g_cache_hash[idx] != 0) idx = (idx + 1) % g_cache_cap;
    g_cache[idx].feat = malloc(g_feat_len); memcpy(g_cache[idx].feat, feat, g_feat_len);
    g_cache[idx].pid = pid; g_cache_hash[idx] = h; g_cache_n++;
}

typedef struct { double read, gray, solve, match, write; long tiles, hits; } Timings;

/* Forward declaration for solve_full (defined after main). */
void solve_full(const Img *frame, const uint8_t *gray, int w, int h,
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
        "  --feat <n>              Feature grid size (default: 64)\n"
        "  --bits <n>              Bits per cell, 1-8 (default: 1)\n"
        "  --color                 Enable color matching (3 channels)\n"
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

    if (cli_has("feat")) g_N = cli_opt_int("feat", 64);
    if (cli_has("bits")) g_G = cli_opt_int("bits", 1);
    if (cli_has("color")) g_channels = 3;
    if (cli_has("max-block-pct")) g_max_block_pct = cli_opt_dbl("max-block-pct", 0.5);
    if (cli_has("hero-min-pct")) g_hero_min_pct = cli_opt_dbl("hero-min-pct", 0.0625);
    int max_frames = cli_opt_int("max-frames", 0);
    int threads = cli_opt_int("threads", 0);
    cli_set_threads(threads);

    g_feat_len = g_N * g_N * g_channels;

    FeatureDB db; Registry reg;
    if (ba_load_features(feat_path, &db) != 0) {
        cli_die("cannot load features: %s", feat_path);
    }
    if (ba_load_registry(reg_path, &reg) != 0) {
        cli_die("cannot load registry: %s", reg_path);
    }
    /* Override global params from library header to ensure consistency */
    g_N = db.N; g_G = db.G; g_channels = db.channels; g_feat_len = db.feat_len;

    cli_info("library: %d pages | N=%d G=%d ch=%d | feat_len=%d",
             db.n_pages, db.N, db.G, db.channels, db.feat_len);

    VideoDecoder *vd = video_decoder_open(video_path);
    if (!vd) cli_die("cannot open video: %s", video_path);

    int fw = video_decoder_width(vd), fh = video_decoder_height(vd);
    int total_frames = (int)video_decoder_fps(vd) * 300; /* estimate */
    cli_info("video: %dx%d | fps=%.1f | estimating %d frames",
             fw, fh, video_decoder_fps(vd), total_frames);

    /* hero pages by mean intensity over feature bytes */
    int pid_white = 0, pid_black = 0;
    double mx = -1, mn = 256;
    for (int i = 0; i < db.n_pages; i++) {
        double s = 0;
        const uint8_t *f = db.data + (size_t)i * db.feat_len;
        for (int j = 0; j < db.feat_len; j++) s += f[j];
        double m = s / db.feat_len;
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
        solve_full(&frame, gray.pixels, fw, fh, &db, &reg, pid_white, pid_black,
                   max_block, hero_min, manifest, &n, tiles, &nt, &t);

        for (int k = 0; k < n; k++) if (manifest[k].op_id >= 0) total_tiles++;

        char fn[1024]; snprintf(fn, sizeof(fn), "%s/%04d.bin", man_dir, fi);
        FILE *f = fopen(fn, "wb");
        if (f) {
            uint32_t nn = (uint32_t)n;
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
            cli_progress_frame(fi, max_frames > 0 ? max_frames : total_frames,
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

/* Full solver with crop extraction + batch match. */
void solve_full(const Img *frame, const uint8_t *gray, int w, int h,
                const FeatureDB *db, const Registry *reg,
                int pid_white, int pid_black, int max_block, int hero_min,
                Inst *manifest, int *nout, uint8_t *tiles, int *ntiles, Timings *t) {
    const int CELL = 8;
    int32_t *sum = img_integral(gray, w, h);
    int stride = w + 1;
    int gh = (h + CELL - 1) / CELL, gw = (w + CELL - 1) / CELL;
    uint8_t *visited = (uint8_t *)calloc((size_t)gh * gw, 1);
    int feat_len = db->feat_len;
    int max_cells = max_block / CELL;
    int n = 0, nt = 0;

    int *placeholder_idx = NULL;
    int placeholder_cap = 0;

    clock_t t0 = clock();

    for (int cy = 0; cy < gh; cy++) {
        int y = cy * CELL;
        for (int cx = 0; cx < gw; cx++) {
            int x = cx * CELL;
            if (visited[(size_t)cy * gw + cx]) continue;
            int color = (gray[(size_t)y * w + x] >= 127) ? 1 : 0;
            int mcw = 1, mch = 1;

            while (cx + mcw + 1 <= gw && mcw + 1 <= max_cells) {
                int any = 0;
                for (int yy = cy; yy < cy + mch; yy++)
                    if (visited[(size_t)yy * gw + (cx + mcw)]) { any = 1; break; }
                if (any) break;
                int x0 = cx * CELL, y0 = cy * CELL;
                int x1 = (cx + mcw + 1) * CELL, y1 = (cy + mch) * CELL;
                int cnt = sum[y1 * stride + x1] - sum[y0 * stride + x1]
                        - sum[y1 * stride + x0] + sum[y0 * stride + x0];
                int area = (mcw + 1) * mch * CELL * CELL;
                int pure = (color == 1) ? (cnt * 255 >= area * (127 + 20))
                                         : (cnt * 255 <= area * (127 - 20));
                if (pure) mcw++; else break;
            }
            while (cy + mch + 1 <= gh && mch + 1 <= max_cells) {
                int any = 0;
                for (int xx = cx; xx < cx + mcw; xx++)
                    if (visited[(size_t)(cy + mch) * gw + xx]) { any = 1; break; }
                if (any) break;
                int x0 = cx * CELL, y0 = cy * CELL;
                int x1 = (cx + mcw) * CELL, y1 = (cy + mch + 1) * CELL;
                int cnt = sum[y1 * stride + x1] - sum[y0 * stride + x1]
                        - sum[y1 * stride + x0] + sum[y0 * stride + x0];
                int area = mcw * (mch + 1) * CELL * CELL;
                int pure = (color == 1) ? (cnt * 255 >= area * (127 + 20))
                                         : (cnt * 255 <= area * (127 - 20));
                if (pure) mch++; else break;
            }

            int mw = mcw * CELL, mh = mch * CELL;
            if (x + mw > w) mw = w - x;
            if (y + mh > h) mh = h - y;
            for (int yy = cy; yy < cy + mch; yy++)
                for (int xx = cx; xx < cx + mcw; xx++)
                    visited[(size_t)yy * gw + xx] = 1;

            Inst inst;
            inst.x = x; inst.y = y; inst.w = mw; inst.h = mh;

            if (mw >= hero_min && mh >= hero_min) {
                int pid = (color == 1) ? pid_white : pid_black;
                inst.op_id = pid;
                inst.page_idx = reg->entries[pid].page_idx;
                manifest[n++] = inst;
            } else {
                Img crop;
                crop.w = mw; crop.h = mh; crop.channels = 3;
                crop.stride = mw * 3;
                crop.pixels = (uint8_t *)malloc((size_t)mw * mh * 3);
                for (int yy = 0; yy < mh; yy++) {
                    memcpy(crop.pixels + (size_t)yy * crop.stride,
                           frame->pixels + (size_t)(y + yy) * frame->stride + x * 3,
                           (size_t)mw * 3);
                }
                uint8_t *feat_buf = (uint8_t *)malloc((size_t)feat_len);
                img_compute_feature(&crop, db->N, db->G, db->channels == 3, feat_buf);
                img_free(&crop);

                int pid = -1;
                if (cache_lookup(feat_buf, &pid)) {
                    t->hits++;
                    inst.op_id = pid;
                    inst.page_idx = reg->entries[pid].page_idx;
                    manifest[n++] = inst;
                } else {
                    int pidx = n;
                    if (nt >= placeholder_cap) {
                        int ncap = placeholder_cap ? placeholder_cap * 2 : 256;
                        int *new_idx = (int *)realloc(placeholder_idx,
                                                       (size_t)ncap * sizeof(int));
                        if (!new_idx) { free(feat_buf); goto done; }
                        placeholder_idx = new_idx;
                        placeholder_cap = ncap;
                    }
                    placeholder_idx[nt] = pidx;
                    memcpy(tiles + (size_t)nt * feat_len, feat_buf, (size_t)feat_len);
                    nt++;
                    inst.op_id = -1;
                    inst.page_idx = -1;
                    manifest[n++] = inst;
                }
                free(feat_buf);
            }
        }
    }
    free(sum);
    free(visited);

    if (nt > 0) {
        clock_t tm = clock();
        int *results = (int *)malloc((size_t)nt * sizeof(int));
        match_batch(db->data, tiles, db->n_pages, nt, feat_len, results);
        for (int i = 0; i < nt; i++) {
            int pid = results[i];
            int idx = placeholder_idx[i];
            manifest[idx].op_id = pid;
            manifest[idx].page_idx = reg->entries[pid].page_idx;
            cache_put(tiles + (size_t)i * feat_len, pid);
        }
        free(results);
        t->match += (double)(clock() - tm) / CLOCKS_PER_SEC;
        t->tiles += nt;
    }

done:
    free(placeholder_idx);
    t->solve += (double)(clock() - t0) / CLOCKS_PER_SEC;
    *nout = n;
    *ntiles = nt;
}
