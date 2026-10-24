#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "source.h"

/* Initialise an empty diagnostic list. */
void diags_init(diags_t* d) {
    memset(d, 0, sizeof(*d));
}

/* Free all diagnostics and zero the struct. */
void diags_free(diags_t* d) {
    if (d && d->items) {
        for (size_t i = 0; i < d->count; ++i) {
            free(d->items[i].file);
            d->items[i].file = NULL;
        }
    }
    free(d->items);
    memset(d, 0, sizeof(*d));
}

/* Grow the items array if needed (doubles on each reallocation). */
static void ensure_cap(diags_t* d) {
    if (d->count < d->cap) return;
    size_t ncap = d->cap ? d->cap * 2 : 32;
    diag_t* np = (diag_t*)realloc(d->items, ncap * sizeof(diag_t));
    if (!np) return; /* best-effort; the caller checks d->count < d->cap */
    d->items = np;
    d->cap = ncap;
}

/* Append a formatted diagnostic.  The file name is always duplicated so
 * callers using temporary path buffers (e.g. include expansion) are safe. */
void diag_addf(diags_t* d, diag_severity_t sev, const char* file,
               unsigned line, unsigned col, unsigned len,
               const char* fmt, ...) {
    if (!d) return;
    ensure_cap(d);
    if (d->count >= d->cap) return;
    diag_t* x = &d->items[d->count++];
    memset(x, 0, sizeof(*x));
    x->sev = sev;
    // Duplicate file name so diagnostics remain valid even if the caller's
    // path buffer is transient (e.g. include-expansion uses temporary strings).
    const char* f = file ? file : "<input>";
    x->file = strdup(f);
    if (!x->file) x->file = strdup("<input>");
    x->line = line;
    x->col = col;
    x->len = (len == 0 ? 1 : len);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->msg, sizeof(x->msg), fmt, ap);
    va_end(ap);
}

/* Locate the start of a 1-based line in text and set *out_end to one
 * past the last character (before the newline).
 * Used by diags_print to extract the source line for display. */
static const char* line_ptr(const char* text, unsigned target_line, const char** out_end) {
    unsigned line = 1;
    const char* p = text;
    const char* start = p;
    while (*p && line < target_line) {
        if (*p == '\n') { line++; start = p + 1; }
        p++;
    }
    const char* e = start;
    while (*e && *e != '\n') e++;
    if (out_end) *out_end = e;
    return start;
}

/* Print all diagnostics to stderr.
 * For each diagnostic, prints:
 *   file:line:col: severity: message
 *   <source line>
 *   ^~~~~ (caret underline)
 * The source line is loaded from the diagnostic's file when possible;
 * otherwise the caller's source_text is used as a fallback. */
/* Print all diagnostics to stderr, each followed by the relevant source line
 * with a caret-underline showing the highlighted column span.
 * Attempts to load the diagnostic's file from disk; falls back to source_text.
 * in:  d           — diagnostic list to print
 *      source_text — preprocessed source used as fallback for line display */
void diags_print(const diags_t* d, const char* source_text) {
    for (size_t i = 0; i < d->count; ++i) {
        const diag_t* x = &d->items[i];
        const char* sev = (x->sev == DIAG_ERROR) ? "error" : "warning";
        fprintf(stderr, "%s:%u:%u: %s: %s\n", x->file, x->line, x->col, sev, x->msg);
        // Try to show the source line from the diagnostic's file.
        // Falls back to the provided source_text (typically the preprocessed main input).
        const char* text = source_text;
        source_t fs = {0};
        if (x->file && x->file[0] && strcmp(x->file, "<input>") != 0) {
            if (source_load(x->file, &fs)) {
                source_strip_comments_preserve_layout(&fs);
                text = fs.text;
            }
        }
        if (!text) { source_free(&fs); continue; }
        const char* le = NULL;
        const char* ls = line_ptr(text, x->line, &le);
        fwrite(ls, 1, (size_t)(le - ls), stderr);
        fputc('\n', stderr);
        /* Print caret underline: spaces up to col, then ^ followed by ~ repeats. */
        unsigned col = x->col ? x->col : 1;
        for (unsigned sp = 1; sp < col; ++sp) fputc(' ', stderr);
        fputc('^', stderr);
        for (unsigned k = 1; k < x->len; ++k) fputc('~', stderr);
        fputc('\n', stderr);

        source_free(&fs);
    }
}
