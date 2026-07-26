/*
 * render.c — pure-C render stage with CLI integration.
 *
 * Reads manifests_greedy/<frame>.bin, looks up source images from registry,
 * assembles frames, and writes them to an output file via libav.
 *
 * Optimizations:
 *   - Atlas cache: render each unique source page once, reuse across tiles/frames
 *   - OpenMP parallel instructions: disjoint canvas regions, no locks
 *   - Pthreads encode pipeline: assembly overlaps with encoding
 *   - Row-wise memcpy blit (eliminates per-pixel branching)
 *   - Pre-loaded manifests (batch I/O at startup)
 *
 * Usage:
 *   render --manifests dir --registry registry.bin --output out.mov \
 *          --width W --height H [--fps N] [--channels N] [--threads N] [--verbose] [--quiet] [--json]
 */
#include "badapple.h"
#include "imgops.h"
#include "video.h"
#include <math.h>
#include "cli.h"
#include "pdf.h"
#include "system_detect.h"
#include "vt_prores.h"

#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#define MAX_MANIFESTS 100000
#define MAX_INSTS     65536
#define FRAME_QUEUE_SIZE 4  /* number of canvas slots in the encode pipeline */

typedef struct { int32_t x, y, w, h, op_id, page_idx; } Inst;

static int has_suffix(const char *s, const char *suffix) {
    size_t sl = strlen(s), fl = strlen(suffix);
    if (sl < fl) return 0;
    return strcasecmp(s + sl - fl, suffix) == 0;
}

static int cmp_manifest(const void *a, const void *b) {
    /* Sort by numeric frame number embedded in filename (e.g. "0042.bin").
     * Falls back to lexicographic if no digits found. */
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    /* Find last '/' then parse leading digits */
    const char *fa = strrchr(sa, '/');
    const char *fb = strrchr(sb, '/');
    fa = fa ? fa + 1 : sa;
    fb = fb ? fb + 1 : sb;
    int na = atoi(fa), nb = atoi(fb);
    if (na != nb) return na - nb;
    return strcmp(sa, sb);
}

static int load_manifest(const char *path, Inst **out, int *nout,
                         int target_w, int target_h) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 12) { fclose(f); return -1; }

    uint32_t src_w = 0, src_h = 0, n = 0;
    if (fread(&src_w, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&src_h, 4, 1, f) != 1) { fclose(f); return -1; }
    if (fread(&n, 4, 1, f) != 1) { fclose(f); return -1; }
    if (n > MAX_INSTS) { fclose(f); return -1; }

    /* Compute uniform scale with center-crop to fill output canvas */
    double scale = 1.0, ox = 0.0, oy = 0.0;
    if (src_w > 0 && src_h > 0 && (src_w != (uint32_t)target_w || src_h != (uint32_t)target_h)) {
        double sx = (double)target_w / (double)src_w;
        double sy = (double)target_h / (double)src_h;
        scale = (sx > sy) ? sx : sy;
        ox = ((double)target_w - (double)src_w * scale) * 0.5;
        oy = ((double)target_h - (double)src_h * scale) * 0.5;
    }

    Inst *insts = (Inst *)malloc((size_t)n * sizeof(Inst));
    if (!insts) { fclose(f); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        int32_t b[6];
        if (fread(b, 4, 6, f) != 6) { free(insts); fclose(f); return -1; }
        if (scale != 1.0) {
            /* Compute new position and size with rounding (not truncation)
             * to eliminate 1px gaps at tile boundaries. */
            double nx = (double)b[0] * scale + ox;
            double ny = (double)b[1] * scale + oy;
            double nw = (double)b[2] * scale;
            double nh = (double)b[3] * scale;
            insts[i].x = (int32_t)(nx + 0.5);
            insts[i].y = (int32_t)(ny + 0.5);
            insts[i].w = (int32_t)(nw + 0.5);
            insts[i].h = (int32_t)(nh + 0.5);
            /* Clamp to canvas bounds */
            if (insts[i].x < 0) insts[i].x = 0;
            if (insts[i].y < 0) insts[i].y = 0;
            if (insts[i].x + insts[i].w > target_w) insts[i].w = target_w - insts[i].x;
            if (insts[i].y + insts[i].h > target_h) insts[i].h = target_h - insts[i].y;
        } else {
            insts[i].x = b[0]; insts[i].y = b[1];
            insts[i].w = b[2]; insts[i].h = b[3];
        }
        insts[i].op_id = b[4]; insts[i].page_idx = b[5];
    }
    fclose(f);
    *out = insts; *nout = (int)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Atlas cache — tile-level caching                                   */
/*  Key: (op_id, tile_w, tile_h). Each entry stores the pre-scaled    */
/*  tile image ready to blit. Tiny entries (~50 KB) vs old full-page  */
/*  entries (~16 MB), so thousands fit in the budget.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int      op_id;        /* key part 1: registry index (-1/-2 = solid) */
    int      tile_w;       /* key part 2: output tile width */
    int      tile_h;       /* key part 3: output tile height */
    Img      tile;         /* cached pre-scaled tile (owner) */
    size_t   bytes;        /* memory used by this entry */
    int      valid;        /* 1 if slot is occupied */
    uint32_t lru;          /* insertion order for eviction */
} AtlasEntry;

typedef struct {
    AtlasEntry *entries;
    int         capacity;
    int         count;
    size_t      total_bytes;
    size_t      budget;
    uint32_t    tick;       /* monotonic counter for LRU */
    int         enabled;
    long        hits;       /* cache hit counter */
    long        misses;     /* cache miss counter */
} AtlasCache;

static void atlas_init(AtlasCache *atlas, size_t budget) {
    atlas->capacity = 256;
    atlas->count = 0;
    atlas->total_bytes = 0;
    atlas->budget = budget;
    atlas->tick = 0;
    atlas->enabled = (budget > 0);
    atlas->hits = 0;
    atlas->misses = 0;
    atlas->entries = (AtlasEntry *)calloc((size_t)atlas->capacity, sizeof(AtlasEntry));
    if (!atlas->entries) {
        atlas->enabled = 0;
        return;
    }
}

static void atlas_free(AtlasCache *atlas) {
    for (int i = 0; i < atlas->capacity; i++) {
        if (atlas->entries[i].valid == 1) img_free(&atlas->entries[i].tile);
    }
    free(atlas->entries);
    atlas->entries = NULL;
    atlas->count = 0;
    atlas->total_bytes = 0;
}

static unsigned atlas_hash(int op_id, int tw, int th) {
    unsigned h = (unsigned)op_id * 2654435761u;
    h ^= (unsigned)tw * 374761393u;
    h ^= (unsigned)th * 668265263u;
    return h ? h : 1;
}

/* Thread-safe read-only lookup (no shared state mutation) */
static AtlasEntry *atlas_lookup(AtlasCache *atlas, int op_id, int tw, int th) {
    if (!atlas->enabled) return NULL;
    unsigned h = atlas_hash(op_id, tw, th);
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (atlas->entries[idx].valid == 0) return NULL;
        if (atlas->entries[idx].valid == 2) continue;
        if (atlas->entries[idx].op_id == op_id &&
            atlas->entries[idx].tile_w == tw &&
            atlas->entries[idx].tile_h == th)
            return &atlas->entries[idx];
    }
    return NULL;
}

/* Single-threaded lookup that tracks hits/misses (for pre-population) */
static AtlasEntry *atlas_lookup_stats(AtlasCache *atlas, int op_id, int tw, int th) {
    if (!atlas->enabled) { atlas->misses++; return NULL; }
    unsigned h = atlas_hash(op_id, tw, th);
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (atlas->entries[idx].valid == 0) { atlas->misses++; return NULL; }
        if (atlas->entries[idx].valid == 2) continue;
        if (atlas->entries[idx].op_id == op_id &&
            atlas->entries[idx].tile_w == tw &&
            atlas->entries[idx].tile_h == th) {
            atlas->entries[idx].lru = atlas->tick++;
            atlas->hits++;
            return &atlas->entries[idx];
        }
    }
    atlas->misses++;
    return NULL;
}

static void atlas_evict_lru(AtlasCache *atlas) {
    int oldest_idx = -1;
    uint32_t oldest_tick = UINT32_MAX;
    for (int i = 0; i < atlas->capacity; i++) {
        if (atlas->entries[i].valid == 1 && atlas->entries[i].lru < oldest_tick) {
            oldest_tick = atlas->entries[i].lru;
            oldest_idx = i;
        }
    }
    if (oldest_idx >= 0) {
        atlas->total_bytes -= atlas->entries[oldest_idx].bytes;
        img_free(&atlas->entries[oldest_idx].tile);
        atlas->entries[oldest_idx].valid = 2;
        atlas->count--;
    }
}

static void atlas_insert(AtlasCache *atlas, int op_id, int tw, int th,
                          const Img *tile) {
    if (!atlas->enabled) return;

    /* Evict until we have room */
    size_t entry_bytes = (size_t)tile->w * tile->h * tile->channels;
    while (atlas->total_bytes + entry_bytes > atlas->budget && atlas->count > 0)
        atlas_evict_lru(atlas);

    /* Grow if needed */
    if (atlas->count >= atlas->capacity * 3 / 4) {
        int old_cap = atlas->capacity;
        atlas->capacity *= 2;
        AtlasEntry *new_entries = (AtlasEntry *)calloc((size_t)atlas->capacity, sizeof(AtlasEntry));
        if (!new_entries) {
            atlas->capacity = old_cap;
        } else {
        /* Rehash existing entries (skip tombstones) */
        for (int i = 0; i < old_cap; i++) {
            if (atlas->entries[i].valid == 1) {
                unsigned h = atlas_hash(atlas->entries[i].op_id,
                                        atlas->entries[i].tile_w,
                                        atlas->entries[i].tile_h);
                for (int p = 0; p < atlas->capacity; p++) {
                    int idx = (int)((h + (unsigned)p) % (unsigned)atlas->capacity);
                    if (new_entries[idx].valid == 0) {
                        new_entries[idx] = atlas->entries[i];
                        break;
                    }
                }
            }
        }
        free(atlas->entries);
        atlas->entries = new_entries;
        }
    }

    /* Insert — prefer reusing a tombstone slot over an empty one */
    unsigned h = atlas_hash(op_id, tw, th);
    int tombstone_idx = -1;
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (atlas->entries[idx].valid == 0) {
            int use_idx = (tombstone_idx >= 0) ? tombstone_idx : idx;
            atlas->entries[use_idx].op_id = op_id;
            atlas->entries[use_idx].tile_w = tw;
            atlas->entries[use_idx].tile_h = th;
            {
                Img *dst = &atlas->entries[use_idx].tile;
                dst->w = tw;
                dst->h = th;
                dst->channels = tile->channels;
                dst->stride = tw * tile->channels;
                dst->pixels = (uint8_t *)malloc((size_t)tw * th * tile->channels);
                if (!dst->pixels) return;
                size_t row_bytes = (size_t)tw * tile->channels;
                for (int y = 0; y < th; y++)
                    memcpy(dst->pixels + (size_t)y * dst->stride,
                           tile->pixels + (size_t)y * tile->stride,
                           row_bytes);
            }
            atlas->entries[use_idx].bytes = (size_t)tw * th * tile->channels;
            atlas->entries[use_idx].valid = 1;
            atlas->entries[use_idx].lru = atlas->tick++;
            atlas->total_bytes += atlas->entries[use_idx].bytes;
            atlas->count++;
            return;
        }
        if (atlas->entries[idx].valid == 2 && tombstone_idx < 0)
            tombstone_idx = idx;
    }
    /* All slots occupied — if we found a tombstone, reuse it */
    if (tombstone_idx >= 0) {
        int use_idx = tombstone_idx;
        atlas->entries[use_idx].op_id = op_id;
        atlas->entries[use_idx].tile_w = tw;
        atlas->entries[use_idx].tile_h = th;
        {
            Img *dst = &atlas->entries[use_idx].tile;
            dst->w = tw;
            dst->h = th;
            dst->channels = tile->channels;
            dst->stride = tw * tile->channels;
            dst->pixels = (uint8_t *)malloc((size_t)tw * th * tile->channels);
            if (!dst->pixels) return;
            size_t row_bytes = (size_t)tw * tile->channels;
            for (int y = 0; y < th; y++)
                memcpy(dst->pixels + (size_t)y * dst->stride,
                       tile->pixels + (size_t)y * tile->stride,
                       row_bytes);
        }
        atlas->entries[use_idx].bytes = (size_t)tw * th * tile->channels;
        atlas->entries[use_idx].valid = 1;
        atlas->entries[use_idx].lru = atlas->tick++;
        atlas->total_bytes += atlas->entries[use_idx].bytes;
        atlas->count++;
    }
}

/* Load a source page, scale to tile size, cache the result */
static void atlas_cache_tile(AtlasCache *atlas, const Registry *reg,
                              int op_id, int tw, int th, int channels) {
    if (op_id < 0 || op_id >= reg->n) return;
    if (tw <= 0 || th <= 0) return;
    if (atlas_lookup_stats(atlas, op_id, tw, th)) return;  /* already cached */

    const char *pdf_path = reg->entries[op_id].pdf_path;
    int page_idx = reg->entries[op_id].page_idx;

    Img src;
    if (pdf_render_page(pdf_path, page_idx, 1.0f, &src) != 0) return;

    if (channels == 3) {
        /* Cache BGR directly — skip grayscale conversion */
        Img scaled;
        img_resize_area(&src, &scaled, tw, th);
        img_free(&src);
        atlas_insert(atlas, op_id, tw, th, &scaled);
        img_free(&scaled);
    } else {
        Img gray;
        img_to_gray(&src, &gray);
        img_free(&src);

        /* Scale directly to tile output size */
        Img scaled;
        img_resize_area(&gray, &scaled, tw, th);
        img_free(&gray);

        atlas_insert(atlas, op_id, tw, th, &scaled);
        img_free(&scaled);
    }
}

/* ------------------------------------------------------------------ */
/*  Frame encode pipeline (pthreads producer-consumer queue)           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *pixels;   /* owned copy of frame pixel data (grayscale or BGR) */
    int      valid;    /* 1 = slot holds a frame ready for encoding */
} FrameSlot;

typedef struct {
    FrameSlot     slots[FRAME_QUEUE_SIZE];
    int           write_pos;
    int           read_pos;
    int           count;
    int           encoding;    /* slots currently being encoded by consumer */
    int           stop;          /* signal encoder to exit */
    pthread_mutex_t mutex;
    pthread_cond_t  can_write;   /* main thread waits when queue full */
    pthread_cond_t  can_read;    /* encoder thread waits when queue empty */
    VideoEncoder  *ve;
    VTEncoder    *vt;            /* hardware ProRes encoder (NULL if using FFmpeg) */
    int            width, height;
    int            channels;     /* 1 = grayscale, 3 = BGR color */
    size_t         canvas_bytes; /* pre-allocated buffer size */
    long           frames_encoded;
} EncodePipeline;

static void *encoder_thread(void *arg) {
    EncodePipeline *ep = (EncodePipeline *)arg;
    while (1) {
        pthread_mutex_lock(&ep->mutex);
        while (ep->count == 0 && !ep->stop)
            pthread_cond_wait(&ep->can_read, &ep->mutex);
        if (ep->stop && ep->count == 0) {
            pthread_mutex_unlock(&ep->mutex);
            break;
        }
        /* Take frame from queue — find next queued slot */
        int slot_idx = ep->read_pos;
        while (ep->slots[slot_idx].valid != 1)
            slot_idx = (slot_idx + 1) % FRAME_QUEUE_SIZE;
        uint8_t *pixels = ep->slots[slot_idx].pixels;
        ep->read_pos = (slot_idx + 1) % FRAME_QUEUE_SIZE;
        ep->count--;
        ep->encoding++;
        pthread_cond_signal(&ep->can_write);
        pthread_mutex_unlock(&ep->mutex);

        /* Encode — dispatch to hardware or software encoder */
        if (ep->vt) {
            vt_prores_write(ep->vt, pixels, ep->width, ep->height,
                            ep->width * ep->channels, ep->channels);
        } else {
            Img frame;
            frame.w = ep->width; frame.h = ep->height;
            frame.channels = ep->channels;
            frame.stride = ep->width * ep->channels;
            frame.pixels = pixels;
            video_encoder_write(ep->ve, &frame);
        }

        /* Release buffer back to pool */
        pthread_mutex_lock(&ep->mutex);
        ep->slots[slot_idx].valid = 0;
        ep->encoding--;
        pthread_cond_signal(&ep->can_write);
        pthread_mutex_unlock(&ep->mutex);

        ep->frames_encoded++;
    }
    return NULL;
}

static void pipeline_init(EncodePipeline *ep, VideoEncoder *ve, VTEncoder *vt,
                           int w, int h, int channels, size_t canvas_bytes) {
    memset(ep, 0, sizeof(*ep));
    ep->ve = ve;
    ep->vt = vt;
    ep->width = w;
    ep->height = h;
    ep->channels = channels > 0 ? channels : 1;
    ep->canvas_bytes = canvas_bytes;
    pthread_mutex_init(&ep->mutex, NULL);
    pthread_cond_init(&ep->can_write, NULL);
    pthread_cond_init(&ep->can_read, NULL);
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        ep->slots[i].pixels = (uint8_t *)malloc(canvas_bytes);
        ep->slots[i].valid = 0;
    }
}

static void pipeline_push(EncodePipeline *ep, const uint8_t *pixels, size_t nbytes) {
    pthread_mutex_lock(&ep->mutex);
    while (ep->count + ep->encoding >= FRAME_QUEUE_SIZE)
        pthread_cond_wait(&ep->can_write, &ep->mutex);
    /* Find a free slot (valid == 0) */
    int slot = ep->write_pos;
    while (ep->slots[slot].valid != 0)
        slot = (slot + 1) % FRAME_QUEUE_SIZE;
    memcpy(ep->slots[slot].pixels, pixels, nbytes);
    ep->slots[slot].valid = 1;
    ep->write_pos = (slot + 1) % FRAME_QUEUE_SIZE;
    ep->count++;
    pthread_cond_signal(&ep->can_read);
    pthread_mutex_unlock(&ep->mutex);
}

static void pipeline_flush(EncodePipeline *ep) {
    pthread_mutex_lock(&ep->mutex);
    ep->stop = 1;
    pthread_cond_signal(&ep->can_read);
    pthread_mutex_unlock(&ep->mutex);
}

static void pipeline_shutdown(EncodePipeline *ep) {
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        free(ep->slots[i].pixels);
        ep->slots[i].pixels = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Help                                                               */
/* ------------------------------------------------------------------ */

static void print_help(void) {
    fprintf(stderr,
        "videomatch render — render final video from manifests\n"
        "\n"
        "Usage:\n"
        "  render --manifests <dir> --registry <file> --output <file> \\\n"
        "         --width W --height H [--fps N] [options]\n"
        "\n"
        "Required:\n"
        "  --manifests <dir>       Directory with manifest .bin files\n"
        "  --registry <file>       registry.bin from build step\n"
        "  --output <file>         Output video file (.mov)\n"
        "  --width <n>             Output frame width\n"
        "  --height <n>            Output frame height (auto-computed from source\n"
        "                           aspect ratio if omitted)\n"
        "  --fps <n>               Output frame rate (auto-detect from source if omitted)\n"
        "\n"
        "Options:\n"
        "  --channels <n>          1=grayscale (default), 3=BGR color\n"
        "  --codec <name>          FFmpeg codec (default: auto-detect best available)\n"
        "  --pix-fmt <name>        Pixel format (default: auto from codec)\n"
        "  --no-hw                 Disable hardware encoder, force software (ProRes)\n"
        "  --threads <n>           Thread count for frame assembly (0 = auto)\n"
        "  --max-frames <n>        Process at most N frames (0 = all)\n"
        "  --verbose, -v           Verbose output\n"
        "  --quiet, -q             Suppress non-error output\n"
        "  --json                  JSON output mode\n"
        "  --help, -h              Show this help\n"
        "\n"
        "  Note: When using mise, prefix flags with `--` to avoid interception:\n"
        "    mise run render -- --output result.mov\n"
        "\n"
        "Pipeline:\n"
        "  1. Load registry + manifests\n"
        "  2. Render source pages (PDF/images) to atlas (cached)\n"
        "  3. Assemble frames (OpenMP parallel) → encode → output\n"
    );
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int render_main(int argc, char **argv) {

    const char *man_dir    = cli_opt_str("manifests", "manifests_greedy");
    const char *reg_path   = cli_opt_str("registry", "registry.bin");
    char output_buf[2048];
    snprintf(output_buf, sizeof(output_buf), "%s", cli_opt_str("output", "output.mov"));
    char *output           = output_buf;
    int width              = cli_opt_int("width", 0);
    int height             = cli_opt_int("height", 0);
    double fps             = cli_opt_dbl("fps", 0.0);
    const char *codec      = cli_opt_str("codec", "prores_ks");
    const char *pix_fmt    = cli_opt_str("pix-fmt", "yuv422p10le");
    int threads            = cli_opt_int("threads", 0);
    int max_frames         = cli_opt_int("max-frames", 0);
    int hw                 = !cli_has("no-hw");  /* hardware ON by default */
    int channels           = cli_opt_int("channels", 1);
    if (channels != 1 && channels != 3) channels = 1;

    if (cli_has("help") || cli_has("h") || !cli_has("manifests")) {
        print_help();
        return cli_has("manifests") ? 0 : 1;
    }

    /* ── Detect system resources ──────────────────────────────── */
    SystemConfig sys = system_detect();
    if (threads > 0)
        sys.num_threads = threads;
    cli_set_threads(sys.num_threads);

#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(sys.num_threads);
#endif

    /* ── Auto-detect codec ──────────────────────────────────── */
    int use_vt_prores = 0;  /* flag: use AVFoundation ProRes (macOS Apple Silicon) */

    if (hw) {
        /* Priority 1: Hardware ProRes via AVFoundation (macOS Apple Silicon).
         * ProRes is the native codec for Apple Silicon's dedicated encode engine.
         * Outputs .mov (QuickTime container). */
        if (vt_prores_available()) {
            use_vt_prores = 1;
            /* Ensure .mov extension for ProRes */
            if (output) {
                size_t olen = strlen(output);
                if (olen >= 4 && strcasecmp(output + olen - 4, ".mp4") == 0) {
                    cli_info("note: output extension changed to .mov for hardware ProRes");
                    if (olen >= 4) strcpy(output + olen - 3, "mov");
                } else if (olen < 4 || output[olen - 4] != '.') {
                    if (olen + 5 <= sizeof(output_buf)) {
                        strcat(output, ".mov");
                        cli_info("note: appended .mov extension for hardware ProRes");
                    }
                }
            }
            cli_info("hw encoder: prores (AVFoundation)");
        } else {
            /* Priority 2: FFmpeg hardware encoder (VideoToolbox/VAAPI H.264/HEVC).
             * Outputs .mp4. */
            const char *hw_codec = video_probe_hw_encoder();
            if (hw_codec) {
                codec = hw_codec;
                pix_fmt = "yuv420p";
                /* Auto-switch .mov → .mp4 for non-ProRes HW codecs */
                if (output) {
                    size_t olen = strlen(output);
                    if (olen >= 4 && strcasecmp(output + olen - 4, ".mov") == 0) {
                        cli_info("note: output extension changed to .mp4 for hardware codec");
                        strcpy(output + olen - 3, "mp4");
                    } else if (olen < 4 || output[olen - 4] != '.') {
                        if (olen + 5 <= sizeof(output_buf)) {
                            strcat(output, ".mp4");
                            cli_info("note: appended .mp4 extension for hardware codec");
                        }
                    }
                }
                cli_info("hw encoder: %s (%s)", codec, pix_fmt);
            } else {
                /* Priority 3: No HW encoder — use software ProRes */
                codec = "prores_ks";
                pix_fmt = "yuv422p10le";
                cli_info("no hw encoder found, using software prores_ks");
            }
        }
    }
    /* If --no-hw, use whatever --codec and --pix-fmt were set (defaults: prores_ks / yuv422p10le).
     * Ensure .mov extension for software ProRes if no extension given. */
    if (!hw && output && strstr(codec, "prores")) {
        size_t olen = strlen(output);
        if (olen < 4 || output[olen - 4] != '.') {
            if (olen + 5 <= sizeof(output_buf)) {
                strcat(output, ".mov");
                cli_info("note: appended .mov extension for ProRes");
            }
        }
    }

    cli_info("system: %ld cores | %zu MB RAM | %d threads | cache %s (%zu MB)",
             sys.cpu_cores,
             sys.total_memory_bytes / (1024 * 1024),
             sys.num_threads,
             sys.cache_enabled ? "on" : "off",
             sys.cache_budget_bytes / (1024 * 1024));

    /* ── Load registry ────────────────────────────────────────── */
    Registry reg;
    if (ba_load_registry(reg_path, &reg) != 0) {
        cli_die("cannot load registry: %s", reg_path);
    }
    cli_info("registry: %d entries", reg.n);

    /* ── Scan manifests directory ─────────────────────────────── */
    DIR *d = opendir(man_dir);
    if (!d) cli_die("cannot open manifests dir: %s", man_dir);

    char **manifest_paths = NULL;
    int n_manifests = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!has_suffix(de->d_name, ".bin")) continue;
        if (strcmp(de->d_name, "fps.bin") == 0) continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", man_dir, de->d_name);
        if (n_manifests >= cap) {
            cap = cap ? cap * 2 : 256;
            char **tmp = realloc(manifest_paths, (size_t)cap * sizeof(char *));
            if (!tmp) { free(manifest_paths); cli_die("out of memory"); }
            manifest_paths = tmp;
        }
        char *dup = strdup(full);
        if (!dup) {
            for (int i = 0; i < n_manifests; i++) free(manifest_paths[i]);
            free(manifest_paths);
            cli_die("out of memory");
        }
        manifest_paths[n_manifests++] = dup;
    }
    closedir(d);

    if (n_manifests == 0) cli_die("no manifests found in: %s", man_dir);
    cli_info("manifests: %d frames", n_manifests);

    /* Sort manifests by filename (frame order) */
    qsort(manifest_paths, (size_t)n_manifests, sizeof(char *), cmp_manifest);

    /* ── Auto-detect source dimensions from first manifest ──────────
     * Read src_w/src_h from the manifest header (first 8 bytes) to compute
     * output dimensions that preserve the source aspect ratio. */
    {
        FILE *pf = fopen(manifest_paths[0], "rb");
        if (pf) {
            uint32_t msw = 0, msh = 0;
            if (fread(&msw, 4, 1, pf) == 1 && fread(&msh, 4, 1, pf) == 1 && msw > 0 && msh > 0) {
                double src_aspect = (double)msw / (double)msh;
                if (width <= 0 && height <= 0) {
                    /* Neither specified: match source resolution */
                    width = (int)msw; height = (int)msh;
                } else if (width > 0 && height <= 0) {
                    /* Width only: compute height from aspect ratio */
                    height = (int)((double)width / src_aspect + 0.5);
                    height = (height / 2) * 2;
                } else if (height > 0 && width <= 0) {
                    /* Height only: compute width from aspect ratio */
                    width = (int)((double)height * src_aspect + 0.5);
                    width = (width / 2) * 2;
                } else {
                    /* Both specified: fit within bounding box preserving aspect.
                     * E.g. 7680×4320 with 4:3 source → 5760×4320 (pillarbox). */
                    double dst_aspect = (double)width / (double)height;
                    if (dst_aspect > src_aspect) {
                        /* Output is wider than source → shrink width */
                        width = (int)((double)height * src_aspect + 0.5);
                        width = (width / 2) * 2;
                    } else {
                        /* Output is taller than source → shrink height */
                        height = (int)((double)width / src_aspect + 0.5);
                        height = (height / 2) * 2;
                    }
                }
            }
            fclose(pf);
        }
        if (width <= 0) width = 7680;
        if (height <= 0) height = 4320;
    }

    /* ── Pre-load all manifests into memory ───────────────────── */
    typedef struct { Inst *insts; int n; } LoadedManifest;
    LoadedManifest *loaded = (LoadedManifest *)calloc((size_t)n_manifests, sizeof(LoadedManifest));
    if (!loaded) cli_die("cannot allocate loaded manifests");
    int loaded_count = 0;
    for (int i = 0; i < n_manifests; i++) {
        Inst *insts = NULL; int n = 0;
        if (load_manifest(manifest_paths[i], &insts, &n, width, height) != 0) {
            cli_warn("skip bad manifest: %s", manifest_paths[i]);
            loaded[i].insts = NULL;
            loaded[i].n = 0;
            continue;
        }
        loaded[i].insts = insts;
        loaded[i].n = n;
        loaded_count++;
    }
    cli_info("loaded: %d manifests (%d frames)", loaded_count, n_manifests);

    /* Auto-detect fps from sidecar written by arrange.
     * If --fps was explicitly specified, use it but warn if it differs. */
    {
        double src_fps = 0.0;
        char fps_path[2048];
        snprintf(fps_path, sizeof(fps_path), "%s/fps.bin", man_dir);
        FILE *fpsf = fopen(fps_path, "rb");
        if (fpsf) {
            if (fread(&src_fps, sizeof(double), 1, fpsf) != 1) {
                src_fps = 0.0;
            }
            fclose(fpsf);
        }
        if (fps <= 0.0) {
            /* No --fps specified: use fps.bin */
            if (src_fps > 0.0) {
                fps = src_fps;
                cli_info("auto-detected fps: %.2f (from source video)", fps);
            }
        } else if (src_fps > 0.0 && fabs(fps - src_fps) > 0.1) {
            /* --fps specified but differs from source: warn */
            cli_warn("fps mismatch: --fps=%.2f but source video is %.2f fps "
                     "(output may play at wrong speed)", fps, src_fps);
        }
    }
    if (fps <= 0.0) fps = 30.0;  /* safe fallback */

    cli_info("render: %dx%d @ %.1f fps %s → %s", width, height, fps,
             channels == 3 ? "(color)" : "(grayscale)", output);

    /* ── Open encoder ─────────────────────────────────────────── */
    VideoEncoder *ve = NULL;
    VTEncoder *vt = NULL;

    if (use_vt_prores) {
        /* Hardware ProRes via AVFoundation (macOS Apple Silicon) */
        int profile = 2; /* HQ */
        if (codec && strstr(codec, "4444XQ")) profile = 4;
        else if (codec && strstr(codec, "4444")) profile = 3;
        else if (codec && strstr(codec, "LT")) profile = 0;
        else if (codec && strstr(codec, "proRes422")) profile = 1;
        vt = vt_prores_open(output, width, height, fps, profile, channels);
        if (vt) {
            cli_info("hw prores: AVFoundation (profile %d, %s)", profile,
                     channels == 1 ? "grayscale" : "color");
        } else {
            /* VT ProRes failed — try FFmpeg HW encoder, then software */
            cli_info("hw prores: AVFoundation unavailable, falling back to FFmpeg");
            const char *hw_codec = video_probe_hw_encoder();
            if (hw_codec) {
                codec = hw_codec;
                pix_fmt = "yuv420p";
                /* Switch extension from .mov to .mp4 */
                if (output) {
                    size_t olen = strlen(output);
                    if (olen >= 4 && strcasecmp(output + olen - 4, ".mov") == 0) {
                        strcpy(output + olen - 3, "mp4");
                    }
                }
                cli_info("hw encoder fallback: %s (%s)", codec, pix_fmt);
            } else {
                codec = "prores_ks";
                pix_fmt = "yuv422p10le";
            }
        }
    }

    if (!vt) {
        ve = video_encoder_open(output, width, height, fps, pix_fmt, codec);
        if (!ve) cli_die("cannot open encoder: %s", output);
    }

    /* ── Single canvas buffer (pipeline_push copies into queue) ── */
    size_t canvas_bytes = (size_t)width * (size_t)height * (size_t)channels;
    uint8_t *canvas = (uint8_t *)calloc(canvas_bytes, 1);
    if (!canvas) cli_die("cannot allocate canvas");

    /* ── Atlas cache ──────────────────────────────────────────── */
    AtlasCache atlas;
    atlas_init(&atlas, sys.cache_budget_bytes);

    /* ── Initialize encode pipeline ───────────────────────────── */
    EncodePipeline ep;
    pipeline_init(&ep, ve, vt, width, height, channels, canvas_bytes);
    pthread_t enc_thread;
    pthread_create(&enc_thread, NULL, encoder_thread, &ep);

    /* ── Process frames ───────────────────────────────────────── */
    int frames_done = 0;
    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int max_frames_actual = (max_frames > 0 && max_frames < n_manifests) ? max_frames : n_manifests;
    if (max_frames_actual < n_manifests)
        cli_info("max-frames: %d (of %d)", max_frames_actual, n_manifests);

    for (int fi = 0; fi < max_frames_actual; fi++) {
        Inst *insts = loaded[fi].insts;
        int n = loaded[fi].n;

        /* Clear canvas */
        memset(canvas, 0, canvas_bytes);

        if (n > 0 && insts) {
            /* ── Pre-populate atlas cache for this frame's tiles ── */
            if (atlas.enabled) {
                for (int i = 0; i < n; i++) {
                    if (insts[i].op_id >= 0 && insts[i].w > 0 && insts[i].h > 0)
                        atlas_cache_tile(&atlas, &reg, insts[i].op_id,
                                         insts[i].w, insts[i].h, channels);
                }
            }

            /* ── Blit instructions (OpenMP parallel) ───────────────────── */
#ifdef _OPENMP
            #pragma omp parallel for schedule(dynamic) if(n > 4)
#endif
            for (int i = 0; i < n; i++) {
                if (insts[i].op_id < 0) {
                    /* Solid color — row-wise fill */
                    uint8_t val = (insts[i].op_id == -2) ? 255 : 0;
                    int sy0 = insts[i].y, sx0 = insts[i].x;
                    for (int yy = 0; yy < insts[i].h; yy++) {
                        int dst_y = sy0 + yy;
                        if (dst_y < 0 || dst_y >= height) continue;
                        int fill_x = sx0, fill_w = insts[i].w;
                        if (fill_x < 0) { fill_w += fill_x; fill_x = 0; }
                        if (fill_x + fill_w > width) fill_w = width - fill_x;
                        if (fill_w > 0) {
                            if (channels == 3) {
                                uint8_t *dst = &canvas[((size_t)dst_y * width + fill_x) * 3];
                                for (int px = 0; px < fill_w; px++) {
                                    dst[px*3+0] = val;
                                    dst[px*3+1] = val;
                                    dst[px*3+2] = val;
                                }
                            } else {
                                memset(&canvas[(size_t)dst_y * width + fill_x], val, (size_t)fill_w);
                            }
                        }
                    }
                    continue;
                }

                if (insts[i].op_id >= reg.n) continue;

                int dw = insts[i].w, dh = insts[i].h;
                if (dw <= 0 || dh <= 0) continue;

                /* Check tile-level atlas cache first */
                AtlasEntry *cached = atlas_lookup(&atlas, insts[i].op_id, dw, dh);

                Img tile_img;
                int need_free = 0;

                if (cached) {
                    /* Cache hit — tile already scaled to (dw × dh) */
                    tile_img = cached->tile;
                } else {
                    /* Cache miss — render source page and scale on-the-fly */
                    const char *pdf_path = reg.entries[insts[i].op_id].pdf_path;
                    int page_idx = reg.entries[insts[i].op_id].page_idx;

                    Img src;
                    if (pdf_render_page(pdf_path, page_idx, 1.0f, &src) != 0) continue;
                    if (channels == 3) {
                        /* Keep BGR — scale directly */
                        img_resize_area(&src, &tile_img, dw, dh);
                        img_free(&src);
                    } else {
                        img_to_gray(&src, &tile_img);
                        img_free(&src);
                        /* Scale directly to output tile size */
                        Img scaled;
                        img_resize_area(&tile_img, &scaled, dw, dh);
                        img_free(&tile_img);
                        tile_img = scaled;
                    }
                    need_free = 1;
                }

                /* Blit to canvas — row-wise memcpy (disjoint region, no lock needed) */
                int sy0 = insts[i].y, sx0 = insts[i].x;
                int tile_channels = tile_img.channels;
                for (int yy = 0; yy < dh; yy++) {
                    int dst_y = sy0 + yy;
                    if (dst_y < 0 || dst_y >= height) continue;
                    int copy_x = sx0, copy_src_x = 0;
                    int copy_w = dw;
                    if (copy_x < 0) { copy_src_x = -copy_x; copy_w += copy_x; copy_x = 0; }
                    if (copy_x + copy_w > width) copy_w = width - copy_x;
                    if (copy_w > 0) {
                        size_t copy_bytes = (size_t)copy_w * tile_channels;
                        memcpy(&canvas[((size_t)dst_y * width + copy_x) * channels],
                               &tile_img.pixels[(size_t)yy * tile_img.stride + copy_src_x * tile_channels],
                               copy_bytes);
                    }
                }
                if (need_free) img_free(&tile_img);
            }
        }

        /* ── Push assembled frame to encoder pipeline ──────────────── */
        pipeline_push(&ep, canvas, canvas_bytes);

        frames_done++;
        if (!g_cli.quiet && frames_done % 30 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double el = (ts_now.tv_sec - ts_start.tv_sec) + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            double fps_out = frames_done / (el > 0 ? el : 1);
            double cache_pct = (atlas.hits + atlas.misses > 0)
                ? (double)atlas.hits / (double)(atlas.hits + atlas.misses) * 100.0
                : 0.0;
            cli_progress_frame("render", frames_done, n_manifests, fps_out, cache_pct);
        }
    }

    /* ── Flush encoder and join thread ─────────────────────────── */
    pipeline_flush(&ep);
    pthread_join(enc_thread, NULL);

    /* ── Cleanup ──────────────────────────────────────────────── */
    if (vt) vt_prores_close(vt);
    else video_encoder_close(ve);
    pipeline_shutdown(&ep);
    pthread_mutex_destroy(&ep.mutex);
    pthread_cond_destroy(&ep.can_write);
    pthread_cond_destroy(&ep.can_read);
    atlas_free(&atlas);
    free(canvas);

    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    double total_time = (ts_now.tv_sec - ts_start.tv_sec) + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
    double out_fps = frames_done / (total_time > 0 ? total_time : 1.0);

    long cache_lookups = atlas.hits + atlas.misses;
    double cache_hit_pct = cache_lookups > 0 ? 100.0 * atlas.hits / cache_lookups : 0.0;

    char summary[256];
    snprintf(summary, sizeof(summary),
             "render complete in %.2fs | %.2f fps | %d frames | cache %.1f%%",
             total_time, out_fps, frames_done, cache_hit_pct);
    cli_progress_done(summary);

    for (int i = 0; i < n_manifests; i++) {
        if (loaded[i].insts) free(loaded[i].insts);
        free(manifest_paths[i]);
    }
    free(loaded);
    free(manifest_paths);
    ba_free_registry(&reg);

    return 0;
}
