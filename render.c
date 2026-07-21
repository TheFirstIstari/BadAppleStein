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
 *          --width W --height H --fps 60 [--threads N] [--verbose] [--quiet] [--json]
 */
#include "badapple.h"
#include "imgops.h"
#include "video.h"
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
    return strcmp(*(const char **)a, *(const char **)b);
}

static int load_manifest(const char *path, Inst **out, int *nout) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 4) { fclose(f); return -1; }

    uint32_t n = 0;
    fread(&n, 4, 1, f);
    if (n > MAX_INSTS) { fclose(f); return -1; }

    Inst *insts = (Inst *)malloc((size_t)n * sizeof(Inst));
    for (uint32_t i = 0; i < n; i++) {
        int32_t b[6];
        if (fread(b, 4, 6, f) != 6) { free(insts); fclose(f); return -1; }
        insts[i].x = b[0]; insts[i].y = b[1]; insts[i].w = b[2];
        insts[i].h = b[3]; insts[i].op_id = b[4]; insts[i].page_idx = b[5];
    }
    fclose(f);
    *out = insts; *nout = (int)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Atlas cache — render each unique source page once                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int      op_id;        /* key: registry index (-1/-2 for solid colors) */
    Img      gray;         /* cached grayscale image (owner) */
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
}

static void atlas_free(AtlasCache *atlas) {
    for (int i = 0; i < atlas->capacity; i++) {
        if (atlas->entries[i].valid) img_free(&atlas->entries[i].gray);
    }
    free(atlas->entries);
    atlas->entries = NULL;
    atlas->count = 0;
    atlas->total_bytes = 0;
}

static unsigned atlas_hash(int op_id) {
    unsigned h = (unsigned)op_id * 2654435761u;
    return h ? h : 1;
}

/* Thread-safe read-only lookup (no shared state mutation) */
static AtlasEntry *atlas_lookup(AtlasCache *atlas, int op_id) {
    if (!atlas->enabled) return NULL;
    unsigned h = atlas_hash(op_id);
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (!atlas->entries[idx].valid) return NULL;
        if (atlas->entries[idx].op_id == op_id)
            return &atlas->entries[idx];
    }
    return NULL;
}

/* Single-threaded lookup that tracks hits/misses (for pre-population) */
static AtlasEntry *atlas_lookup_stats(AtlasCache *atlas, int op_id) {
    if (!atlas->enabled) { atlas->misses++; return NULL; }
    unsigned h = atlas_hash(op_id);
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (!atlas->entries[idx].valid) { atlas->misses++; return NULL; }
        if (atlas->entries[idx].op_id == op_id) {
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
        if (atlas->entries[i].valid && atlas->entries[i].lru < oldest_tick) {
            oldest_tick = atlas->entries[i].lru;
            oldest_idx = i;
        }
    }
    if (oldest_idx >= 0) {
        atlas->total_bytes -= atlas->entries[oldest_idx].bytes;
        img_free(&atlas->entries[oldest_idx].gray);
        atlas->entries[oldest_idx].valid = 0;
        atlas->count--;
    }
}

static void atlas_insert(AtlasCache *atlas, int op_id, const Img *gray) {
    if (!atlas->enabled) return;

    /* Evict until we have room (each entry is ~w*h bytes) */
    size_t entry_bytes = (size_t)gray->w * gray->h;
    while (atlas->total_bytes + entry_bytes > atlas->budget && atlas->count > 0)
        atlas_evict_lru(atlas);

    /* Grow if needed */
    if (atlas->count >= atlas->capacity * 3 / 4) {
        int old_cap = atlas->capacity;
        atlas->capacity *= 2;
        AtlasEntry *new_entries = (AtlasEntry *)calloc((size_t)atlas->capacity, sizeof(AtlasEntry));
        /* Rehash existing entries */
        for (int i = 0; i < old_cap; i++) {
            if (atlas->entries[i].valid) {
                unsigned h = atlas_hash(atlas->entries[i].op_id);
                for (int p = 0; p < atlas->capacity; p++) {
                    int idx = (int)((h + (unsigned)p) % (unsigned)atlas->capacity);
                    if (!new_entries[idx].valid) {
                        new_entries[idx] = atlas->entries[i];
                        break;
                    }
                }
            }
        }
        free(atlas->entries);
        atlas->entries = new_entries;
    }

    /* Insert */
    unsigned h = atlas_hash(op_id);
    for (int probe = 0; probe < atlas->capacity; probe++) {
        int idx = (int)((h + (unsigned)probe) % (unsigned)atlas->capacity);
        if (!atlas->entries[idx].valid) {
            atlas->entries[idx].op_id = op_id;
            atlas->entries[idx].gray.pixels = NULL;
            img_resize_area(gray, &atlas->entries[idx].gray, gray->w, gray->h);
            atlas->entries[idx].bytes = (size_t)gray->w * gray->h;
            atlas->entries[idx].valid = 1;
            atlas->entries[idx].lru = atlas->tick++;
            atlas->total_bytes += atlas->entries[idx].bytes;
            atlas->count++;
            return;
        }
    }
}

/* Pre-render a source page into grayscale and cache it */
static void atlas_cache_page(AtlasCache *atlas, const Registry *reg, int op_id) {
    if (op_id < 0 || op_id >= reg->n) return;
    if (atlas_lookup_stats(atlas, op_id)) return;  /* already cached */

    const char *pdf_path = reg->entries[op_id].pdf_path;
    int page_idx = reg->entries[op_id].page_idx;

    Img src;
    if (pdf_render_page(pdf_path, page_idx, 3.0f, &src) != 0) return;

    Img gray;
    img_to_gray(&src, &gray);
    img_free(&src);

    atlas_insert(atlas, op_id, &gray);
    img_free(&gray);
}

/* ------------------------------------------------------------------ */
/*  Frame encode pipeline (pthreads producer-consumer queue)           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *pixels;   /* owned copy of frame grayscale data */
    int      valid;    /* 1 = slot holds a frame ready for encoding */
} FrameSlot;

typedef struct {
    FrameSlot     slots[FRAME_QUEUE_SIZE];
    int           write_pos;
    int           read_pos;
    int           count;
    int           stop;          /* signal encoder to exit */
    pthread_mutex_t mutex;
    pthread_cond_t  can_write;   /* main thread waits when queue full */
    pthread_cond_t  can_read;    /* encoder thread waits when queue empty */
    VideoEncoder  *ve;
    VTEncoder    *vt;            /* hardware ProRes encoder (NULL if using FFmpeg) */
    int            width, height;
    int            channels;     /* 1 = grayscale, 3 = BGR color */
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
        /* Take frame from queue */
        FrameSlot slot = ep->slots[ep->read_pos];
        ep->read_pos = (ep->read_pos + 1) % FRAME_QUEUE_SIZE;
        ep->count--;
        pthread_cond_signal(&ep->can_write);
        pthread_mutex_unlock(&ep->mutex);

        /* Encode — dispatch to hardware or software encoder */
        if (ep->vt) {
            vt_prores_write(ep->vt, slot.pixels, ep->width, ep->height,
                            ep->width * ep->channels, ep->channels);
        } else {
            Img frame;
            frame.w = ep->width; frame.h = ep->height;
            frame.channels = ep->channels;
            frame.stride = ep->width * ep->channels;
            frame.pixels = slot.pixels;
            video_encoder_write(ep->ve, &frame);
        }
        free(slot.pixels);
        ep->frames_encoded++;
    }
    return NULL;
}

static void pipeline_init(EncodePipeline *ep, VideoEncoder *ve, VTEncoder *vt,
                           int w, int h, int channels) {
    memset(ep, 0, sizeof(*ep));
    ep->ve = ve;
    ep->vt = vt;
    ep->width = w;
    ep->height = h;
    ep->channels = channels > 0 ? channels : 1;
    pthread_mutex_init(&ep->mutex, NULL);
    pthread_cond_init(&ep->can_write, NULL);
    pthread_cond_init(&ep->can_read, NULL);
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++)
        ep->slots[i].pixels = NULL;
}

static void pipeline_push(EncodePipeline *ep, const uint8_t *pixels, size_t nbytes) {
    pthread_mutex_lock(&ep->mutex);
    while (ep->count >= FRAME_QUEUE_SIZE)
        pthread_cond_wait(&ep->can_write, &ep->mutex);
    int slot = ep->write_pos;
    ep->slots[slot].pixels = (uint8_t *)malloc(nbytes);
    memcpy(ep->slots[slot].pixels, pixels, nbytes);
    ep->slots[slot].valid = 1;
    ep->write_pos = (ep->write_pos + 1) % FRAME_QUEUE_SIZE;
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

/* ------------------------------------------------------------------ */
/*  Help                                                               */
/* ------------------------------------------------------------------ */

static void print_help(void) {
    fprintf(stderr,
        "videomatch render — render final video from manifests\n"
        "\n"
        "Usage:\n"
        "  render --manifests <dir> --registry <file> --output <file> \\\n"
        "         --width W --height H --fps N [options]\n"
        "\n"
        "Required:\n"
        "  --manifests <dir>       Directory with manifest .bin files\n"
        "  --registry <file>       registry.bin from build step\n"
        "  --output <file>         Output video file (.mov)\n"
        "  --width <n>             Output frame width\n"
        "  --height <n>            Output frame height\n"
        "  --fps <n>               Output frame rate\n"
        "\n"
        "Options:\n"
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
        "  2. Render source pages (PDF/images) to grayscale atlas (cached)\n"
        "  3. Assemble frames (OpenMP parallel) → encode → output\n"
    );
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    cli_init();
    cli_parse(argc, argv);

    const char *man_dir    = cli_opt_str("manifests", "manifests_greedy");
    const char *reg_path   = cli_opt_str("registry", "registry.bin");
    char output_buf[2048];
    snprintf(output_buf, sizeof(output_buf), "%s", cli_opt_str("output", "output.mov"));
    char *output           = output_buf;
    int width              = cli_opt_int("width", 7680);
    int height             = cli_opt_int("height", 4320);
    double fps             = cli_opt_dbl("fps", 60.0);
    const char *codec      = cli_opt_str("codec", "prores_ks");
    const char *pix_fmt    = cli_opt_str("pix-fmt", "yuv422p10le");
    int threads            = cli_opt_int("threads", 0);
    int max_frames         = cli_opt_int("max-frames", 0);
    int hw                 = !cli_has("no-hw");  /* hardware ON by default */

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
                    strcpy(output + olen - 3, "mov");
                } else if (olen < 4 || output[olen - 4] != '.') {
                    if (olen + 5 <= 4096) {
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
                        if (olen + 5 <= 4096) {
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
            if (olen + 5 <= 4096) {
                strcat(output, ".mov");
                cli_info("note: appended .mov extension for ProRes");
            }
        }
    }

    cli_info("render: %dx%d @ %.1f fps → %s", width, height, fps, output);
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
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", man_dir, de->d_name);
        if (n_manifests >= cap) {
            cap = cap ? cap * 2 : 256;
            manifest_paths = (char **)realloc(manifest_paths, (size_t)cap * sizeof(char *));
        }
        manifest_paths[n_manifests++] = strdup(full);
    }
    closedir(d);

    if (n_manifests == 0) cli_die("no manifests found in: %s", man_dir);
    cli_info("manifests: %d frames", n_manifests);

    /* Sort manifests by filename (frame order) */
    qsort(manifest_paths, (size_t)n_manifests, sizeof(char *), cmp_manifest);

    /* ── Pre-load all manifests into memory ───────────────────── */
    typedef struct { Inst *insts; int n; } LoadedManifest;
    LoadedManifest *loaded = (LoadedManifest *)calloc((size_t)n_manifests, sizeof(LoadedManifest));
    int loaded_count = 0;
    for (int i = 0; i < n_manifests; i++) {
        Inst *insts = NULL; int n = 0;
        if (load_manifest(manifest_paths[i], &insts, &n) != 0) {
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

    /* ── Open encoder ─────────────────────────────────────────── */
    VideoEncoder *ve = NULL;
    VTEncoder *vt = NULL;
    int channels = 1;  /* current pipeline is grayscale; ready for 3 */

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
    size_t canvas_bytes = (size_t)width * height;
    uint8_t *canvas = (uint8_t *)calloc(canvas_bytes, 1);
    if (!canvas) cli_die("cannot allocate canvas");

    /* ── Atlas cache ──────────────────────────────────────────── */
    AtlasCache atlas;
    atlas_init(&atlas, sys.cache_budget_bytes);

    /* ── Initialize encode pipeline ───────────────────────────── */
    EncodePipeline ep;
    pipeline_init(&ep, ve, vt, width, height, channels);
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
        if (loaded[fi].n == 0) continue;
        Inst *insts = loaded[fi].insts;
        int n = loaded[fi].n;

        /* Clear canvas */
        memset(canvas, 0, canvas_bytes);

        /* ── Pre-populate atlas cache for this frame's source pages ── */
        if (atlas.enabled) {
            for (int i = 0; i < n; i++) {
                if (insts[i].op_id >= 0)
                    atlas_cache_page(&atlas, &reg, insts[i].op_id);
            }
        }

        /* ── Blit instructions (OpenMP parallel) ───────────────────── */
#ifdef _OPENMP
        #pragma omp parallel for schedule(dynamic) if(n > 4)
#endif
        for (int i = 0; i < n; i++) {
            if (insts[i].op_id < 0) {
                /* Solid color — row-wise memset */
                uint8_t val = (insts[i].op_id == -2) ? 255 : 0;
                int sy0 = insts[i].y, sx0 = insts[i].x;
                for (int yy = 0; yy < insts[i].h; yy++) {
                    int dst_y = sy0 + yy;
                    if (dst_y < 0 || dst_y >= height) continue;
                    int fill_x = sx0, fill_w = insts[i].w;
                    if (fill_x < 0) { fill_w += fill_x; fill_x = 0; }
                    if (fill_x + fill_w > width) fill_w = width - fill_x;
                    if (fill_w > 0)
                        memset(&canvas[(size_t)dst_y * width + fill_x], val, (size_t)fill_w);
                }
                continue;
            }

            if (insts[i].op_id >= reg.n) continue;

            /* Check atlas cache first */
            AtlasEntry *cached = atlas_lookup(&atlas, insts[i].op_id);

            Img *gray;
            Img gray_local;
            int need_free = 0;

            if (cached) {
                gray = &cached->gray;
            } else {
                /* Render source page on-the-fly (cache disabled or budget exceeded) */
                const char *pdf_path = reg.entries[insts[i].op_id].pdf_path;
                int page_idx = reg.entries[insts[i].op_id].page_idx;

                Img src;
                if (pdf_render_page(pdf_path, page_idx, 3.0f, &src) != 0) continue;
                img_to_gray(&src, &gray_local);
                img_free(&src);
                gray = &gray_local;
                need_free = 1;
            }

            /* Scale to fit block */
            int dw = insts[i].w, dh = insts[i].h;
            Img scaled;
            img_resize_area(gray, &scaled, dw, dh);
            if (need_free) img_free(&gray_local);

            /* Blit to canvas — row-wise memcpy (disjoint region, no lock needed) */
            int sy0 = insts[i].y, sx0 = insts[i].x;
            for (int yy = 0; yy < dh; yy++) {
                int dst_y = sy0 + yy;
                if (dst_y < 0 || dst_y >= height) continue;
                int copy_x = sx0, copy_src_x = 0;
                int copy_w = dw;
                if (copy_x < 0) { copy_src_x = -copy_x; copy_w += copy_x; copy_x = 0; }
                if (copy_x + copy_w > width) copy_w = width - copy_x;
                if (copy_w > 0) {
                    memcpy(&canvas[(size_t)dst_y * width + copy_x],
                           &scaled.pixels[(size_t)yy * scaled.stride + copy_src_x],
                           (size_t)copy_w);
                }
            }
            img_free(&scaled);
        }

        /* ── Push assembled frame to encoder pipeline ──────────────── */
        pipeline_push(&ep, canvas, canvas_bytes * channels);

        frames_done++;
        if (!g_cli.quiet && frames_done % 30 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double el = (ts_now.tv_sec - ts_start.tv_sec) + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            double fps_out = frames_done / (el > 0 ? el : 1);
            cli_progress_frame(frames_done, n_manifests, fps_out, 0);
        }
    }

    /* ── Flush encoder and join thread ─────────────────────────── */
    pipeline_flush(&ep);
    pthread_join(enc_thread, NULL);

    /* ── Cleanup ──────────────────────────────────────────────── */
    if (vt) vt_prores_close(vt);
    else video_encoder_close(ve);
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
