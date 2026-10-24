#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── String view ─────────────────────────────────────────────────────────
 * sv_t is a non-owning slice into an existing buffer.  It is used
 * throughout the assembler core to avoid repeated malloc/free for
 * sub-string tokens.  The pointed-to data is never NUL-terminated and
 * must outlive the sv_t.
 */
typedef struct {
    const char* p;   /* pointer into source text — not NUL-terminated */
    size_t len;      /* byte count (not including any terminator)       */
} sv_t;

/* Construct an sv_t from a NUL-terminated C string (O(n) via strlen). */
static inline sv_t sv_from_cstr(const char* s) {
    sv_t v; v.p = s; v.len = s ? strlen(s) : 0; return v;
}

/* Case-insensitive equality: sv_t vs NUL-terminated string.
 * Returns true if both represent the same text, ignoring ASCII case.
 * in:  a       — string view
 *      b_cstr  — NUL-terminated comparison string
 * out: true if equal ignoring case */
bool sv_ieq(sv_t a, const char* b_cstr);

/* Parse an integer literal from a string view.
 * Accepted prefixes: '$' or '&' for hex, '%' for binary, none for decimal.
 * Underscore separators are ignored (e.g. %1000_0000).
 * Also recognises CPU-model names used in conditional assembly:
 *   65c02 -> 6503, 6502u -> 6501.
 * in:  tok — token to parse
 * out: *out — parsed value (only written on success)
 * ret: true on success, false if tok is empty or contains invalid digits */
bool parse_int_literal(sv_t tok, int64_t* out);

/* Return true if v fits in an unsigned 16-bit value (0..65535). */
static inline bool in_u16(int64_t v) { return v >= 0 && v <= 65535; }

/* Return true if v fits in an unsigned 32-bit value (0..4294967295). */
static inline bool in_u32(int64_t v) { return v >= 0 && v <= 0xFFFFFFFFLL; }

/* Return the smallest byte width (1, 2, or 4) needed to store v as an
 * unsigned integer.  Returns 0 if v is negative or exceeds 32 bits. */
unsigned smallest_width_u32(int64_t v);

/* Return true if c may begin an identifier (letter, underscore, or dot).
 * The dot is included because directives such as .org start with it. */
bool is_ident_start(char c);

/* Return true if c may continue an identifier (letter, digit, underscore,
 * or dot). */
bool is_ident_char(char c);

/* Return true if c is an ASCII horizontal whitespace or CR character
 * (space, tab, or carriage return).  Newline is intentionally excluded
 * so line-boundary logic can use this predicate safely. */
static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Trim leading and trailing whitespace (space, tab, CR) from a string view.
 * The underlying data is not modified; the returned sv_t points into the
 * same buffer with adjusted p and len.
 * in:  v — string view to trim
 * out: trimmed string view */
static inline sv_t sv_trim(sv_t v) {
    while (v.len && (v.p[0] == ' ' || v.p[0] == '\t' || v.p[0] == '\r')) {
        v.p++; v.len--;
    }
    while (v.len && (v.p[v.len - 1] == ' ' || v.p[v.len - 1] == '\t' || v.p[v.len - 1] == '\r')) {
        v.len--;
    }
    return v;
}

/* Allocate a heap copy of n bytes from p, appending a NUL terminator.
 * in:  p — source bytes
 *      n — byte count (NUL is written at p[n])
 * ret: heap-allocated NUL-terminated string, or NULL on OOM */
char* xstrdup_n(const char* p, size_t n);

/* Allocate a heap copy of a NUL-terminated string.
 * in:  s — source string (may be NULL, returns NULL)
 * ret: heap-allocated duplicate, or NULL on NULL input or OOM */
char* xstrdup(const char* s);
