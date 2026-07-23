/*
 * cli.c — lightweight CLI framework for videomatch.
 *
 * Provides subcommand dispatch, option parsing, progress reporting,
 * and structured output for the arrange/build/render/inspect tools.
 */
#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */
CLICtx g_cli = {0};

void cli_init(void) {
    memset(&g_cli, 0, sizeof(g_cli));
    g_cli.threads = 0; /* auto */
}

void cli_set_threads(int n) {
    if (n > 0) g_cli.threads = n;
}

/* ------------------------------------------------------------------ */
/*  Simple option store (flat key=value pairs)                         */
/* ------------------------------------------------------------------ */
#define CLI_MAX_OPTS 128
static struct {
    char name[64];
    char value[256];
} g_opts[CLI_MAX_OPTS];
static int g_nopts = 0;

void cli_store(const char *name, const char *value) {
    for (int i = 0; i < g_nopts; i++) {
        if (strcmp(g_opts[i].name, name) == 0) {
            snprintf(g_opts[i].value, sizeof(g_opts[i].value), "%s", value);
            return;
        }
    }
    if (g_nopts < CLI_MAX_OPTS) {
        snprintf(g_opts[g_nopts].name, sizeof(g_opts[g_nopts].name), "%s", name);
        snprintf(g_opts[g_nopts].value, sizeof(g_opts[g_nopts].value), "%s", value);
        g_nopts++;
    }
}

static const char *cli_get(const char *name, const char *def) {
    for (int i = 0; i < g_nopts; i++) {
        if (strcmp(g_opts[i].name, name) == 0) return g_opts[i].value;
    }
    return def;
}

/* ------------------------------------------------------------------ */
/*  Public option accessors                                            */
/* ------------------------------------------------------------------ */
const char *cli_opt_str(const char *name, const char *def) {
    return cli_get(name, def);
}
int cli_opt_int(const char *name, int def) {
    const char *v = cli_get(name, NULL);
    if (v && *v) return atoi(v);
    return def;
}
double cli_opt_dbl(const char *name, double def) {
    const char *v = cli_get(name, NULL);
    if (v && *v) return atof(v);
    return def;
}
bool cli_opt_bool(const char *name, bool def) {
    for (int i = 0; i < g_nopts; i++) {
        if (strcmp(g_opts[i].name, name) == 0) return true;
    }
    return def;
}
bool cli_has(const char *name) {
    for (int i = 0; i < g_nopts; i++) {
        if (strcmp(g_opts[i].name, name) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Argument parser                                                    */
/* ------------------------------------------------------------------ */
void cli_parse(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        /* Flags */
        if (strcmp(arg, "--verbose") == 0 || strcmp(arg, "-v") == 0) {
            g_cli.verbose = true;
        } else if (strcmp(arg, "--quiet") == 0 || strcmp(arg, "-q") == 0) {
            g_cli.quiet = true;
        } else if (strcmp(arg, "--json") == 0) {
            g_cli.json = true;
        } else if (strncmp(arg, "--threads=", 10) == 0) {
            g_cli.threads = atoi(arg + 10);
        } else if (strcmp(arg, "--threads") == 0 && i + 1 < argc) {
            g_cli.threads = atoi(argv[++i]);
        } else if (strcmp(arg, "--") == 0) {
            continue; /* POSIX end-of-flags separator — skip and keep going */
        } else if (strncmp(arg, "--", 2) == 0) {
            /* --key=value */
            char *eq = strchr(arg + 2, '=');
            if (eq) {
                size_t klen = (size_t)(eq - (arg + 2));
                char *key = (char *)malloc(klen + 1);
                if (key) {
                    memcpy(key, arg + 2, klen);
                    key[klen] = '\0';
                    cli_store(key, eq + 1);
                    free(key);
                }
            } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                cli_store(arg + 2, argv[++i]);
            } else {
                cli_store(arg + 2, "1");
            }
        } else if (arg[0] == '-' && arg[1] != '-') {
            /* -k value or -kv */
            for (int j = 1; arg[j]; j++) {
                char name[2] = {arg[j], '\0'};
                if (arg[j + 1] == '=') {
                    cli_store(name, arg + j + 2);
                    break;
                } else if (i + 1 < argc && argv[i + 1][0] != '-') {
                    cli_store(name, argv[++i]);
                    break;
                } else {
                    cli_store(name, "1");
                }
            }
        }
        /* Positional args are ignored here; subcommands handle them */
    }
}

/* ------------------------------------------------------------------ */
/*  Output helpers                                                    */
/* ------------------------------------------------------------------ */
static int is_tty(void) {
    return isatty(fileno(stderr));
}

void cli_info(const char *fmt, ...) {
    if (g_cli.quiet) return;
    va_list ap;
    va_start(ap, fmt);
    if (is_tty() && !g_cli.json) {
        fprintf(stderr, "\033[1;36m[info]\033[0m ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[info] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

void cli_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (is_tty() && !g_cli.json) {
        fprintf(stderr, "\033[1;33m[warn]\033[0m ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[warn] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

void cli_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (is_tty() && !g_cli.json) {
        fprintf(stderr, "\033[1;31m[error]\033[0m ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[error] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

void cli_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (is_tty() && !g_cli.json) {
        fprintf(stderr, "\033[1;31m[fatal]\033[0m ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[fatal] ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
    exit(1);
}

/* ------------------------------------------------------------------ */
/*  Progress reporting                                                 */
/* ------------------------------------------------------------------ */
static double g_last_progress = -1.0;

void cli_progress_frame(const char *label, int frame, int total_frames,
                        double fps, double cache_hit_pct) {
    if (g_cli.quiet || g_cli.json) return;

    double pct = 0.0;
    if (total_frames > 0) pct = (double)frame / (double)total_frames * 100.0;

    /* Throttle: only update if >= 1% change or first frame */
    if (pct - g_last_progress < 1.0 && g_last_progress >= 0.0) return;
    g_last_progress = pct;

    if (is_tty()) {
        fprintf(stderr, "\r\033[K");
        fprintf(stderr, "\033[1;32m[%s]\033[0m frame %d/%d (%.1f%%) | %.1f fps | cache %.1f%%",
                label, frame, total_frames, pct, fps, cache_hit_pct);
        fflush(stderr);
    } else {
        fprintf(stderr, "[%s] frame %d/%d (%.1f%%) | %.1f fps | cache %.1f%%\n",
                label, frame, total_frames, pct, fps, cache_hit_pct);
    }
}

void cli_progress_stage(const char *stage, int percent) {
    if (g_cli.quiet || g_cli.json) return;
    if (is_tty()) {
        fprintf(stderr, "\r\033[K");
        fprintf(stderr, "\033[1;34m[stage]\033[0m %s: %d%%", stage, percent);
        fflush(stderr);
    }
}

void cli_progress_done(const char *summary) {
    if (g_cli.quiet) return;
    if (is_tty()) {
        fprintf(stderr, "\r\033[K");
        fprintf(stderr, "\033[1;32m[done]\033[0m %s\n", summary);
    } else {
        fprintf(stderr, "[done] %s\n", summary);
    }
}

/* ------------------------------------------------------------------ */
/*  JSON output helpers (minimal)                                      */
/* ------------------------------------------------------------------ */
void cli_json_start(void) {
    if (g_cli.json) fprintf(stderr, "{\n");
}

void cli_json_field(const char *name, const char *value) {
    if (g_cli.json) {
        /* NOTE: neither name nor value are JSON-escaped here; embedded "
         * characters will produce invalid JSON. A future fix should escape
         * backslash, double-quote, and control characters. */
        fprintf(stderr, "  \"%s\": \"%s\",\n", name, value);
    }
}

void cli_json_end(void) {
    if (g_cli.json) fprintf(stderr, "}\n");
}
