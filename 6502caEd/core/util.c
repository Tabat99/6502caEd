#include "util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Return true if c may begin an identifier token (letter, '_', or '.'). */
bool is_ident_start(char c) {
    return (c == '.' || c == '_' || isalpha((unsigned char)c));
}

/* Return true if c may continue an identifier token (letter, digit, '_', or '.'). */
bool is_ident_char(char c) {
    return (c == '.' || c == '_' || isalnum((unsigned char)c));
}

/* Case-insensitive equality between an sv_t and a NUL-terminated string. */
bool sv_ieq(sv_t a, const char* b_cstr) {
    size_t bl = strlen(b_cstr);
    if (a.len != bl) return false;
    for (size_t i = 0; i < a.len; ++i) {
        unsigned char ca = (unsigned char)a.p[i];
        unsigned char cb = (unsigned char)b_cstr[i];
        if (toupper(ca) != toupper(cb)) return false;
    }
    return true;
}

/* Return the numeric value (0..15) of a single hex digit, or -1 if invalid. */
static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse an integer literal.  Supports $hex, &hex, %binary, and decimal.
 * Underscore separators are silently skipped.
 * Special CPU-model tokens are mapped to their internal numeric codes:
 *   65c02 -> 6503  (so .if cpu == 65c02 works in conditional assembly)
 *   6502u -> 6501  (undocumented-opcodes variant) */
bool parse_int_literal(sv_t tok, int64_t* out) {
    if (!tok.p || tok.len == 0) return false;

    // Convenience CPU-model literals used in conditional assembly.
    // Expressions are numeric, but many sources write:
    //   .if cpu == 65c02
    //   .if cpu == 6502u
    // Map each to a distinct internal value:
    //   6502  -> 6502  (plain numeric, handled below)
    //   6502u -> 6501  (undocumented opcodes variant)
    //   65c02 -> 6503
    //   65816 -> 65816 (plain numeric, handled below)
    if (sv_ieq(tok, "65c02") || sv_ieq(tok, "65C02")) {
        if (out) *out = 6503;
        return true;
    }
    if (sv_ieq(tok, "6502u") || sv_ieq(tok, "6502U")) {
        if (out) *out = 6501;
        return true;
    }

    int base = 10;
    size_t i = 0;
    if (tok.p[0] == '$' || tok.p[0] == '&') { base = 16; i = 1; }
    else if (tok.p[0] == '%') { base = 2; i = 1; }

    if (i >= tok.len) return false;

    int64_t v = 0;
    for (; i < tok.len; ++i) {
        char c = tok.p[i];
        if (c == '_') continue;  /* allow underscore separators */
        int d = digit_val(c);
        if (d < 0 || d >= base) return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}

/* Return the smallest byte width (1, 2, or 4) that can hold v as an
 * unsigned integer.  Returns 0 if v is negative or exceeds 32 bits. */
unsigned smallest_width_u32(int64_t v) {
    if (v < 0) return 0;
    if (v <= 0xFF) return 1;
    if (v <= 0xFFFF) return 2;
    if (v <= 0xFFFFFFFFLL) return 4;
    return 0;
}

/* Allocate a heap copy of n bytes from p, NUL-terminated. */
char* xstrdup_n(const char* p, size_t n) {
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

/* Allocate a heap copy of a NUL-terminated string. */
char* xstrdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    return xstrdup_n(s, n);
}
