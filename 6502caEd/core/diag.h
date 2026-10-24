#pragma once
#include <stdbool.h>
#include <stddef.h>

/* ── Diagnostics ─────────────────────────────────────────────────────────
 * A diags_t collects assembler errors and warnings during a single
 * assembly run.  Each diagnostic records the source location (file, line,
 * column, highlight length) and a formatted message.
 *
 * Diagnostics are printed to stderr by diags_print, which also attempts
 * to load and display the relevant source line with a caret underline.
 */

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING
} diag_severity_t;

/* A single error or warning. */
typedef struct {
    diag_severity_t sev;
    char* file;        /* owned string (duplicated by diag_addf)   */
    unsigned line;     /* 1-based source line number               */
    unsigned col;      /* 1-based column of the token start        */
    unsigned len;      /* highlight length in characters (>= 1)    */
    char msg[256];     /* formatted message (truncated at 255 chars)*/
} diag_t;

/* Dynamic array of diagnostics. */
typedef struct {
    diag_t* items;
    size_t count;
    size_t cap;
} diags_t;

/* Initialise an empty diagnostic list. */
void diags_init(diags_t* d);

/* Free all memory owned by the list and zero the struct. */
void diags_free(diags_t* d);

/* Append a formatted diagnostic at the given source location.
 * The file string is duplicated so the caller's buffer need not persist.
 * in:  d          — diagnostic list
 *      sev        — DIAG_ERROR or DIAG_WARNING
 *      file       — source file name (may be NULL -> "<input>")
 *      line       — 1-based line number
 *      col        — 1-based column number
 *      len        — number of characters to underline (0 treated as 1)
 *      fmt, ...   — printf-style message format */
void diag_addf(diags_t* d, diag_severity_t sev, const char* file,
               unsigned line, unsigned col, unsigned len,
               const char* fmt, ...);

/* Print all diagnostics to stderr with a caret underline.
 * source_text is the preprocessed source used as a fallback when a
 * diagnostic's file cannot be loaded from disk. */
void diags_print(const diags_t* d, const char* source_text);

/* Return true if the list contains at least one DIAG_ERROR. */
static inline bool diags_has_errors(const diags_t* d) {
    for (size_t i = 0; i < d->count; ++i) if (d->items[i].sev == DIAG_ERROR) return true;
    return false;
}

/* Return true if the list contains at least one DIAG_WARNING. */
static inline bool diags_has_warnings(const diags_t* d) {
    for (size_t i = 0; i < d->count; ++i) if (d->items[i].sev == DIAG_WARNING) return true;
    return false;
}
