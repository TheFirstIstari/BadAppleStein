/*
 * cli.h — lightweight CLI framework for videomatch.
 *
 * Provides subcommand dispatch, option parsing, progress reporting,
 * and structured output for the arrange/build/render/inspect tools.
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Context (global, set once at startup)                             */
/* ------------------------------------------------------------------ */
typedef struct {
    bool verbose;
    bool quiet;
    bool json;
    int  threads;       /* 0 = auto-detect */
} CLICtx;

extern CLICtx g_cli;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */
void cli_init(void);
void cli_set_threads(int n);
void cli_parse(int argc, char **argv);

/* ------------------------------------------------------------------ */
/*  Output                                                             */
/* ------------------------------------------------------------------ */
void cli_info(const char *fmt, ...);
void cli_warn(const char *fmt, ...);
void cli_error(const char *fmt, ...);
void cli_die(const char *fmt, ...);   /* prints + exits */

/* Progress: percent is 0-100. total_frames may be 0 if unknown.
 * label is the stage tag shown in brackets, e.g. "arrange", "render". */
void cli_progress_frame(const char *label, int frame, int total_frames,
                        double fps, double cache_hit_pct);
void cli_progress_stage(const char *stage, int percent);
void cli_progress_done(const char *summary);

/* ------------------------------------------------------------------ */
/*  JSON output helpers                                                */
/* ------------------------------------------------------------------ */
void cli_json_start(void);
void cli_json_field(const char *name, const char *value);
void cli_json_end(void);

/* ------------------------------------------------------------------ */
/*  Option helpers                                                     */
/* ------------------------------------------------------------------ */
const char *cli_opt_str(const char *name, const char *def);
int         cli_opt_int(const char *name, int def);
double      cli_opt_dbl(const char *name, double def);
bool        cli_opt_bool(const char *name, bool def);
bool        cli_has(const char *name);

void        cli_store(const char *name, const char *value);

#endif /* CLI_H */
