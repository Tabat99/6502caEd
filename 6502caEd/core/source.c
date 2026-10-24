#include "source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load the entire contents of path into a heap buffer.
 * Uses fseek/ftell to determine size, then reads in one call. */
bool source_load(const char* path, source_t* out) {
    memset(out, 0, sizeof(*out));
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out->text = (char*)malloc((size_t)n + 1);
    if (!out->text) { fclose(f); return false; }
    size_t got = fread(out->text, 1, (size_t)n, f);
    fclose(f);
    out->len = got;
    out->text[got] = '\0';
    return true;
}

/* Free the heap buffer. */
void source_free(source_t* s) {
    free(s->text);
    s->text = NULL;
    s->len = 0;
}

/* Erase comments in-place by overwriting their characters with spaces.
 * All newlines are left intact so that line/column tracking in the
 * assembler and diagnostics remains correct after this call.
 *
 * Comment styles handled:
 *   ; comment       — semicolon to end of line (standard 6502 assembler style)
 *   // comment      — C++ style
 *   /* comment * /  — C block comment (may span multiple lines)
 *
 * String literals are tracked so that a ; or // inside a string is not
 * treated as a comment start. */
void source_strip_comments_preserve_layout(source_t* s) {
    bool in_str = false;
    bool in_block = false;

    for (size_t i = 0; i < s->len; ++i) {
        char c = s->text[i];
        char n1 = (i + 1 < s->len) ? s->text[i + 1] : '\0';

        if (in_block) {
            /* Inside a block comment: erase until the closing delimiter. */
            if (c == '*' && n1 == '/') {
                s->text[i] = ' ';
                s->text[i + 1] = ' ';
                i++;
                in_block = false;
            } else {
                if (c != '\n') s->text[i] = ' ';
            }
            continue;
        }

        if (!in_str) {
            if (c == '/' && n1 == '*') {
                s->text[i] = ' ';
                s->text[i + 1] = ' ';
                i++;
                in_block = true;
                continue;
            }
            if (c == '/' && n1 == '/') {
                /* erase until newline */
                s->text[i] = ' ';
                s->text[i + 1] = ' ';
                i += 2;
                for (; i < s->len && s->text[i] != '\n'; ++i) s->text[i] = ' ';
                i--; /* for-loop will ++ */
                continue;
            }
            if (c == ';') {
                /* erase until newline */
                for (; i < s->len && s->text[i] != '\n'; ++i) s->text[i] = ' ';
                i--;
                continue;
            }
        }

        /* String tracking: toggle on unescaped double-quote.
         * This is intentionally simple; escape sequences inside strings
         * are not handled here (they're irrelevant for comment stripping). */
        if (c == '"') {
            in_str = !in_str;
        }
    }
}
