/*
 * main.c — unified entry point for the badapplestein binary.
 *
 * Dispatches to arrange_main(), render_main(), or build_main() based on
 * subcommand detection. Handles library path resolution, preset expansion,
 * temp-manifest lifecycle, and injects resolved options into the global
 * CLI option store before calling sub-functions.
 *
 * The arrange/render/build functions read all their options from the
 * global option store (populated by cli_parse + cli_store injections).
 */
#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* Sub-function entry points (implemented in arrange.c, render.c,
 * build_library.c respectively). */
extern int arrange_main(int argc, char **argv);
extern int render_main(int argc, char **argv);
extern int build_main(int argc, char **argv);

#ifndef VERSION
#define VERSION "dev"
#endif

/* ------------------------------------------------------------------ */
/*  Usage                                                              */
/* ------------------------------------------------------------------ */
static void print_usage(void) {
    fprintf(stderr,
        "badapplestein \u2014 reconstruct videos using source library pages\n"
        "\n"
        "Usage:\n"
        "  badapplestein build <sources_dir> [options]    Build a source library\n"
        "  badapplestein encode <input> <output> [options]  Encode a video\n"
        "  badapplestein <input> <output> [options]         (shorthand for encode)\n"
        "\n"
        "Options:\n"
        "  --library <dir>       Library directory (default: ./ or ~/.badapplestein/library/)\n"
        "  --preset <name>       Output preset: 8k, 4k, 1080p, 720p\n"
        "  --width <n>           Output width (overrides preset, auto from source if omitted)\n"
        "  --height <n>          Output height (auto from source aspect if omitted)\n"
        "  --fps <n>             Output fps (auto from source if omitted)\n"
        "  --codec <name>        Output codec (default: auto-detect best available)\n"
        "  --no-hw               Disable hardware encoding\n"
        "  --max-frames <n>      Process only N frames (0 = all)\n"
        "  --threads <n>         Thread count (0 = auto)\n"
        "  --keep-manifests      Keep temp manifests after encoding\n"
        "  --verbose, -v         Verbose output\n"
        "  --quiet, -q           Suppress non-error output\n"
        "  --json                JSON output mode\n"
        "  --help, -h            Show this help\n"
        "  --version, -V        Show version\n"
        "\n"
        "Examples:\n"
        "  badapplestein build ~/Documents/pdfs/\n"
        "  badapplestein encode input.mp4 output.mov\n"
        "  badapplestein input.mp4 output.mov --preset 4k\n"
        "  badapplestein input.mp4 output.mov --library ./mylib --width 1920\n"
    );
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void path_join(const char *dir, const char *file, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%s/%s", dir, file);
}

/* Copy parent directory of path into buf. Falls back to "." on bare names. */
static void parent_dir(const char *path, char *buf, size_t bufsz) {
    strncpy(buf, path, bufsz - 1);
    buf[bufsz - 1] = '\0';
    char *last_slash = strrchr(buf, '/');
    if (!last_slash) {
        strncpy(buf, ".", bufsz);
    } else if (last_slash == buf) {
        buf[1] = '\0';
    } else {
        *last_slash = '\0';
    }
}

/* Recursively remove a directory (safe for temp dirs we created). */
static void rmdir_recursive(const char *dir) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "warning: rmdir_recursive failed for %s (ignored)\n", dir);
    }
}

/* ------------------------------------------------------------------ */
/*  Library path resolution                                            */
/* ------------------------------------------------------------------ */
static const char *resolve_library(const char *explicit) {
    static char buf[1024];

    /* 1. Explicit --library flag */
    if (explicit) return explicit;

    /* 2. Current directory */
    char path[1024];
    path_join(".", "features.bin", path, sizeof(path));
    if (file_exists(path)) {
        path_join(".", "registry.bin", path, sizeof(path));
        if (file_exists(path)) return ".";
    }

    /* 3. ~/.badapplestein/library/ */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.badapplestein/library", home);
        path_join(buf, "features.bin", path, sizeof(path));
        if (file_exists(path)) {
            path_join(buf, "registry.bin", path, sizeof(path));
            if (file_exists(path)) return buf;
        }
    }

    /* 4. Fallback: current directory */
    return ".";
}

/* ------------------------------------------------------------------ */
/*  Preset system                                                      */
/* ------------------------------------------------------------------ */
static int preset_width(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "8k") == 0)   return 7680;
    if (strcmp(name, "4k") == 0)   return 3840;
    if (strcmp(name, "1080p") == 0) return 1920;
    if (strcmp(name, "720p") == 0)  return 1280;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Subcommand detection                                               */
/* ------------------------------------------------------------------ */
enum SubCmd { SUB_NONE, SUB_BUILD, SUB_ENCODE };

/* Long options that consume a following value argument.
 * Used to skip option values when scanning for positionals. */
static int opt_needs_value(const char *key) {
    return (strcmp(key, "library") == 0 ||
            strcmp(key, "preset") == 0 ||
            strcmp(key, "width") == 0 ||
            strcmp(key, "height") == 0 ||
            strcmp(key, "fps") == 0 ||
            strcmp(key, "codec") == 0 ||
            strcmp(key, "max-frames") == 0 ||
            strcmp(key, "threads") == 0);
}

/* Scan argv to identify subcommand and collect positional arguments.
 * Skips all --key [value] and -flag pairs. */
static void scan_args(int argc, char **argv,
                      enum SubCmd *sub_out,
                      const char **pos, int *npos_out) {
    enum SubCmd sub = SUB_NONE;
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* POSIX end-of-flags: everything after -- is positional */
        if (strcmp(arg, "--") == 0) {
            i++;
            while (i < argc && npos < 4) pos[npos++] = argv[i++];
            break;
        }

        /* Long option */
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            const char *key = arg + 2;
            const char *eq = strchr(key, '=');
            if (!eq && opt_needs_value(key) && i + 1 < argc) {
                i++; /* skip value */
            }
            continue;
        }

        /* Short option (-v, -q, -h, or -k value) */
        if (arg[0] == '-' && arg[1] != '\0') {
            /* For short flags, skip the next arg only if it looks like
             * a value (doesn't start with '-') — mirrors cli_parse
             * behavior. All current short opts are boolean flags. */
            continue;
        }

        /* Positional argument */
        if (sub == SUB_NONE) {
            if (strcmp(arg, "build") == 0) {
                sub = SUB_BUILD;
                continue;
            }
            if (strcmp(arg, "encode") == 0) {
                sub = SUB_ENCODE;
                continue;
            }
            /* Not a known subcommand — auto-detect as encode */
            sub = SUB_ENCODE;
        }

        if (npos < 4) pos[npos++] = arg;
    }

    *sub_out = sub;
    *npos_out = npos;
}

/* ------------------------------------------------------------------ */
/*  Encode subcommand                                                  */
/* ------------------------------------------------------------------ */
static int run_encode(const char *input, const char *output,
                      const char *library_arg) {
    /* Resolve library path */
    const char *lib_dir = resolve_library(library_arg);
    char features_path[1024];
    char registry_path[1024];
    path_join(lib_dir, "features.bin", features_path, sizeof(features_path));
    path_join(lib_dir, "registry.bin", registry_path, sizeof(registry_path));

    if (!file_exists(features_path))
        cli_die("features.bin not found in library: %s", lib_dir);
    if (!file_exists(registry_path))
        cli_die("registry.bin not found in library: %s", lib_dir);

    /* Create temp manifests directory in output's parent */
    char output_dir[1024];
    parent_dir(output, output_dir, sizeof(output_dir));

    char manifests_tpl[2048];
    snprintf(manifests_tpl, sizeof(manifests_tpl),
             "%s/.badapplestein-manifests-XXXXXX", output_dir);
    char *manifests_dir = mkdtemp(manifests_tpl);
    if (!manifests_dir)
        cli_die("cannot create temp manifests directory in %s", output_dir);

    cli_info("library: %s", lib_dir);
    cli_info("input: %s", input);
    cli_info("output: %s", output);
    cli_info("manifests: %s", manifests_dir);

    /* Inject resolved paths into the option store */
    cli_store("features", features_path);
    cli_store("registry", registry_path);
    cli_store("manifests", manifests_dir);
    cli_store("video", input);

    /* Resolve width: --width overrides --preset; 0 means auto from source */
    int width = cli_opt_int("width", 0);
    if (width == 0) {
        const char *preset_name = cli_opt_str("preset", NULL);
        int pw = preset_width(preset_name);
        if (pw > 0) {
            char wbuf[32];
            snprintf(wbuf, sizeof(wbuf), "%d", pw);
            cli_store("width", wbuf);
        }
    }

    /* Inject output path for render stage */
    cli_store("output", output);

    /* ── Stage 1: Arrange ──────────────────────────────────────── */
    cli_info("arranging...");
    int rc = arrange_main(0, NULL);
    if (rc != 0) {
        cli_error("arrange stage failed");
        goto cleanup;
    }

    /* ── Stage 2: Render ───────────────────────────────────────── */
    cli_info("rendering...");
    rc = render_main(0, NULL);
    if (rc != 0) {
        cli_error("render stage failed");
    }

cleanup:
    /* Clean up temp manifests unless --keep-manifests */
    if (!cli_has("keep-manifests")) {
        rmdir_recursive(manifests_dir);
    } else {
        cli_info("keeping manifests: %s", manifests_dir);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/*  Build subcommand                                                   */
/* ------------------------------------------------------------------ */
static int run_build(const char *sources_dir, const char *library_arg) {
    /* Resolve library output directory */
    const char *lib_dir = resolve_library(library_arg);

    /* Create library directory if it doesn't exist */
    if (!dir_exists(lib_dir)) {
        if (mkdir(lib_dir, 0755) != 0)
            cli_die("cannot create library directory: %s", lib_dir);
    }

    /* Set --out to features.bin in the library directory */
    char features_path[1024];
    path_join(lib_dir, "features.bin", features_path, sizeof(features_path));
    cli_store("out", features_path);

    cli_info("sources: %s", sources_dir);
    cli_info("library: %s", lib_dir);

    /* build_main reads argv[1] for sources_dir directly */
    const char *argv_build[] = {"build", sources_dir, NULL};
    return build_main(2, (char **)argv_build);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    cli_init();
    cli_parse(argc, argv);

    /* --help / -h shows usage regardless of subcommand */
    if (cli_has("help") || cli_has("h")) {
        print_usage();
        return 0;
    }

    if (cli_has("version") || cli_has("V")) {
        printf("badapplestein %s\n", VERSION);
        return 0;
    }

    /* Detect subcommand and collect positional args */
    enum SubCmd sub = SUB_NONE;
    const char *pos[4];
    int npos = 0;
    scan_args(argc, argv, &sub, pos, &npos);

    /* Grab library option (shared by both subcommands) */
    const char *library_arg = cli_opt_str("library", NULL);

    switch (sub) {
    case SUB_BUILD: {
        if (npos < 1) {
            cli_error("build requires a <sources_dir> argument");
            print_usage();
            return 1;
        }
        /* --preset is only valid with encode */
        if (cli_has("preset"))
            cli_warn("--preset is ignored for build subcommand");
        /* --keep-manifests is only valid with encode */
        if (cli_has("keep-manifests"))
            cli_warn("--keep-manifests is ignored for build subcommand");
        return run_build(pos[0], library_arg);
    }

    case SUB_ENCODE: {
        if (npos < 2) {
            cli_error("encode requires <input> and <output> arguments");
            print_usage();
            return 1;
        }
        return run_encode(pos[0], pos[1], library_arg);
    }

    default:
        /* No subcommand and no positionals — show usage */
        if (npos == 0) {
            print_usage();
            return 1;
        }
        /* Should not happen: scan_args auto-detects encode */
        cli_error("no subcommand detected");
        print_usage();
        return 1;
    }
}
