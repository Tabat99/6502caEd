#pragma once
#include <stddef.h>
#include <stdbool.h>

/* ── Source file loading ──────────────────────────────────────────────────
 * source_t owns a heap copy of a source file's text.  The text is
 * NUL-terminated and unchanged from the file on disk until
 * source_strip_comments_preserve_layout is called.
 */
typedef struct {
    char* text;  /* heap-allocated, NUL-terminated source text */
    size_t len;  /* byte count, not including the NUL          */
} source_t;

/* Load a source file into a heap buffer.
 * in:  path — file path to open
 * out: *out — populated source_t (must be freed with source_free)
 * ret: false if the file cannot be opened or read */
bool source_load(const char* path, source_t* out);

/* Free the heap buffer and zero the struct. */
void source_free(source_t* s);

/* Remove all comments from the source text in-place, replacing each
 * comment character with a space.  Newlines are preserved so that all
 * line and column numbers remain correct.  Handles:
 *   ;          — line comment (to end of line)
 *   // ...     — C++-style line comment
 *   /* ... */  /* — block comment (may span lines) */
void source_strip_comments_preserve_layout(source_t* s);
