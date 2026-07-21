/*
 * build_library.c — build a matching library from images + PDFs (pure C).
 *
 * Walks a directory: every *.pdf contributes its pages; every image file
 * (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff) contributes one entry.
 * Optionally also a JSON manifest listing explicit sources.
 *
 * Output:
 *   features.bin : uint32 n, N, G, channels ; n*N*N*channels bytes
 *   registry.bin : uint32 n ; per entry int32 page_idx, uint32 path_len, path
 *
 * Usage:
 *   build_library <sources_dir> [--feat N] [--bits G] [--color] [--multi-scale]
 *                  [--out features.bin] [--verbose] [--quiet] [--json]
 */
#include "badapple.h"
#include "imgops.h"
#include "video.h"
#include "pdf.h"
#include "cli.h"

#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

static int has_ext(const char *p, const char *ext) {
    const char *dot = strrchr(p, '.');
    return dot && strcasecmp(dot, ext) == 0;
}
static int is_pdf(const char *p) { return has_ext(p, ".pdf"); }
static int is_img(const char *p) {
    return has_ext(p, ".png") || has_ext(p, ".jpg") || has_ext(p, ".jpeg") ||
           has_ext(p, ".bmp") || has_ext(p, ".gif") || has_ext(p, ".tif") || has_ext(p, ".tiff");
}

static int load_image_av(const char *path, Img *out) {
    VideoDecoder *vd = video_decoder_open(path);
    if (!vd) return -1;
    int r = video_decoder_next(vd, out);
    video_decoder_close(vd);
    return (r == 1) ? 0 : -1;
}

typedef struct { uint8_t *data; int len; int n; } FeatBuf;

static void feat_push(FeatBuf *fb, const uint8_t *feat, int len) {
    uint8_t *nw = (uint8_t *)realloc(fb->data, (size_t)(fb->len + len));
    if (!nw) return;
    memcpy(nw + fb->len, feat, (size_t)len);
    fb->data = nw; fb->len += len; fb->n += 1;
}

static void print_help(void) {
    fprintf(stderr,
        "videomatch build — build a source library from PDFs and images\n"
        "\n"
        "Usage:\n"
        "  build_library <sources_dir> [options]\n"
        "\n"
        "Arguments:\n"
        "  sources_dir              Directory containing PDFs and/or images\n"
        "\n"
        "Options:\n"
        "  --feat <n>               Feature grid N (default: 64)\n"
        "  --bits <n>               Bits per cell G, 1-8 (default: 1)\n"
        "  --color                  Enable color matching (3 channels)\n"
        "  --multi-scale            Emit 0.5x, 1.0x, 1.5x, 2.0x variants\n"
        "  --out <file>             Output features.bin (default: features.bin)\n"
        "  --threads <n>            Thread count for feature extraction (0 = auto)\n"
        "  --verbose, -v            Verbose output\n"
        "  --quiet, -q              Suppress non-error output\n"
        "  --json                   JSON output mode\n"
        "  --help, -h               Show this help\n"
        "\n"
        "  Note: When using mise, prefix flags with `--` to avoid interception:\n"
        "    mise run build-library -- --help\n"
        "\n"
        "Output:\n"
        "  features.bin  Binary feature vectors\n"
        "  registry.bin  Binary page index\n"
    );
}

int main(int argc, char **argv) {
    cli_init();
    cli_parse(argc, argv);

    if (cli_has("help") || cli_has("h") || argc < 2) {
        print_help();
        return argc < 2 ? 1 : 0;
    }

    const char *src = argv[1];
    int N = cli_opt_int("feat", 64);
    int G = cli_opt_int("bits", 1);
    int color = cli_opt_bool("color", 0);
    int multiscale = cli_opt_bool("multi-scale", 0);
    const char *feat_out = cli_opt_str("out", "features.bin");
    const char *reg_out = "registry.bin";
    int threads = cli_opt_int("threads", 0);
    cli_set_threads(threads);

    int channels = color ? 3 : 1;
    int feat_len = N * N * channels;

    char **paths = NULL;
    int *page_idxs = NULL;
    int nreg = 0, cap = 0;
    FeatBuf fb = {0};

    float scales[4] = {1.0f};
    int nscales = 1;
    if (multiscale) {
        float s[4] = {0.5f, 1.0f, 1.5f, 2.0f};
        memcpy(scales, s, sizeof(s));
        nscales = 4;
    }

    DIR *d = opendir(src);
    if (!d) {
        cli_die("cannot open sources dir: %s", src);
    }

    int total_sources = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", src, de->d_name);
        if (is_pdf(full)) {
            /* count pages */
            for (int pg = 0; ; pg++) {
                Img im;
                if (pdf_render_page(full, pg, 1.0f, &im) != 0) break;
                img_free(&im);
                total_sources++;
            }
        } else if (is_img(full)) {
            total_sources++;
        }
    }
    closedir(d);

    if (total_sources == 0) {
        cli_die("no sources found in: %s", src);
    }

    cli_info("sources: %d entries | N=%d G=%d ch=%d | scales=%d",
             total_sources, N, G, channels, nscales);

    d = opendir(src);
    if (!d) cli_die("cannot reopen sources dir: %s", src);

    int processed = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", src, de->d_name);

        if (is_pdf(full)) {
            for (int pg = 0; ; pg++) {
                for (int si = 0; si < nscales; si++) {
                    Img sim;
                    if (pdf_render_page(full, pg, scales[si], &sim) != 0) continue;
                    uint8_t *feat = (uint8_t *)malloc(feat_len);
                    img_compute_feature(&sim, N, G, color, feat);
                    img_free(&sim);
                    feat_push(&fb, feat, feat_len);
                    free(feat);
                    if (nreg >= cap) {
                        int ncap = cap ? cap * 2 : 256;
                        char **new_paths = realloc(paths, (size_t)ncap * sizeof(char *));
                        int *new_idxs = realloc(page_idxs, (size_t)ncap * sizeof(int));
                        if (!new_paths || !new_idxs) { free(new_paths); free(new_idxs); free(feat); cli_die("out of memory"); }
                        paths = new_paths;
                        page_idxs = new_idxs;
                        cap = ncap;
                    }
                    paths[nreg] = strdup(full);
                    page_idxs[nreg] = pg;
                    nreg++;
                    processed++;
                }
                if (!g_cli.quiet && processed % 10 == 0) {
                    cli_progress_stage("build", processed * 100 / total_sources);
                }
            }
        } else if (is_img(full)) {
            Img im;
            if (load_image_av(full, &im) != 0) {
                cli_warn("skip unreadable image: %s", full);
                continue;
            }
            for (int si = 0; si < nscales; si++) {
                Img sim;
                if (scales[si] == 1.0f) {
                    sim = im; sim.pixels = NULL;
                } else {
                    img_resize_area(&im, &sim, im.w * (int)scales[si], im.h * (int)scales[si]);
                }
                uint8_t *feat = (uint8_t *)malloc(feat_len);
                img_compute_feature(&sim, N, G, color, feat);
                if (scales[si] != 1.0f) img_free(&sim);
                feat_push(&fb, feat, feat_len);
                free(feat);
                if (nreg >= cap) {
                    int ncap = cap ? cap * 2 : 256;
                    char **new_paths = realloc(paths, (size_t)ncap * sizeof(char *));
                    int *new_idxs = realloc(page_idxs, (size_t)ncap * sizeof(int));
                    if (!new_paths || !new_idxs) { free(new_paths); free(new_idxs); free(feat); cli_die("out of memory"); }
                    paths = new_paths;
                    page_idxs = new_idxs;
                    cap = ncap;
                }
                paths[nreg] = strdup(full);
                page_idxs[nreg] = -1;
                nreg++;
                processed++;
            }
            img_free(&im);
        }
    }
    closedir(d);

    if (nreg == 0) {
        cli_die("no sources processed");
    }

    /* Write features.bin */
    FILE *f = fopen(feat_out, "wb");
    if (!f) cli_die("cannot write %s", feat_out);
    uint32_t un = (uint32_t)nreg, uN = (uint32_t)N, uG = (uint32_t)G, uch = (uint32_t)channels;
    fwrite(&un, 4, 1, f); fwrite(&uN, 4, 1, f);
    fwrite(&uG, 4, 1, f); fwrite(&uch, 4, 1, f);
    fwrite(fb.data, 1, (size_t)fb.len, f);
    fclose(f);

    /* Write registry.bin */
    f = fopen(reg_out, "wb");
    if (!f) cli_die("cannot write %s", reg_out);
    fwrite(&un, 4, 1, f);
    for (int i = 0; i < nreg; i++) {
        int32_t pi = page_idxs[i];
        uint32_t pl = (uint32_t)strlen(paths[i]);
        fwrite(&pi, 4, 1, f); fwrite(&pl, 4, 1, f);
        fwrite(paths[i], 1, pl, f);
    }
    fclose(f);

    cli_progress_done("library built");
    cli_info("output: %s (%d entries, %d bytes)", feat_out, nreg, fb.len);
    cli_info("registry: %s (%d entries)", reg_out, nreg);

    /* cleanup */
    for (int i = 0; i < nreg; i++) free(paths[i]);
    free(paths); free(page_idxs); free(fb.data);

    return 0;
}
