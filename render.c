/*
 * render.c — pure-C render stage with CLI integration.
 *
 * Reads manifests_greedy/*.bin, looks up source images from registry,
 * assembles frames, and writes them to an output file via libav.
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

#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_MANIFESTS 100000
#define MAX_INSTS     65536

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
        "  --codec <name>          FFmpeg codec (default: prores_ks)\n"
        "  --pix-fmt <name>        Pixel format (default: yuv422p10le)\n"
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
        "  2. Render source pages (PDF/images) to grayscale atlas\n"
        "  3. Assemble frames → pipe raw video to ffmpeg → output.mov\n"
    );
}

int main(int argc, char **argv) {
    cli_init();
    cli_parse(argc, argv);

    const char *man_dir    = cli_opt_str("manifests", "manifests_greedy");
    const char *reg_path   = cli_opt_str("registry", "registry.bin");
    const char *output     = cli_opt_str("output", "output.mov");
    int width              = cli_opt_int("width", 7680);
    int height             = cli_opt_int("height", 4320);
    double fps             = cli_opt_dbl("fps", 60.0);
    const char *codec      = cli_opt_str("codec", "prores_ks");
    const char *pix_fmt    = cli_opt_str("pix-fmt", "yuv422p10le");
    int threads            = cli_opt_int("threads", 0);
    int max_frames         = cli_opt_int("max-frames", 0);
    cli_set_threads(threads);

    if (cli_has("help") || cli_has("h") || !cli_has("manifests")) {
        print_help();
        return cli_has("manifests") ? 0 : 1;
    }

    cli_info("render: %dx%d @ %.1f fps → %s", width, height, fps, output);

    /* Load registry */
    Registry reg;
    if (ba_load_registry(reg_path, &reg) != 0) {
        cli_die("cannot load registry: %s", reg_path);
    }
    cli_info("registry: %d entries", reg.n);

    /* Scan manifests directory */
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

    /* Build atlas: render each unique (pdf_path, page_idx) to a grayscale image */
    /* Note: source pages are rendered on-the-fly per frame (no atlas cache yet). */

    /* Open ffmpeg encoder */
    VideoEncoder *ve = video_encoder_open(output, width, height, fps, pix_fmt, codec);
    if (!ve) cli_die("cannot open encoder: %s", output);

    /* Process frames */
    int frames_done = 0;
    clock_t start = clock();

    int max_frames_actual = (max_frames > 0 && max_frames < n_manifests) ? max_frames : n_manifests;
    if (max_frames_actual < n_manifests)
        cli_info("max-frames: %d (of %d)", max_frames_actual, n_manifests);

    /* Pre-allocate grayscale canvas (reused across frames) */
    size_t canvas_bytes = (size_t)width * height;
    uint8_t *canvas = (uint8_t *)calloc(canvas_bytes, 1);
    if (!canvas) cli_die("cannot allocate canvas");

    for (int fi = 0; fi < max_frames_actual; fi++) {
        Inst *insts = NULL; int n = 0;
        if (load_manifest(manifest_paths[fi], &insts, &n) != 0) {
            cli_warn("skip bad manifest: %s", manifest_paths[fi]);
            continue;
        }

        /* Clear canvas for this frame (memset is faster than calloc for reuse) */
        memset(canvas, 0, canvas_bytes);

        /* Blit each instruction */
        for (int i = 0; i < n; i++) {
            if (insts[i].op_id < 0) {
                /* Solid color */
                if (insts[i].op_id == -1) {
                    for (int yy = insts[i].y; yy < insts[i].y + insts[i].h && yy < height; yy++) {
                        if (yy < 0) continue;
                        for (int xx = insts[i].x; xx < insts[i].x + insts[i].w && xx < width; xx++) {
                            if (xx < 0) continue;
                            canvas[(size_t)yy * width + xx] = 0;
                        }
                    }
                } else if (insts[i].op_id == -2) {
                    for (int yy = insts[i].y; yy < insts[i].y + insts[i].h && yy < height; yy++) {
                        if (yy < 0) continue;
                        for (int xx = insts[i].x; xx < insts[i].x + insts[i].w && xx < width; xx++) {
                            if (xx < 0) continue;
                            canvas[(size_t)yy * width + xx] = 255;
                        }
                    }
                }
                continue;
            }

            /* Look up source */
            if (insts[i].op_id >= reg.n) continue;
            const char *pdf_path = reg.entries[insts[i].op_id].pdf_path;
            int page_idx = reg.entries[insts[i].op_id].page_idx;

            /* Render source page (full resolution) */
            Img src;
            if (pdf_render_page(pdf_path, page_idx, 3.0f, &src) != 0) continue;

            /* Convert to grayscale */
            Img gray;
            img_to_gray(&src, &gray);
            img_free(&src);

            /* Scale to fit block */
            int dw = insts[i].w, dh = insts[i].h;
            Img scaled;
            img_resize_area(&gray, &scaled, dw, dh);
            img_free(&gray);

            /* Blit to canvas */
            for (int yy = 0; yy < dh && (insts[i].y + yy) < height; yy++) {
                int sy = insts[i].y + yy;
                if (sy < 0) continue;
                for (int xx = 0; xx < dw && (insts[i].x + xx) < width; xx++) {
                    int sx = insts[i].x + xx;
                    if (sx < 0) continue;
                    canvas[(size_t)sy * width + sx] = scaled.pixels[(size_t)yy * scaled.stride + xx];
                }
            }
            img_free(&scaled);
        }

        /* Encode frame */
        Img frame;
        frame.w = width; frame.h = height; frame.channels = 1;
        frame.stride = width; frame.pixels = canvas;
        video_encoder_write(ve, &frame);
        free(insts);

        frames_done++;
        if (!g_cli.quiet && frames_done % 30 == 0) {
            double el = (double)(clock() - start) / CLOCKS_PER_SEC;
            double fps_out = frames_done / (el > 0 ? el : 1);
            cli_progress_frame(frames_done, n_manifests, fps_out, 0);
        }
    }

    video_encoder_close(ve);
    free(canvas);

    double total_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    double out_fps = frames_done / (total_time > 0 ? total_time : 1.0);

    char summary[256];
    snprintf(summary, sizeof(summary),
             "render complete in %.2fs | %.2f fps | %d frames",
             total_time, out_fps, frames_done);
    cli_progress_done(summary);

    /* cleanup */
    for (int i = 0; i < n_manifests; i++) free(manifest_paths[i]);
    free(manifest_paths);
    ba_free_registry(&reg);

    return 0;
}
