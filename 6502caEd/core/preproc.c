#include "preproc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "source.h"
#include "expr.h"
#include "sym.h"

#define PP_FILE_MARK 0x1d
#define PP_LINE_MARK 0x1f  /* line-number reset: \x1f<decimal line>\n */

/* ---------- pragma-once registry ---------------------------------------- */
/* Files that have been seen with .pragma once are recorded here.  The set is
 * reset at the start of each preproc_run_ex call so it is scoped to one
 * top-level assembly run. */
#define PP_ONCE_MAX 128
static char  *g_once_paths[PP_ONCE_MAX];
static size_t g_once_count = 0;

static void once_reset(void) {
    for (size_t i = 0; i < g_once_count; i++) { free(g_once_paths[i]); g_once_paths[i] = NULL; }
    g_once_count = 0;
}
/* Returns true if `path` is already in the once set. */
static bool once_has(const char *path) {
    if (!path) return false;
    for (size_t i = 0; i < g_once_count; i++)
        if (g_once_paths[i] && strcmp(g_once_paths[i], path) == 0) return true;
    return false;
}
/* Adds `path` to the once set (duplicates the string). */
static void once_add(const char *path) {
    if (!path || once_has(path)) return;
    if (g_once_count >= PP_ONCE_MAX) return;
    g_once_paths[g_once_count++] = strdup(path);
}
/* ---------- end pragma-once registry ------------------------------------ */

// Forward decls (sbuf is defined later in this file)
typedef struct sbuf_t sbuf_t;
/* Append a single character to the string buffer. */
static bool sbuf_putc(sbuf_t* b, char c);
/* Append n bytes from s to the string buffer. */
static bool sbuf_puts_n(sbuf_t* b, const char* s, size_t n);
/* Append a NUL-terminated string to the string buffer. */
static bool sbuf_puts(sbuf_t* b, const char* s);

// Full whitespace predicate including newlines (used for token scanning in preproc).
// The shared is_ws() in util.h matches only space/tab/CR.
static bool pp_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/* FNV-1a hash of s, folded to lowercase for case-insensitive lookup.
 * Used by the defs hash table. */
static uint32_t hash_sv(sv_t s) {
    // FNV-1a
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.len; ++i) {
        // Case-insensitive hashing (preprocessor symbols/directives are case-insensitive).
        unsigned char c = (unsigned char)s.p[i];
        c = (unsigned char)tolower(c);
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

/* Initialise an empty preprocessor-define hash table (128 slots). */
void defs_init(defs_t* d) {
    memset(d, 0, sizeof(*d));
    d->cap = 128;
    d->tab = (def_entry_t*)calloc(d->cap, sizeof(def_entry_t));
}

/* Free all keys, values, and the slot array. */
void defs_free(defs_t* d) {
    if (!d) return;
    for (size_t i = 0; i < d->cap; ++i) {
        if (d->tab[i].used) {
            free(d->tab[i].key);
            free(d->tab[i].val);
        }
    }
    free(d->tab);
    memset(d, 0, sizeof(*d));
}

/* Double the hash table capacity and rehash all existing entries.
 * Uses stored key_len to avoid strlen during rehash. */
static bool defs_grow(defs_t* d) {
    size_t oldcap = d->cap;
    def_entry_t* old = d->tab;

    size_t newcap = oldcap * 2;
    def_entry_t* nt = (def_entry_t*)calloc(newcap, sizeof(def_entry_t));
    if (!nt) return false;

    d->tab = nt;
    d->cap = newcap;
    d->count = 0;

    for (size_t i = 0; i < oldcap; ++i) {
        if (!old[i].used) continue;
        sv_t k = { old[i].key, old[i].key_len };
        // reinsert
        uint32_t h = hash_sv(k);
        size_t mask = d->cap - 1;
        size_t idx = h & mask;
        while (d->tab[idx].used) idx = (idx + 1) & mask;
        d->tab[idx].used = true;
        d->tab[idx].key = old[i].key;
        d->tab[idx].key_len = old[i].key_len;
        d->tab[idx].val = old[i].val;
        d->count++;
        old[i].key = NULL;
        old[i].key_len = 0;
        old[i].val = NULL;
        old[i].used = false;
    }
    free(old);
    return true;
}

/* Insert or update a preprocessor define.
 * If the key already exists its value is replaced.
 * in:  key — define name (NUL-terminated)
 *      val — replacement text (NUL-terminated)
 * ret: false on OOM */
bool defs_set(defs_t* d, const char* key, const char* val) {
    if (!d || !key || !*key) return false;
    if (d->count * 10 >= d->cap * 7) {
        if (!defs_grow(d)) return false;
    }

    sv_t k = sv_from_cstr(key);
    uint32_t h = hash_sv(k);
    size_t mask = d->cap - 1;
    size_t idx = h & mask;

    while (d->tab[idx].used) {
        if (d->tab[idx].key_len == k.len &&
            sv_ieq(k, d->tab[idx].key)) {
            // overwrite
            free(d->tab[idx].val);
            d->tab[idx].val = xstrdup_n(val, strlen(val));
            return d->tab[idx].val != NULL;
        }
        idx = (idx + 1) & mask;
    }

    d->tab[idx].used = true;
    d->tab[idx].key = xstrdup_n(key, k.len);
    d->tab[idx].key_len = k.len;
    d->tab[idx].val = xstrdup_n(val, strlen(val));
    if (!d->tab[idx].key || !d->tab[idx].val) return false;
    d->count++;
    return true;
}

// Remove a definition (used by #undef/.undef). Returns true if it existed.
// Uses open addressing, so we must "close" the probe chain by re-inserting
// subsequent clustered entries.
/* Remove a define (used by #undef / .undef).
 * Handles open-addressing by rehashing the subsequent cluster.
 * ret: true if the key was present and removed */
bool defs_del(defs_t* d, sv_t key) {
    if (!d || !key.p || key.len == 0) return false;
    size_t mask = d->cap - 1;
    size_t idx = hash_sv(key) & mask;

    while (d->tab[idx].used) {
        if (d->tab[idx].key_len == key.len &&
            sv_ieq(key, d->tab[idx].key)) {
            // delete this slot
            free(d->tab[idx].key);
            free(d->tab[idx].val);
            d->tab[idx].key = NULL;
            d->tab[idx].val = NULL;
            d->tab[idx].key_len = 0;
            d->tab[idx].used = false;
            if (d->count) d->count--;

            // rehash the following cluster
            size_t next = (idx + 1) & mask;
            while (d->tab[next].used) {
                char* k = d->tab[next].key;
                size_t kl = d->tab[next].key_len;
                char* v = d->tab[next].val;
                d->tab[next].key = NULL;
                d->tab[next].key_len = 0;
                d->tab[next].val = NULL;
                d->tab[next].used = false;
                if (d->count) d->count--;

                sv_t ksv = { k, kl };
                size_t j = hash_sv(ksv) & mask;
                while (d->tab[j].used) j = (j + 1) & mask;
                d->tab[j].used = true;
                d->tab[j].key = k;
                d->tab[j].key_len = kl;
                d->tab[j].val = v;
                d->count++;

                next = (next + 1) & mask;
            }
            return true;
        }
        idx = (idx + 1) & mask;
    }
    return false;
}

/* Look up a define by name (case-insensitive via the hash).
 * in:  key — string view of the name
 * ret: pointer to the value string, or NULL if not found */
const char* defs_get(const defs_t* d, sv_t key) {
    if (!d || !d->tab || key.len == 0) return NULL;
    uint32_t h = hash_sv(key);
    size_t mask = d->cap - 1;
    size_t idx = h & mask;

    while (d->tab[idx].used) {
        if (d->tab[idx].key_len == key.len &&
            sv_ieq(key, d->tab[idx].key)) {
            return d->tab[idx].val;
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}

// ----------------------------
// .include / include expansion
// ----------------------------

/* Return a pointer to the last path separator in path, or NULL if none.
 * Used to extract the directory portion for relative include resolution. */
static const char* path_dirname_end(const char* path) {
    if (!path) return NULL;
    const char* last = NULL;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') last = p;
    }
    return last; // points to slash, or NULL
}

/* Return true if the path is absolute (starts with '/' or '\\', or 'X:'). */
static bool path_is_abs(const char* p) {
    if (!p || !*p) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    // Windows drive form "C:..."; harmless on Linux but keeps behavior sane.
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') return true;
    return false;
}

static char* path_join_dir_file(const char* base_file, const char* rel) {
    if (!base_file || !rel) return NULL;
    if (path_is_abs(rel)) return xstrdup_n(rel, strlen(rel));
    const char* slash = path_dirname_end(base_file);
    if (!slash) {
        // base has no directory component
        return xstrdup_n(rel, strlen(rel));
    }
    size_t dir_len = (size_t)(slash - base_file) + 1; // include slash
    size_t rel_len = strlen(rel);
    char* out = (char*)malloc(dir_len + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, base_file, dir_len);
    memcpy(out + dir_len, rel, rel_len);
    out[dir_len + rel_len] = '\0';
    return out;
}

/* Return true if c may appear inside an identifier token (for macro-argument scanning). */
static bool is_ident_ch(unsigned char c) {
    return (c == '.' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
}

/* Case-insensitive equality between a string view and a literal C string.
 * Thin wrapper around sv_ieq used for directive keyword checks. */
static bool tok_ieq_cstr(sv_t tok, const char* lit) {
    return sv_ieq(tok, lit);
}

/* Parse a quoted or angle-bracket include path from a directive line.
 * in:  line, line_len — the source line
 * out: *out_path_start, *out_path_len — byte range of the path (excluding quotes)
 * ret: false if no valid path is found */
static bool parse_include_path(const char* line, size_t line_len, size_t* out_path_off, size_t* out_path_len, unsigned* out_col) {
    // expects ... "path" (no escapes)
    size_t i = 0;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    
    if (i < line_len && line[i] == '#') i++; // allow #include/#includeroot
// first token (already parsed by caller)
    while (i < line_len && is_ident_ch((unsigned char)line[i])) i++;
    // skip ws
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line_len || line[i] != '"') {
        if (out_col) *out_col = (unsigned)(i + 1);
        return false;
    }
    size_t q0 = i;
    i++; // after opening quote
    size_t p0 = i;
    while (i < line_len && line[i] != '"') i++;
    if (i >= line_len) {
        if (out_col) *out_col = (unsigned)(q0 + 1);
        return false;
    }
    size_t p1 = i;
    if (out_path_off) *out_path_off = p0;
    if (out_path_len) *out_path_len = p1 - p0;
    if (out_col) *out_col = (unsigned)(p0 + 1);
    return true;
}

/* Relaxed include-path parser that also accepts unquoted paths.
 * Used as a fallback when parse_include_path finds no quoted form. */
static bool parse_path_arg_relaxed(const char* line, size_t line_len, size_t* out_path_off, size_t* out_path_len, unsigned* out_col) {
  // Accept either "quoted path" or a bare token path (until whitespace/comment).
  // For quoted, we delegate to parse_include_path().
  if (out_path_off) *out_path_off = 0;
  if (out_path_len) *out_path_len = 0;
  if (out_col) *out_col = 1;

  // skip directive token
  size_t i = 0;
  while (i < line_len && !pp_is_ws(line[i])) i++;
  while (i < line_len && pp_is_ws(line[i])) { i++; if (out_col) (*out_col)++; }
  if (i >= line_len) return false;

  if (line[i] == '"') {
    // Quoted path: find terminating quote.
    unsigned col = out_col ? *out_col : 1;
    size_t startq = i;
    i++; col++;
    size_t start = i;
    while (i < line_len && line[i] != '"') { i++; col++; }
    if (i >= line_len) return false;
    size_t len = i - start;
    if (out_path_off) *out_path_off = start;
    if (out_path_len) *out_path_len = len;
    if (out_col) *out_col = (unsigned)(startq + 1); // 1-based column of first char inside quotes
    return len > 0;
  }

  size_t start = i;
  unsigned col = out_col ? *out_col : 1;
  // token ends at whitespace or comment ';'
  while (i < line_len && !pp_is_ws(line[i]) && line[i] != ';') i++;
  size_t len = (i > start) ? (i - start) : 0;
  if (len == 0) return false;
  if (out_path_off) *out_path_off = start;
  if (out_path_len) *out_path_len = len;
  if (out_col) *out_col = col;
  return true;
}

// forward decl
/* Recursively expand .include directives in in_text.
 * Results are appended to out.  depth limits nesting to prevent cycles.
 * in:  file           — path of the file being processed (for diagnostics)
 *      in_text        — source text (already with comments stripped)
 *      expand_includes — if false, .include lines are blanked but not followed
 *      stack/stack_depth — paths currently being included (cycle detection) */
static bool pp_expand_includes_rec(const char* file, const char* in_text, sbuf_t* out, diags_t* diags, unsigned depth,
                                  bool expand_includes,
                                  const char** include_paths, size_t include_path_count,
                                  const char* root_dir, char** includeroot_dir,
                                  const char** include_stack, size_t include_stack_len);

/* Append a file-boundary marker line to out.
 * The marker uses the internal PP_FILE_MARK byte so it can be
 * detected and stripped by later passes without affecting line numbers. */
static bool pp_emit_file_mark(sbuf_t* out, const char* file) {
    if (!out || !file) return false;
    sbuf_putc(out, (char)PP_FILE_MARK);
    sbuf_puts(out, file);
    sbuf_putc(out, '\n');
    return true;
}

/* Append a line-number reset marker to out.
 * Used after macro expansion to restore the original line number. */
static void pp_emit_line_mark(sbuf_t* out, unsigned line_no) {
    if (!out) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", line_no);
    sbuf_putc(out, (char)PP_LINE_MARK);
    sbuf_puts(out, buf);
    sbuf_putc(out, '\n');
}


// Simplify a path string for comparison: collapses "/./" and repeated slashes.
// This is NOT a full canonicalization (it does not resolve ".." or symlinks),
// but it is sufficient to treat "a.asm" and "./a.asm" (and "/x/./y") as equal.
/* Allocate a normalised copy of path, collapsing './' and '../' components.
 * Used to detect include cycles. */
static char* path_simplify_dup(const char* p) {
    if (!p) return NULL;
    size_t n = strlen(p);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;

    size_t oi = 0;
    for (size_t i = 0; i < n; ) {
        char c = p[i];

        // Collapse repeated slashes.
        if (c == '/' || c == '\\') {
            char slash = '/';
            out[oi++] = slash;
            // Skip all subsequent slashes/backslashes.
            i++;
            while (i < n && (p[i] == '/' || p[i] == '\\')) i++;

            // If the next part is "./" (or ".\"), skip it.
            if (i + 1 < n && p[i] == '.' && (p[i+1] == '/' || p[i+1] == '\\')) {
                i += 2;
                continue;
            }
            continue;
        }

        out[oi++] = c;
        i++;
    }

    // Remove trailing "/." if present.
    if (oi >= 2 && out[oi-1] == '.' && out[oi-2] == '/') {
        oi -= 2;
        if (oi == 0) out[oi++] = '/';
    }
    // Remove trailing slash (except root "/").
    if (oi >= 2 && out[oi-1] == '/') {
        oi--;
    }

    out[oi] = '\0';
    // If the input was relative and began with "./", preserve it as relative by stripping leading "/"
    // introduced by the loop only when the input started with a slash; for relative we never add a leading slash.
    // (Nothing to do.)

    return out;
}

static bool path_dir_ends_with_slash(const char* p) {
    if (!p) return false;
    size_t n = strlen(p);
    if (n == 0) return false;
    return p[n - 1] == '/' || p[n - 1] == '\\';
}

/* Allocate a heap string joining dir and rel with a path separator.
 * If rel is absolute, returns a copy of rel.
 * in:  dir — directory component (may be NULL or empty)
 *      rel — relative path to append
 * ret: heap-allocated joined path */
static char* path_join_dir_rel(const char* dir, const char* rel) {
    if (!dir || !rel) return NULL;
    if (path_is_abs(rel)) return xstrdup_n(rel, strlen(rel));
    size_t dl = strlen(dir);
    size_t rl = strlen(rel);
    bool has = path_dir_ends_with_slash(dir);
    size_t extra = has ? 0 : 1;
    char* out = (char*)malloc(dl + extra + rl + 1);
    if (!out) return NULL;
    memcpy(out, dir, dl);
    if (!has) out[dl] = '/';
    memcpy(out + dl + extra, rel, rl);
    out[dl + extra + rl] = '\0';
    return out;
}

/* Return true if p contains a directory separator ('/' or '\\').
 * Used to decide whether an include path needs to be resolved relative
 * to the includeroot directory. */
static bool has_dir_sep(const char* p) {
    return (p && (strchr(p, '/') != NULL || strchr(p, '\\') != NULL));
}

// Compare two paths with a minimal normalization suitable for include-loop
// detection. We intentionally avoid filesystem canonicalization here.
static const char* path_skip_dot_slash(const char* p) {
    if (!p) return p;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    return p;
}

/* Return true if paths a and b refer to the same file.
 * Accepts both exact matches and basename-only matches to handle
 * mixed relative/absolute paths in the include stack. */
static bool path_eq_loose(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (strcmp(a, b) == 0) return true;
    a = path_skip_dot_slash(a);
    b = path_skip_dot_slash(b);
    return strcmp(a, b) == 0;
}

/* Allocate a heap copy of the directory portion of path (up to and
 * including the last separator), or "." if no separator is found. */
static char* path_dirname_dup(const char* file) {
    if (!file || !file[0]) return strdup(".");
    const char* s1 = strrchr(file, '/');
    const char* s2 = strrchr(file, '\\');
    const char* s = (s1 && s2) ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
    if (!s) return strdup(".");
    size_t n = (size_t)(s - file);
    if (n == 0) return strdup("/");
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, file, n);
    out[n] = '\0';
    return out;
}

#define PP_MAX_INCLUDE_DEPTH 32  /* must match stack2[] array size */

/* Recursive include-expansion implementation.
 * Processes each line of in_text: include directives are followed (up to depth 64),
 * other lines are passed through.  File-boundary and line-number markers are
 * inserted so later passes can produce correct diagnostics.
 * in:  depth            — current nesting depth (aborts at 64)
 *      root_dir         — directory of the root source file
 *      includeroot_dir  — set by .includeroot, used as fallback search dir
 *      include_stack    — paths of files currently being processed (cycle detection) */
static bool pp_expand_includes_rec(const char* file, const char* in_text, sbuf_t* out, diags_t* diags, unsigned depth,
                                  bool expand_includes,
                                  const char** include_paths, size_t include_path_count,
                                  const char* root_dir, char** includeroot_dir,
                                  const char** include_stack, size_t include_stack_len) {
    if (!file) file = "<input>";
    if (!in_text || !out) return false;
    if (depth >= PP_MAX_INCLUDE_DEPTH) {
        diag_addf(diags, DIAG_ERROR, file, 1, 1, 1, "include nesting too deep");
        return false;
    }

    // Note: recursive include loops are checked when resolving an include path.

    const char* p = in_text;
    unsigned line_no = 1;
    while (*p) {
        const char* ls = p;
        const char* le = ls;
        while (*le && *le != '\n') le++;
        size_t len = (size_t)(le - ls);

        // Preserve existing file markers if present
        if (len > 0 && (unsigned char)ls[0] == PP_FILE_MARK) {
            sbuf_puts_n(out, ls, len);
            sbuf_putc(out, '\n');
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        // Find first token (could include leading '.')
        size_t i = 0;
        while (i < len && (ls[i] == ' ' || ls[i] == '\t')) i++;
        size_t t0 = i;
        while (i < len && is_ident_ch((unsigned char)ls[i])) i++;
        size_t t1 = i;
        sv_t tok;
        tok.p = ls + t0;
        tok.len = (size_t)(t1 - t0);

        bool is_inc = (tok.len != 0) && (tok_ieq_cstr(tok, ".include") || tok_ieq_cstr(tok, "#include"));
        bool is_root = (tok.len != 0) && (tok_ieq_cstr(tok, ".includeroot") || tok_ieq_cstr(tok, "#includeroot"));

        /* .pragma once / #pragma once — mark this file so subsequent includes skip it. */
        if (tok.len != 0 && (tok_ieq_cstr(tok, ".pragma") || tok_ieq_cstr(tok, "#pragma"))) {
            size_t j = t1;
            while (j < len && (ls[j] == ' ' || ls[j] == '\t')) j++;
            size_t k = j;
            while (k < len && is_ident_ch((unsigned char)ls[k])) k++;
            sv_t arg; arg.p = ls + j; arg.len = k - j;
            if (tok_ieq_cstr(arg, "once")) {
                /* Normalise the current file path so comparisons are reliable. */
                char *norm = path_simplify_dup(file);
                once_add(norm ? norm : file);
                free(norm);
            }
            /* Either way, blank the .pragma line (no output) and continue. */
            sbuf_putc(out, '\n');
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        // Disallow missing-prefix directives (users must write .include/#include or .includeroot/#includeroot)
        if ((tok.len != 0) && tok_ieq_cstr(tok, "include")) {
            diag_addf(diags, DIAG_ERROR, file, line_no, (unsigned)(t0 + 1), (unsigned)tok.len,
                      "unknown directive 'include' (did you mean .include?)");
            return false;
        }
        if ((tok.len != 0) && tok_ieq_cstr(tok, "includeroot")) {
            diag_addf(diags, DIAG_ERROR, file, line_no, (unsigned)(t0 + 1), (unsigned)tok.len,
                      "unknown directive 'includeroot' (did you mean .includeroot?)");
            return false;
        }

        if (is_root) {
            if (depth != 0) {
                diag_addf(diags, DIAG_ERROR, file, line_no, (unsigned)(t0 + 1), (unsigned)tok.len,
                          ".includeroot is only allowed in the root file");
                return false;
            }

            // parse includeroot path (quoted or bare)
            size_t po = 0, pl = 0;
            unsigned col = (unsigned)(t0 + 1);
            if (!parse_path_arg_relaxed(ls + t1, len - t1, &po, &pl, &col)) {
                /* For missing/invalid args, underline the directive token itself. */
                diag_addf(diags, DIAG_ERROR, file, line_no, (unsigned)(t0 + 1), (unsigned)tok.len,
                          "includeroot requires a path");
                return false;
            }
            char* raw = xstrdup_n((ls + t1) + po, pl);
            if (!raw) return false;

            // Resolve relative paths against the root file's directory.
            char* resolved = NULL;
            if (path_is_abs(raw)) {
                resolved = xstrdup_n(raw, strlen(raw));
            } else {
                resolved = path_join_dir_rel(root_dir, raw);
            }
            free(raw);
            if (!resolved) return false;

            if (*includeroot_dir) free(*includeroot_dir);
            *includeroot_dir = resolved;

            // Preserve line numbers: emit a blank line for the directive.
            sbuf_putc(out, '\n');
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        if (!is_inc) {
            // normal line
            sbuf_puts_n(out, ls, len);
            sbuf_putc(out, '\n');
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        // For editor-time semantic analysis, we want line numbers to match the
        // original buffer. Disabling include expansion keeps the line count
        // stable by treating include directives as blank lines.
        if (!expand_includes) {
            sbuf_putc(out, '\n');
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        // parse include path
        size_t po = 0, pl = 0;
        unsigned col = 1;
        if (!parse_include_path(ls, len, &po, &pl, &col)) {
            diag_addf(diags, DIAG_ERROR, file, line_no, col, 1, "include requires a quoted path");
            return false;
        }

        char* rel = xstrdup_n(ls + po, pl);
        if (!rel) {
            diag_addf(diags, DIAG_ERROR, file, line_no, col, 1, "out of memory");
            return false;
        }

        // 1) resolve relative to includeroot if specified and the include has no explicit path
        //    (eg. "foo.asm"), otherwise resolve relative to the including file.
        char* inc_path = NULL;
        bool tried_root = false;
        if (includeroot_dir && *includeroot_dir && !has_dir_sep(rel) && !path_is_abs(rel)) {
            inc_path = path_join_dir_rel(*includeroot_dir, rel);
            tried_root = (inc_path != NULL);
        }
        if (!inc_path) {
            inc_path = path_join_dir_file(file, rel);
        }
        if (!inc_path) {
            diag_addf(diags, DIAG_ERROR, file, line_no, col, 1, "out of memory");
            free(rel);
            return false;
        }
        // Normalize the resolved include path for stable comparisons (e.g. collapse "/./").
        {
            char* simp = path_simplify_dup(inc_path);
            if (simp) {
                free(inc_path);
                inc_path = simp;
            }
        }

        // Guard against accidental recursive include loops (A includes B includes A).
        // Note: we compare the resolved include path strings as used by source_load.
        if (include_stack && include_stack_len) {
            for (size_t si = 0; si < include_stack_len; ++si) {
                const char* s = include_stack[si];
                if (s && path_eq_loose(s, inc_path)) {
                    // Highlight the full include directive (not just the quoted filename).
                    // The include site is what the user needs to fix.
                    // Mark the whole directive line as the error span.
                    // (diag len==0 only highlights a single character in the GTK editor)
                    diag_addf(diags, DIAG_ERROR, file, line_no, 1, (unsigned)len,
                              "recursive include detected: '%s'", rel);
                    free(rel);
                    free(inc_path);
                    return false;
                }
            }
        }

        /* pragma-once: if the file has already been included and is marked
         * .pragma once, silently skip it (emit a blank line to preserve the
         * line count of the including file). */
        if (once_has(inc_path)) {
            sbuf_putc(out, '\n');
            free(rel);
            free(inc_path);
            p = (*le == '\n') ? le + 1 : le;
            line_no++;
            continue;
        }

        source_t inc;
        bool loaded = source_load(inc_path, &inc);

        // If we tried includeroot first and it didn't exist, fall back to including-file relative.
        if (!loaded && tried_root) {
            char* alt2 = path_join_dir_file(file, rel);
            if (alt2) {
                if (source_load(alt2, &inc)) {
                    loaded = true;
                    free(inc_path);
                    // Normalize fallback include path too.
                    {
                        char* simp2 = path_simplify_dup(alt2);
                        if (simp2) {
                            free(alt2);
                            alt2 = simp2;
                        }
                    }
                    inc_path = alt2;
                } else {
                    free(alt2);
                }
            }
        }

        // 2) if not found, try -I include paths
        if (!loaded && include_paths && include_path_count) {
            for (size_t pi = 0; pi < include_path_count && !loaded; ++pi) {
                char* alt = path_join_dir_rel(include_paths[pi], rel);
                if (!alt) continue;
                // Normalize search-path candidate too (keeps stack comparisons consistent).
                {
                    char* simp3 = path_simplify_dup(alt);
                    if (simp3) {
                        free(alt);
                        alt = simp3;
                    }
                }
                if (source_load(alt, &inc)) {
                    loaded = true;
                    free(inc_path);
                    inc_path = alt;
                    break;
                }
                free(alt);
            }
        }

        if (!loaded) {
            // Highlight the full include directive (not just the quoted filename).
            // Mark the whole directive line as the error span.
            diag_addf(diags, DIAG_ERROR, file, line_no, 1, (unsigned)len,
                      "cannot read include file '%s'", rel);
            free(rel);
            free(inc_path);
            return false;
        }

        // Keep rel until after recursive expansion so we can emit a helpful
        // diagnostic at the include site if the include fails while expanding.
        source_strip_comments_preserve_layout(&inc);

        // Emit the include line itself (so listing can show it), then switch file context.
        sbuf_puts_n(out, ls, len);
        sbuf_putc(out, '\n');

        pp_emit_file_mark(out, inc_path);

        const char* stack2[PP_MAX_INCLUDE_DEPTH + 1];
        size_t stack2_len = 0;
        if (include_stack && include_stack_len) {
            stack2_len = include_stack_len;
            if (stack2_len >= PP_MAX_INCLUDE_DEPTH) {
                diag_addf(diags, DIAG_ERROR, file, line_no, col, (unsigned)pl,
                          "include nesting too deep");
                source_free(&inc);
                free(inc_path);
                return false;
            }
            memcpy(stack2, include_stack, stack2_len * sizeof(stack2[0]));
        }
        stack2[stack2_len++] = inc_path;

        bool ok = pp_expand_includes_rec(inc_path, inc.text, out, diags, depth + 1, expand_includes,
                                         include_paths, include_path_count,
                                         root_dir, includeroot_dir,
                                         stack2, stack2_len);
        source_free(&inc);
        if (!ok) {
            // Surface the failure at the include site too (editor UX).
            // Highlight the full include directive (not just the quoted filename).
            // Mark the whole directive line as the error span.
            diag_addf(diags, DIAG_ERROR, file, line_no, 1, (unsigned)len,
                      "include failed while expanding '%s'", rel);
            free(rel);
            free(inc_path);
            return false;
        }
        // Restore parent file context and reset line counter to the line
        // after the .include directive, so the assembler attributes any
        // subsequent errors to the correct lines in the parent file.
        pp_emit_file_mark(out, file);
        pp_emit_line_mark(out, line_no + 1);

        free(inc_path);
        free(rel);

        p = (*le == '\n') ? le + 1 : le;
        line_no++;
    }
    return true;
}


// --- macros (simple text macros) ---
typedef struct {
    char* name;        // canonical stored as-is (case-sensitive compare via sv_ieq)
    char** arg_names;  // optional named args
    size_t arg_count;
    char* body;        // newline-separated lines, each line as written (no trailing NUL issues)
    unsigned def_line;
    unsigned def_col;
    bool used;
} macro_entry_t;

typedef struct {
    macro_entry_t* tab;
    size_t cap;
    size_t count;
} macros_t;

/* Initialise an empty macro table. */
static void macros_init(macros_t* m) {
    memset(m, 0, sizeof(*m));
}

/* Free all heap memory owned by a single macro_entry_t (name, arg names, body). */
static void macro_entry_free(macro_entry_t* e) {
    if (!e || !e->used) return;
    free(e->name);
    if (e->arg_names) {
        for (size_t i = 0; i < e->arg_count; ++i) free(e->arg_names[i]);
        free(e->arg_names);
    }
    free(e->body);
    memset(e, 0, sizeof(*e));
}

/* Free all macro entries and the entries array. */
static void macros_free(macros_t* m) {
    if (!m || !m->tab) return;
    for (size_t i = 0; i < m->cap; ++i) macro_entry_free(&m->tab[i]);
    free(m->tab);
    memset(m, 0, sizeof(*m));
}

/* Grow the macro table to new_cap entries. */
static bool macros_grow(macros_t* m, size_t new_cap) {
    macro_entry_t* nt = (macro_entry_t*)calloc(new_cap, sizeof(macro_entry_t));
    if (!nt) return false;
    // rehash
    for (size_t i = 0; i < m->cap; ++i) {
        macro_entry_t* e = &m->tab[i];
        if (!e->used) continue;
        sv_t key = { e->name, strlen(e->name) };
        uint32_t h = hash_sv(key);
        size_t mask = new_cap - 1;
        size_t idx = (size_t)h & mask;
        while (nt[idx].used) idx = (idx + 1) & mask;
        nt[idx] = *e; // shallow move; ownership transferred
        memset(e, 0, sizeof(*e));
    }
    free(m->tab);
    m->tab = nt;
    m->cap = new_cap;
    return true;
}

/* Add or replace a macro definition.
 * in:  name      — macro name (copied)
 *      arg_names — array of argument name strings (ownership transferred)
 *      arg_count — length of arg_names
 *      body      — array of body line strings (ownership transferred)
 *      body_count — length of body */
static bool macros_put(macros_t* m, const char* name, char** arg_names, size_t arg_count, const char* body,
                       unsigned def_line, unsigned def_col) {
    if (m->cap == 0) {
        if (!macros_grow(m, 64)) return false;
    }
    if ((m->count + 1) * 10 >= m->cap * 7) { // load > 0.7
        if (!macros_grow(m, m->cap * 2)) return false;
    }
    sv_t key = sv_from_cstr(name);
    uint32_t h = hash_sv(key);
    size_t mask = m->cap - 1;
    size_t idx = (size_t)h & mask;
    while (m->tab[idx].used) {
        if (sv_ieq(key, m->tab[idx].name)) {
            // replace existing
            macro_entry_free(&m->tab[idx]);
            break;
        }
        idx = (idx + 1) & mask;
    }
    macro_entry_t* e = &m->tab[idx];
    e->used = true;
    e->name = xstrdup_n(name, strlen(name));
    e->arg_names = arg_names;
    e->arg_count = arg_count;
    e->body = xstrdup_n(body, strlen(body));
    e->def_line = def_line;
    e->def_col = def_col;
    if (!e->name || !e->body) return false;
    m->count++;
    return true;
}

/* Look up a macro by name (case-insensitive).
 * ret: pointer to the entry, or NULL if not found */
static const macro_entry_t* macros_get(const macros_t* m, sv_t name) {
    if (!m || !m->tab || m->cap == 0) return NULL;
    uint32_t h = hash_sv(name);
    size_t mask = m->cap - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        const macro_entry_t* e = &m->tab[idx];
        if (!e->used) return NULL;
        if (sv_ieq(name, e->name)) return e;
        idx = (idx + 1) & mask;
    }
}
// --- tiny growable buffer ---
struct sbuf_t {
    char* p;
    size_t len;
    size_t cap;
};

/* Ensure the string buffer has room for add more bytes.
 * Doubles capacity until sufficient, starting at 4096. */
static bool sbuf_reserve(sbuf_t* b, size_t add) {
    size_t need = b->len + add + 1;
    if (need <= b->cap) return true;
    size_t ncap = b->cap ? b->cap * 2 : 4096;
    while (ncap < need) ncap *= 2;
    char* np = (char*)realloc(b->p, ncap);
    if (!np) return false;
    b->p = np;
    b->cap = ncap;
    return true;
}
/* Append a single character c to the string buffer. */
static bool sbuf_putc(sbuf_t* b, char c) {
    if (!sbuf_reserve(b, 1)) return false;
    b->p[b->len++] = c;
    b->p[b->len] = 0;
    return true;
}
/* Append n bytes from s to the string buffer. */
static bool sbuf_puts_n(sbuf_t* b, const char* s, size_t n) {
    if (!sbuf_reserve(b, n)) return false;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return true;
}

/* Append a NUL-terminated string to the string buffer. */
static bool sbuf_puts(sbuf_t* b, const char* s) {
    return sbuf_puts_n(b, s, s ? strlen(s) : 0);
}

/* Advance p past spaces, tabs, CR, and the internal macro-expansion marker (0x1E). */
static const char* skip_ws(const char* p) {
    // Also skip the internal "from-macro" marker (0x1E) which may appear
    // at the start of expanded lines.
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\x1e') ++p;
    return p;
}



/* Parse an identifier starting at p (skipping leading whitespace).
 * Dot-prefixed identifiers are rejected (directives, not names).
 * out: *out_ident — string view of the identifier
 *      *out_col   — 1-based column of the identifier start
 * ret: false if no identifier is found */
static bool parse_ident_sv(const char* p, const char* line_start, sv_t* out_ident, unsigned* out_col) {
    const char* q = skip_ws(p);
    if (!is_ident_start(*q) || *q == '.') return false;
    const char* b = q;
    q++;
    while (is_ident_char(*q)) q++;
    out_ident->p = b;
    out_ident->len = (size_t)(q - b);
    if (out_col) *out_col = (unsigned)(b - line_start) + 1;
    return true;
}

static char** split_commas_trim(const char* p, size_t len, size_t* out_count) {
    // Split by commas, ignoring commas inside double-quoted strings
    size_t cap = 8, n = 0;
    char** arr = (char**)calloc(cap, sizeof(char*));
    if (!arr) return NULL;
    bool in_str = false;
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        char c = (i == len) ? ',' : p[i];
        if (i < len && c == '"') in_str = !in_str;
        if (!in_str && (i == len || c == ',')) {
            size_t a = start;
            size_t b = i;
            while (a < b && (p[a] == ' ' || p[a] == '\t' || p[a] == '\r')) a++;
            while (b > a && (p[b-1] == ' ' || p[b-1] == '\t' || p[b-1] == '\r')) b--;
            size_t seg_len = (b > a) ? (b - a) : 0;
            if (seg_len) {
                if (n == cap) {
                    cap *= 2;
                    char** na = (char**)realloc(arr, cap * sizeof(char*));
                    if (!na) { 
                        for (size_t k = 0; k < n; ++k) free(arr[k]);
                        free(arr);
                        return NULL;
                    }
                    arr = na;
                }
                arr[n++] = xstrdup_n(p + a, seg_len);
            }
            start = i + 1;
        }
    }
    *out_count = n;
    return arr;
}

/* Replace $NAME occurrences in line[0..line_len) with their .def values.
 * $NAME is only substituted outside double-quoted strings.
 * Unrecognised $names are passed through unchanged.
 * out: *out — output buffer receiving the substituted text */
static bool substitute_defs_into(sbuf_t* out, const char* line, size_t line_len, const defs_t* defs) {
    bool in_str = false;
    for (size_t i = 0; i < line_len; ++i) {
        char c = line[i];
        if (c == '"') {
            in_str = !in_str;
            if (!sbuf_putc(out, c)) return false;
            continue;
        }
        if (!in_str && c == '$') {
            size_t j = i + 1;
            if (j < line_len && is_ident_start(line[j]) && line[j] != '.') {
                size_t k = j + 1;
                while (k < line_len && is_ident_char(line[k])) k++;
                sv_t key = { line + j, k - j };
                const char* v = defs_get(defs, key);
                if (v) {
                    if (!sbuf_puts(out, v)) return false;
                    i = k - 1;
                    continue;
                }
            }
        }
        if (!sbuf_putc(out, c)) return false;
    }
    return true;
}

/* Substitute macro argument placeholders in a single macro body line.
 * Positional placeholders: \1..\9  Named placeholders: \argname
 * out: *out — output buffer */
static bool substitute_macro_line(sbuf_t* out, const char* line, size_t line_len,
                                 char** arg_names, size_t arg_count,
                                 char** arg_vals, size_t val_count) {
    /* Replace \1..\9, \name, and bare token arg names with corresponding arg values.
     * Bare names (e.g. "address" when "address" is a declared arg) are substituted
     * when they appear as complete identifier tokens outside string literals/comments. */
    bool in_str = false;
    char str_ch = 0;
    for (size_t i = 0; i < line_len; ++i) {
        char c = line[i];

        /* Track string literals so we don't substitute inside them */
        if (!in_str && (c == '"' || c == '\'')){
            in_str = true; str_ch = c;
            if (!sbuf_putc(out, c)) return false;
            continue;
        }
        if (in_str) {
            if (c == str_ch) in_str = false;
            if (!sbuf_putc(out, c)) return false;
            continue;
        }
        /* Stop at comment */
        if (c == ';') {
            if (!sbuf_puts_n(out, line + i, line_len - i)) return false;
            return true;
        }

        if (c == '\\') {
            if (i + 1 < line_len) {
                char n = line[i+1];
                /* positional \1..\9 */
                if (n >= '1' && n <= '9') {
                    size_t idx = (size_t)(n - '1');
                    if (idx < val_count && arg_vals[idx]) {
                        if (!sbuf_puts(out, arg_vals[idx])) return false;
                    }
                    i += 1;
                    continue;
                }
                /* named \argname */
                if (is_ident_start(n)) {
                    size_t j = i + 1;
                    size_t k = j + 1;
                    while (k < line_len && is_ident_char(line[k])) k++;
                    sv_t nm = { line + j, k - j };
                    bool found = false;
                    for (size_t a = 0; a < arg_count; ++a) {
                        if (arg_names[a] && sv_ieq(nm, arg_names[a])) {
                            if (a < val_count && arg_vals[a]) {
                                if (!sbuf_puts(out, arg_vals[a])) return false;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        /* Unknown \name — emit literally */
                        if (!sbuf_puts_n(out, line + i, k - i)) return false;
                    }
                    i = k - 1;
                    continue;
                }
            }
            if (!sbuf_putc(out, c)) return false;
            continue;
        }

        /* Bare identifier token: check if it matches an arg name */
        if (is_ident_start((unsigned char)c)) {
            size_t j = i + 1;
            while (j < line_len && is_ident_char(line[j])) j++;
            sv_t nm = { line + i, j - i };
            bool found = false;
            for (size_t a = 0; a < arg_count; ++a) {
                if (arg_names[a] && sv_ieq(nm, arg_names[a])) {
                    if (a < val_count && arg_vals[a]) {
                        if (!sbuf_puts(out, arg_vals[a])) return false;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!sbuf_puts_n(out, line + i, j - i)) return false;
            }
            i = j - 1;
            continue;
        }

        if (!sbuf_putc(out, c)) return false;
    }
    return true;
}

// Scan a line for an optional leading label definition followed by a macro invocation.
// Returns true if a macro token is found; outputs macro name token and the prefix (label+ws) length.
static bool scan_macro_invocation(const char* line, size_t line_len,
                                  sv_t* out_macro, size_t* out_prefix_len) {
    const char* p = line;
    const char* end = line + line_len;
    p = skip_ws(p);
    if (p >= end) return false;

    // token0
    const char* t0 = p;
    const char* t1 = t0;
    while (t1 < end && is_ident_char(*t1)) ++t1;
    if (t1 == t0) return false;

    // Is token0 a label definition?
    bool tok0_is_label_def = false;
    const char* after_tok0 = t1;
    if (after_tok0 < end && *after_tok0 == ':') {
        tok0_is_label_def = true;
        after_tok0++; // consume ':'
    } else if (*t0 == '.') {
        // Dot labels are label definitions at start of line, but many directives also start with '.'.
        // Treat token0 as a dot-label definition only when it's followed by another identifier token
        // (e.g. ".loop MACRO ..."). If the next token isn't an identifier (e.g. ".org $1000"),
        // don't treat it as a label definition here.
        const char* nxt = skip_ws(after_tok0);
        if (nxt < end && is_ident_start(*nxt)) tok0_is_label_def = true;
    }

    const char* macro_start = t0;
    if (tok0_is_label_def) {
        macro_start = skip_ws(after_tok0);
        if (macro_start >= end) return false;
        const char* m1 = macro_start;
        while (m1 < end && is_ident_char(*m1)) ++m1;
        if (m1 == macro_start) return false;
        out_macro->p = macro_start;
        out_macro->len = (size_t)(m1 - macro_start);
        *out_prefix_len = (size_t)(macro_start - line);
        return true;
    }

    // no label definition; macro is token0
    out_macro->p = t0;
    out_macro->len = (size_t)(t1 - t0);
    *out_prefix_len = 0;
    return true;
}

/* Free an array of n heap strings and the array itself. */
static void free_split(char** arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
}


/* Convenience wrapper: calls preproc_run_ex with expand_includes=true. */
char* preproc_run(const char* file, const char* in_text, diags_t* diags, defs_t* out_defs,
                 char* io_hex_prefix, bool hex_prefix_cli, char bin_prefix,
                 const char** include_paths, size_t include_path_count) {
    return preproc_run_ex(file, in_text, diags, out_defs, io_hex_prefix, hex_prefix_cli, bin_prefix,
                          true, include_paths, include_path_count);
}

/* Expand macros on a single preprocessed line.
 * If the line invokes a known macro, the body is expanded with argument
 * substitution and the result is written to out (with line markers).
 * Non-macro lines are passed through unchanged.
 * ret: false on OOM */

/* ---------------------------------------------------------------------------
 * colon_split_next: find the next statement separator ':' in a line.
 *
 * Rules:
 *   - ':' at position 0 or immediately after an identifier (label def) is NOT
 *     a separator — e.g. "label:" or ".label:" are labels.
 *   - ':' inside a string literal (single or double quoted) is NOT a separator.
 *   - ':' after a comment character (';') is NOT a separator.
 *   - All other ':' are statement separators.
 *
 * Returns pointer to the ':' separator, or NULL if none found.
 * out_label_end: if the line starts with a label "foo:", points past the ':'
 * so the caller can handle "label: LDA #1 : STA $10" correctly.
 * --------------------------------------------------------------------------- */


static bool process_line_expand(sbuf_t* out, const char* file, unsigned line_no,
                                const char* line, size_t line_len,
                                const defs_t* defs, const macros_t* macros,
                                diags_t* diags,
                                unsigned depth,
                                bool from_macro);

// Conditional assembly
typedef struct {
    bool parent_active;
    bool active;
    bool saw_else;
    bool taken; // any prior branch in this if-chain has been taken
} pp_if_t;

/* Bounded whitespace skip used in expression scanning (no macro-marker). */
static const char* skip_ws_n(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

// Replace occurrences of defined(NAME) in a #if expression with 1/0.
// Runs after $name substitution.
/* Replace defined(NAME) expressions in a .if expression with 1 or 0
 * depending on whether NAME has been .def'd.
 * Runs after $name substitution so that defined($name) also works.
 * in:  s    — expression text
 *      defs — current define table
 * out: *out — rewritten expression */
static bool pp_replace_defined_calls(sbuf_t* out, sv_t s, const defs_t* defs) {
    const char* p = s.p;
    const char* end = s.p + s.len;
    while (p < end) {
        // Look for identifier "defined" with token boundaries.
        if ((end - p) >= 7 && sv_ieq((sv_t){p, 7}, "defined")) {
            unsigned char prev = (p == s.p) ? 0 : (unsigned char)p[-1];
            unsigned char next = (p + 7 < end) ? (unsigned char)p[7] : 0;
            if (!is_ident_ch(prev) && !is_ident_ch(next)) {
                const char* q = p + 7;
                q = skip_ws_n(q, end);
                if (q < end && *q == '(') {
                    q++;
                    q = skip_ws_n(q, end);
                    const char* n0 = q;
                    if (q < end && is_ident_start((unsigned char)*q) && *q != '.') {
                        q++;
                        while (q < end && is_ident_char((unsigned char)*q)) q++;
                        sv_t name = { n0, (size_t)(q - n0) };
                        q = skip_ws_n(q, end);
                        if (q < end && *q == ')') {
                            bool def = defs_get(defs, name) != NULL;
                            if (!sbuf_putc(out, def ? '1' : '0')) return false;
                            p = q + 1;
                            continue;
                        }
                    }
                }
            }
        }

        if (!sbuf_putc(out, *p++)) return false;
    }
    return true;
}

// In #if/#elif expressions we allow bare identifiers to refer to #def names.
// This is *only* for conditional expressions; normal $name substitution rules
// remain unchanged.
/* Replace bare identifier references in a .if expression with their .def
 * values so that  .if VERSION == 3  works without a $ prefix.
 * Identifiers that are not in the define table are left unchanged.
 * out: *out — rewritten expression */
static bool pp_replace_bare_defs_in_expr(sbuf_t* out, sv_t s, const defs_t* defs) {
    if (!out) return false;
    const char* p = s.p;
    const char* end = s.p + s.len;
    while (p < end) {
        // Preserve string literals verbatim.
        if (*p == '"') {
            const char* q = p + 1;
            while (q < end) {
                if (*q == '"') { q++; break; }
                if (*q == '\\' && (q + 1) < end) { q += 2; continue; }
                q++;
            }
            if (!sbuf_puts_n(out, p, (size_t)(q - p))) return false;
            p = q;
            continue;
        }

        // Don't touch $name (handled in substitute_defs_into).
        if (*p == '$') {
            const char* q = p + 1;
            if (q < end && is_ident_start((unsigned char)*q) && *q != '.') {
                q++;
                while (q < end && is_ident_char((unsigned char)*q)) q++;
                if (!sbuf_puts_n(out, p, (size_t)(q - p))) return false;
                p = q;
                continue;
            }
        }

        // Replace bare identifiers with #def values if present.
        if (is_ident_start((unsigned char)*p) && *p != '.') {
            const char* q = p + 1;
            while (q < end && is_ident_char((unsigned char)*q)) q++;
            sv_t name = { p, (size_t)(q - p) };
            const char* v = defs_get(defs, name);
            if (v) {
                if (!sbuf_puts(out, v)) return false;
            } else {
                if (!sbuf_puts_n(out, p, (size_t)(q - p))) return false;
            }
            p = q;
            continue;
        }

        if (!sbuf_putc(out, *p)) return false;
        p++;
    }
    return true;
}

/* Parse a '#'-prefixed directive keyword from the start of a line.
 * Skips leading whitespace.  Returns the keyword without the '#'.
 * out: *out_kw   — keyword token (e.g. "ifdef")
 *      *out_rest — pointer to remainder of line after keyword
 *      *out_col  — 1-based column of the '#'
 * ret: false if the line does not start with a '#' directive */
static bool pp_parse_hash_kw(const char* line, size_t len, sv_t* out_kw, const char** out_rest, unsigned* out_col) {
    size_t i = 0;
    // Allow an internal macro-expansion marker (0x1E) before directives.
    while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\x1e')) i++;
    if (i >= len || line[i] != '#') return false;
    size_t hash_col = i + 1;
    i++;
    size_t k0 = i;
    while (i < len && is_ident_ch((unsigned char)line[i])) i++;
    if (i == k0) return false;
    if (out_kw) *out_kw = (sv_t){ line + k0, i - k0 };
    // rest begins after whitespace
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (out_rest) *out_rest = line + i;
    if (out_col) *out_col = (unsigned)hash_col;
    return true;
}

// Parse directives in the form ".if ...", ".ifdef ..." etc.
// Returns keyword without the leading '.' (so ".if" => "if").
static bool pp_parse_dot_kw(const char* line, size_t len, sv_t* out_kw, const char** out_rest, unsigned* out_col) {
    size_t i = 0;
    // Allow an internal macro-expansion marker (0x1E) before directives.
    while (i < len && (line[i] == ' ' || line[i] == '	' || line[i] == '\x1e')) i++;
    if (i >= len || line[i] != '.') return false;
    size_t dot_col = i + 1;
    i++;
    size_t k0 = i;
    while (i < len && is_ident_ch((unsigned char)line[i])) i++;
    if (i == k0) return false;
    if (out_kw) *out_kw = (sv_t){ line + k0, i - k0 };
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (out_rest) *out_rest = line + i;
    if (out_col) *out_col = (unsigned)dot_col;
    return true;
}

/* Append line_len spaces (and optionally a newline) to out.
 * Used to blank directive lines while preserving line numbers. */
static void pp_blank_line(sbuf_t* out, size_t line_len, bool has_nl) {
    for (size_t i = 0; i < line_len; ++i) sbuf_putc(out, ' ');
    if (has_nl) sbuf_putc(out, '\n');
}

/* Warn if a .if/.elif expression uses bare '=' instead of '==' */
static void pp_warn_bare_assign(const char* file, unsigned line_no,
                                const char* expr_s, size_t expr_len,
                                diags_t* diags) {
    bool in_str = false;
    for (size_t i = 0; i < expr_len; i++) {
        char c = expr_s[i];
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (c == '=') {
            /* Check it is not part of ==, !=, <=, >= */
            char prev = (i > 0) ? expr_s[i - 1] : 0;
            char next = (i + 1 < expr_len) ? expr_s[i + 1] : 0;
            if (next == '=') { i++; continue; } /* == — skip both */
            if (prev == '!' || prev == '<' || prev == '>' || prev == '=')
                continue; /* !=, <=, >=, == trailing */
            /* bare single = found */
            diag_addf(diags, DIAG_WARNING, file, line_no,
                      (unsigned)i + 1, 1,
                      "use '==' for comparison in .if expression; '=' treated as '=='");
            return; /* one warning is enough */
        }
    }
}

/* Evaluate a .if/.elif expression to a boolean.
 * Pipeline: $name substitution -> defined() expansion -> bare-ident lookup
 *           -> numeric expression evaluation via expr_eval.
 * in:  expr_s, expr_len — expression text slice
 *      defs             — current define table
 * out: *out_true        — result
 * ret: false on evaluation error (diagnostic emitted) */
static bool pp_eval_if_expr(const char* file, unsigned line_no,
                            const char* expr_s, size_t expr_len,
                            const defs_t* defs,
                            bool* out_true,
                            diags_t* diags) {
    if (!out_true) return false;

    // #if/#elif expressions support boolean operators (&&, ||, !) and
    // comparisons (==, !=, <=, >=) via the expression evaluator.
    // Apply $name substitution first (skipping strings) so #if can reference #def.
    sbuf_t tmp = {0};
    if (!substitute_defs_into(&tmp, expr_s, expr_len, defs)) {
        free(tmp.p);
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "out of memory");
        return false;
    }
    // After $name substitution, also handle defined(NAME) -> 1/0.
    sbuf_t tmp2 = {0};
    sv_t sub = { tmp.p ? tmp.p : "", tmp.len };
    if (!pp_replace_defined_calls(&tmp2, sub, defs)) {
        free(tmp2.p);
        free(tmp.p);
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "out of memory");
        return false;
    }

    // Also allow bare identifiers to refer to #def names in #if/#elif.
    sbuf_t tmp3 = {0};
    sv_t sub2 = { tmp2.p ? tmp2.p : "", tmp2.len };
    if (!pp_replace_bare_defs_in_expr(&tmp3, sub2, defs)) {
        free(tmp3.p);
        free(tmp2.p);
        free(tmp.p);
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "out of memory");
        return false;
    }

    sv_t src = { tmp3.p ? tmp3.p : "", tmp3.len };
    symtab_t st; symtab_init(&st);
    bool unresolved = false;
    expr_val_t v = {0};
    // In preprocessor conditionals we require the expression to be fully
    // resolvable after #def / defined() expansion.
    bool ok = expr_eval(file, line_no, 1, src, &st, NULL, 0, 0, false, &unresolved, &v, diags, NULL, NULL);
    symtab_free(&st);
    free(tmp3.p);
    free(tmp2.p);
    free(tmp.p);
    if (!ok) {
        expr_val_free(&v);
        return false;
    }
    if (unresolved) {
        expr_val_free(&v);
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "#if expression contains undefined identifiers (use #def or defined(NAME))");
        return false;
    }
    if (v.kind != EXPR_VAL_NUM) {
        expr_val_free(&v);
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "#if expression must be numeric");
        return false;
    }
    *out_true = (v.num != 0);
    expr_val_free(&v);
    return true;
}

/* Run the full preprocessor pipeline on in_text.
 *
 * Stages (in order):
 *   1. .include expansion (recursive, cycle-detected)
 *   2. .macro / .endmacro definition and invocation expansion
 *   3. .def/#def definitions and $name substitution
 *   4. .if/.ifdef/.ifndef/.elif/.else/.endif conditional assembly
 *   5. .cpu directive handling (writes defs["cpu"])
 *   6. .assert/.error/.warning emission
 *
 * in:  file             — root source file path (for diagnostics and include resolution)
 *      in_text          — NUL-terminated source (comments already stripped)
 *      expand_includes  — if false, .include lines are blanked not followed
 *      include_paths    — optional extra search directories for includes
 * out: *out_defs        — define table after preprocessing (caller must defs_free)
 *      *io_hex_prefix   — may be updated by .hexprefix directives
 * ret: newly allocated NUL-terminated preprocessed text, or empty string on error.
 *      Caller must free. */
char* preproc_run_ex(const char* file, const char* in_text, diags_t* diags, defs_t* out_defs,
                     char* io_hex_prefix, bool hex_prefix_cli, char bin_prefix,
                     bool expand_includes,
                     const char** include_paths, size_t include_path_count) {
    // First, expand .include/include directives (recursively) so later stages
    // (macros, #def, $name substitution) see a single logical stream.
    once_reset();
    sbuf_t inc = {0};
    char* root_dir = path_dirname_dup(file);
    char* includeroot_dir = NULL;

    const char* stack0[64];
    char* root_norm = NULL;
    if (file) {
        root_norm = path_simplify_dup(file);
    }
    stack0[0] = root_norm ? root_norm : (file ? file : "<input>");
    const char* root_for_expand = root_norm ? root_norm : file;
    if (!pp_expand_includes_rec(root_for_expand, in_text, &inc, diags, 0, expand_includes,
                                include_paths, include_path_count,
                                root_dir, &includeroot_dir,
                                stack0, 1)) {
        free(root_norm);
        free(root_dir);
        free(includeroot_dir);
        free(inc.p);
        return xstrdup_n("", 0);
    }
    free(root_norm);
    free(root_dir);
    free(includeroot_dir);

    sbuf_t out = {0};
    macros_t macros; macros_init(&macros);

    pp_if_t ifstk[64];
    size_t ifsp = 0;
    bool active = true;

    unsigned line = 1;
    const char* p = inc.p ? inc.p : "";

    // .cpu policy tracking:
    //   - must appear before any non-blank/non-comment/non-.def content
    //   - value must be one of: 6502, 65c02 (case-insensitive)
    bool cpu_seen = false;
    bool saw_nontrivial_before_cpu = false;

    bool cpu_effective_set = false;
    unsigned cpu_effective_line = 0;

    // Set the default CPU to 65c02 so that `cpu` is always defined even
    // when no .cpu directive appears in the source.  A .cpu directive later
    // in the file will overwrite this via defs_set.
    defs_set(out_defs, "cpu", "65c02");

    while (*p) {
        // file marker lines pass through unchanged
        if ((unsigned char)*p == PP_FILE_MARK) {
            const char* line_end = strchr(p, '\n');
            size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
            sbuf_puts_n(&out, p, line_len);
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            continue;
        }
        // line-number reset markers pass through unchanged (do not count as source lines)
        if ((unsigned char)*p == PP_LINE_MARK) {
            const char* line_end = strchr(p, '\n');
            size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
            sbuf_puts_n(&out, p, line_len);
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            /* no line++ — the marker is not a source line */
            continue;
        }
        const char* line_start = p;
        const char* line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);

        const char* q = skip_ws(p);

        // Track whether something other than blank/comment/.def appears before the first .cpu.
        // This enforces the "top of file" .cpu placement policy.
        if (!cpu_seen) {
            const char* endp = line_start + line_len;
            const char* t = q;
            // blank
            if (t >= endp) {
                /* ignore */
            } else if (*t == ';') {
                /* comment-only */
            } else if (*t == '.' || *t == '#') {
                /* allow any directive before .cpu (e.g. .if/.else/.endif, .def, #if, .org, etc.) */
            } else {
                saw_nontrivial_before_cpu = true;
            }
        }

        // ----------------------------
        // Conditional assembly:
        //   #if/#ifdef/#ifndef/#elif/#else/#endif/#endifdef
        //   .if/.ifdef/.ifndef/.elif/.elseif/.else/.endif/.endifdef
        // and assertions:
        //   #assert / .assert
        // ----------------------------
        sv_t hkw = {0};
        const char* hrest = NULL;
        unsigned hcol = 1;
        bool is_dot = false;
        if (!pp_parse_hash_kw(p, line_len, &hkw, &hrest, &hcol)) {
            if (pp_parse_dot_kw(p, line_len, &hkw, &hrest, &hcol)) {
                is_dot = true;
            }
        }

        if (hkw.len) {
            // Normalize keyword comparisons case-insensitively via sv_ieq.
            if (sv_ieq(hkw, "cpu")) {
                // .cpu <name>
                // Sets a predefined symbol "cpu" for conditional assembly:
                //   .if cpu == 6502
                //   .if cpu == 6503   ; (65C02)
                cpu_seen = true;
                if (saw_nontrivial_before_cpu) {
	                    /* hkw is "cpu" (len=3) but we want to underline ".cpu" (len=4). */
	                    diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                              ".cpu must appear before first instruction");
                }

                if (active) {

                    const char* r = hrest;
                    while (*r == ' ' || *r == '\t') r++;
                    const char* n0 = r;
                    while (*r && *r != ';' && *r != '\r' && *r != '\n' && *r != ' ' && *r != '\t') r++;
                    sv_t name = { n0, (size_t)(r - n0) };
                    if (!name.len) {
	                        diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                  ".cpu requires an argument (e.g. 6502 or 65c02)");
                    } else if (sv_ieq(name, "6502")) {
                        if (cpu_effective_set) {
                            diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                      "Multiple .cpu directives (already set at line %u)", cpu_effective_line);
                        } else {
                            defs_set(out_defs, "cpu", "6502");
                            cpu_effective_set = true;
                            cpu_effective_line = line;
                        }
                    } else if (sv_ieq(name, "65c02")) {
                        // Keep the define as "65c02" so tooltips and other UI show the expected value.
                        // Expression evaluation still works because parse_int_literal() recognizes 65c02.
                        if (cpu_effective_set) {
                            diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                      "Multiple .cpu directives (already set at line %u)", cpu_effective_line);
                        } else {
                            defs_set(out_defs, "cpu", "65c02");
                            cpu_effective_set = true;
                            cpu_effective_line = line;
                        }
                    } else if (sv_ieq(name, "6502u") || sv_ieq(name, "6502U")) {
                        // 6502 with undocumented/illegal opcodes enabled
                        if (cpu_effective_set) {
                            diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                      "Multiple .cpu directives (already set at line %u)", cpu_effective_line);
                        } else {
                            defs_set(out_defs, "cpu", "6502u");
                            cpu_effective_set = true;
                            cpu_effective_line = line;
                        }
                    } else if (sv_ieq(name, "65816") || sv_ieq(name, "65c816") ||
                               sv_ieq(name, "65816s") || sv_ieq(name, "w65c816")) {
                        // 65816 / 65C816
                        if (cpu_effective_set) {
                            diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                      "Multiple .cpu directives (already set at line %u)", cpu_effective_line);
                        } else {
                            defs_set(out_defs, "cpu", "65816");
                            cpu_effective_set = true;
                            cpu_effective_line = line;
                        }
                    } else {
                        diag_addf(diags, DIAG_ERROR, file, line, hcol, (unsigned)hkw.len + 1,
                                  "Invalid CPU type: %.*s\nAllowed values: 6502, 6502u, 65c02, 65816",
                                  (int)name.len, name.p);
                    }
                }

                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

            if (sv_ieq(hkw, "ifdef") || sv_ieq(hkw, "ifndef") || sv_ieq(hkw, "if")) {
                bool parent_active = active;
                bool take = false;
                if (parent_active) {
                    if (sv_ieq(hkw, "ifdef") || sv_ieq(hkw, "ifndef")) {
                        const char* r = hrest;
                        const char* n0 = r;
                        if (!is_ident_start((unsigned char)*r) || *r == '.') {
                            diag_addf(diags, DIAG_ERROR, file, line, (unsigned)(r - line_start) + 1, 1,
                                      "%s%.*s requires an identifier", is_dot ? "." : "#", (int)hkw.len, hkw.p);
                            take = false;
                        } else {
                            r++;
                            while (is_ident_char((unsigned char)*r)) r++;
                            sv_t name = { n0, (size_t)(r - n0) };
                            bool defined = defs_get(out_defs, name) != NULL;
                            take = sv_ieq(hkw, "ifdef") ? defined : !defined;
                        }
                    } else {
                        // #if <expr>
                        bool vtrue = false;
                        pp_warn_bare_assign(file, line, hrest, (size_t)((line_start + line_len) - hrest), diags);
                        if (!pp_eval_if_expr(file, line, hrest, (size_t)((line_start + line_len) - hrest), out_defs, &vtrue, diags)) {
                            vtrue = false;
                        }
                        take = vtrue;
                    }
                }
                if (ifsp >= (sizeof(ifstk)/sizeof(ifstk[0]))) {
                    diag_addf(diags, DIAG_ERROR, file, line, 1, 1, "%sif nesting too deep", is_dot ? "." : "#");
                } else {
                    bool a = parent_active && take;
                    ifstk[ifsp++] = (pp_if_t){ parent_active, a, false, a };
                    active = parent_active && take;
                }

                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

            if (sv_ieq(hkw, "elif") || sv_ieq(hkw, "elseif")) {
                if (ifsp == 0) {
                    diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                              "%selif without matching %sif", is_dot ? "." : "#", is_dot ? "." : "#");
                } else {
                    pp_if_t* t = &ifstk[ifsp - 1];
                    if (t->saw_else) {
                        diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                                  "%selif after %selse", is_dot ? "." : "#", is_dot ? "." : "#");
                    } else {
                        bool take = false;
                        if (t->parent_active && !t->taken) {
                            // elif shares the same condition rules as if
                            pp_warn_bare_assign(file, line, hrest, (size_t)((line_start + line_len) - hrest), diags);
                            if (!pp_eval_if_expr(file, line, hrest, (size_t)((line_start + line_len) - hrest), out_defs, &take, diags)) {
                                take = false;
                            }
                        }
                        t->active = t->parent_active && !t->taken && take;
                        if (t->active) t->taken = true;
                        active = t->active;
                    }
                }
                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

            if (sv_ieq(hkw, "else")) {
                if (ifsp == 0) {
                    diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                              "%selse without matching %sif", is_dot ? "." : "#", is_dot ? "." : "#");
                } else {
                    pp_if_t* t = &ifstk[ifsp - 1];
                    if (t->saw_else) {
                        diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                                  "multiple %selse for one %sif", is_dot ? "." : "#", is_dot ? "." : "#");
                    } else {
                        t->saw_else = true;
                        t->active = t->parent_active && !t->taken;
                        t->taken = t->parent_active;
                        active = t->active;
                    }
                }
                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

            if (sv_ieq(hkw, "endif") || sv_ieq(hkw, "endifdef")) {
                if (ifsp == 0) {
                    diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                              "%sendif without matching %sif", is_dot ? "." : "#", is_dot ? "." : "#");
                } else {
                    pp_if_t t = ifstk[--ifsp];
                    (void)t;
                    active = (ifsp == 0) ? true : ifstk[ifsp - 1].active;
                }
                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

            // Unknown directive:
            // For "#" prefixed directives, flag as an error (user likely meant a preprocessor directive).
            // For "." directives, let the assembler handle them.
            if (!is_dot &&
                !tok_ieq_cstr(hkw, "define") && !tok_ieq_cstr(hkw, "undef") &&
                !tok_ieq_cstr(hkw, "ifdef") && !tok_ieq_cstr(hkw, "ifndef") &&
                !tok_ieq_cstr(hkw, "if") && !tok_ieq_cstr(hkw, "elif") &&
                !tok_ieq_cstr(hkw, "else") && !tok_ieq_cstr(hkw, "endif") &&
                !tok_ieq_cstr(hkw, "include") && !tok_ieq_cstr(hkw, "includeroot") &&
                !tok_ieq_cstr(hkw, "error") && !tok_ieq_cstr(hkw, "warning")) {
                diag_addf(diags, DIAG_ERROR, file, line, 1, (unsigned)(hkw.len + 1),
                          "unknown preprocessor directive #%.*s", (int)hkw.len, hkw.p);
                pp_blank_line(&out, line_len, line_end != NULL);
                p = line_end ? line_end + 1 : p + line_len;
                line++;
                continue;
            }

        }

        if (!active) {
            // In inactive blocks, preserve layout by blanking line.
            pp_blank_line(&out, line_len, line_end != NULL);
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            continue;
        }

        // --- macro definition start? ---
        bool is_macro_start = false;
        bool is_macro_end = false;
        bool kw_colon = false; // tolerate "macro: NAME" and "endm:" forms

        // grab first token (could include leading '.')
        const char* t0 = q;
        while (*t0 == ' ' || *t0 == '\t' || *t0 == '\r') ++t0;
        const char* t1 = t0;
        while (t1 < p + line_len && is_ident_char(*t1)) ++t1;
        sv_t first = { t0, (size_t)(t1 - t0) };

        // Optional ":" immediately after keyword token (no whitespace).
        // This makes the parser a bit more forgiving for users who accidentally write
        // "macro: NAME" thinking it's a directive rather than a label.
        if (t1 < p + line_len && *t1 == ':') kw_colon = true;

        if (first.len) {
            // Begin forms: .macro / MACRO / macro
            if (sv_ieq(first, ".macro") || sv_ieq(first, "macro") || sv_ieq(first, "MACRO")) is_macro_start = true;
            // Also accept "macro:" (with no whitespace before ':')
            if (kw_colon && (sv_ieq(first, "macro") || sv_ieq(first, "MACRO"))) is_macro_start = true;

            // End forms: .endmacro / ENDM / endm / .endm
            if (sv_ieq(first, ".endmacro") || sv_ieq(first, "endm") || sv_ieq(first, "ENDM") || sv_ieq(first, ".endm")) is_macro_end = true;
            // Also accept "endm:" (with no whitespace before ':')
            if (kw_colon && (sv_ieq(first, "endm") || sv_ieq(first, "ENDM"))) is_macro_end = true;
        }

        if (is_macro_start) {
            // parse macro name + optional arg names
            sv_t mname = {0};
            unsigned name_col = 1;
            const char* after_kw = t1;
            if (kw_colon) after_kw++; // consume ':' in "macro:" form
            if (!parse_ident_sv(after_kw, line_start, &mname, &name_col)) {
                diag_addf(diags, DIAG_ERROR, file, line, name_col, 1, "missing macro name");
            } else {
                // args after name
                const char* after_name = after_kw;
                after_name = skip_ws(after_name);
                // consume name chars we already scanned: mname starts at skip_ws(t1)
                after_name = mname.p + mname.len;
                const char* rest = after_name;
                rest = skip_ws(rest);
                size_t arg_count = 0;
                char** arg_names = NULL;
                if (rest < p + line_len) {
                    arg_names = split_commas_trim(rest, (size_t)((p + line_len) - rest), &arg_count);
                } else {
                    arg_count = 0;
                    arg_names = NULL;
                }

                // collect body until end marker
                sbuf_t body = {0};
                // output blank line for the .macro line to preserve line count
                for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
                if (line_end) sbuf_putc(&out, '\n');

                // advance to next line
                p = line_end ? line_end + 1 : p + line_len;
                line++;

                while (*p) {
                    line_start = p;
                    line_end = strchr(p, '\n');
                    line_len = line_end ? (size_t)(line_end - p) : strlen(p);

                    const char* qq = skip_ws(p);
                    const char* a0 = qq;
                    const char* a1 = a0;
                    while (a1 < p + line_len && is_ident_char(*a1)) ++a1;
                    sv_t tok = { a0, (size_t)(a1 - a0) };
                    bool end_here = tok.len && (sv_ieq(tok, ".endmacro") || sv_ieq(tok, "endm") || sv_ieq(tok, "ENDM") || sv_ieq(tok, ".endm"));
                    // tolerate "endm:" (no whitespace before ':')
                    if (!end_here && tok.len && (sv_ieq(tok, "endm") || sv_ieq(tok, "ENDM")) && a1 < p + line_len && *a1 == ':') {
                        end_here = true;
                    }
                    if (end_here) {
                        // blank out end line too
                        for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
                        if (line_end) sbuf_putc(&out, '\n');
                        // store macro
                        char* name_c = xstrdup_n(mname.p, mname.len);
                        // Redefinition check (case-insensitive)
                        const macro_entry_t* prev = name_c ? macros_get(&macros, sv_from_cstr(name_c)) : NULL;
                        if (prev) {
                            diag_addf(diags, DIAG_ERROR, file, line - 1, name_col, (unsigned)mname.len,
                                      "macro '%.*s' redefined (previous at line %u)",
                                      (int)mname.len, mname.p, prev->def_line);
                        }

                        if (!name_c || !macros_put(&macros, name_c, arg_names, arg_count, body.p ? body.p : "", line - 1, name_col)) {
                            diag_addf(diags, DIAG_ERROR, file, line, 1, 1, "out of memory storing macro");
                            // free arg_names on failure
                            free_split(arg_names, arg_count);
                        }
                        free(name_c);
                        free(body.p);
                        // consume end line and continue outer loop
                        p = line_end ? line_end + 1 : p + line_len;
                        line++;
                        goto next_outer;
                    } else {
                        // append this line to macro body (with newline)
                        sbuf_puts_n(&body, p, line_len);
                        sbuf_putc(&body, '\n');
                        // also blank it in output to preserve line count
                        for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
                        if (line_end) sbuf_putc(&out, '\n');

                        p = line_end ? line_end + 1 : p + line_len;
                        line++;
                    }
                }
                // if we get here, unterminated macro
                diag_addf(diags, DIAG_ERROR, file, line - 1, 1, 1, "unterminated macro (missing endm/.endmacro)");
                free(body.p);
                if (arg_names) { for (size_t i = 0; i < arg_count; ++i) free(arg_names[i]); free(arg_names); }
                break;
            }
            // blanked .macro line already, consume it
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            goto next_outer;
        }

        if (is_macro_end) {
            diag_addf(diags, DIAG_ERROR, file, line, (unsigned)(t0 - line_start) + 1, (unsigned)first.len,
                      "unexpected macro end");
            // fall through and treat as normal line
        }

        // --- .repeat N / .endrepeat ---
        // Collect body lines and emit them N times (preproc level unrolling).
        // Syntax: .repeat <count>  ...body...  .endrepeat
        bool is_repeat_start = sv_ieq(first, ".repeat") || sv_ieq(first, "repeat");
        bool is_endrepeat    = sv_ieq(first, ".endrepeat") || sv_ieq(first, "endrepeat");

        if (is_endrepeat) {
            diag_addf(diags, DIAG_ERROR, file, line, (unsigned)(t0 - line_start) + 1, (unsigned)first.len,
                      ".endrepeat without matching .repeat");
            // blank and skip
            for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            goto next_outer;
        }

        if (is_repeat_start && active) {
            // Parse the repeat count expression (may reference #def constants)
            const char* rcount_s = t1;
            while (*rcount_s == ' ' || *rcount_s == '\t') rcount_s++;
            // strip trailing comment/whitespace for expression
            const char* rcount_e = p + line_len;
            while (rcount_e > rcount_s && (rcount_e[-1] == ' ' || rcount_e[-1] == '\t' || rcount_e[-1] == '\r')) rcount_e--;
            // strip leading ; comment
            const char* rc = rcount_s;
            while (rc < rcount_e && *rc != ';') rc++;
            rcount_e = rc;
            bool count_ok = false;
            long long rcount = 0;
            if (rcount_e > rcount_s) {
                bool vtrue = false;
                // reuse pp_eval_if_expr to get numeric value
                sbuf_t tmp_rc = {0};
                if (substitute_defs_into(&tmp_rc, rcount_s, (size_t)(rcount_e - rcount_s), out_defs)) {
                    sv_t src_rc = { tmp_rc.p ? tmp_rc.p : "", tmp_rc.len };
                    symtab_t st_rc; symtab_init(&st_rc);
                    bool unr_rc = false;
                    expr_val_t v_rc = {0};
                    if (expr_eval(file, line, 1, src_rc, &st_rc, NULL, 0, 0, false, &unr_rc, &v_rc, diags, NULL, NULL)) {
                        if (v_rc.kind == EXPR_VAL_NUM && v_rc.num >= 0 && v_rc.num <= 65536) {
                            rcount = (long long)v_rc.num;
                            count_ok = true;
                        } else {
                            diag_addf(diags, DIAG_ERROR, file, line,
                                      (unsigned)(rcount_s - line_start) + 1,
                                      (unsigned)(rcount_e - rcount_s),
                                      ".repeat count must be a non-negative integer (0..65536)");
                        }
                    }
                    expr_val_free(&v_rc);
                    symtab_free(&st_rc);
                }
                free(tmp_rc.p);
            } else {
                diag_addf(diags, DIAG_ERROR, file, line,
                          (unsigned)(t0 - line_start) + 1, (unsigned)first.len,
                          ".repeat requires a count");
            }

            // blank the .repeat line
            for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            line++;

            // Collect body lines until .endrepeat
            sbuf_t rbody = {0};
            size_t rbody_lines = 0;
            bool found_end = false;
            while (*p) {
                const char* bl_start = p;
                const char* bl_end = strchr(p, '\n');
                size_t bl_len = bl_end ? (size_t)(bl_end - p) : strlen(p);

                const char* qq2 = p;
                while (*qq2 == ' ' || *qq2 == '\t') qq2++;
                const char* a0 = qq2;
                const char* a1 = a0;
                while (a1 < p + bl_len && is_ident_char(*a1)) ++a1;
                sv_t btok = { a0, (size_t)(a1 - a0) };
                bool is_end = btok.len && (sv_ieq(btok, ".endrepeat") || sv_ieq(btok, "endrepeat"));

                if (is_end) {
                    // blank the .endrepeat line
                    for (size_t i = 0; i < bl_len; ++i) sbuf_putc(&out, ' ');
                    if (bl_end) sbuf_putc(&out, '\n');
                    p = bl_end ? bl_end + 1 : p + bl_len;
                    line++;
                    found_end = true;
                    break;
                }

                // accumulate body line
                sbuf_puts_n(&rbody, p, bl_len);
                sbuf_putc(&rbody, '\n');
                // blank it in output (will be replaced by N copies)
                for (size_t i = 0; i < bl_len; ++i) sbuf_putc(&out, ' ');
                if (bl_end) sbuf_putc(&out, '\n');
                rbody_lines++;
                p = bl_end ? bl_end + 1 : p + bl_len;
                line++;
            }

            if (!found_end) {
                diag_addf(diags, DIAG_ERROR, file, line - 1, 1, 1,
                          "unterminated .repeat (missing .endrepeat)");
            } else if (count_ok && rcount > 0 && rbody.p) {
                // Emit body N times, each time substituting defs and expanding macros
                for (long long ri = 0; ri < rcount; ri++) {
                    const char* bp2 = rbody.p;
                    while (*bp2) {
                        const char* be2 = strchr(bp2, '\n');
                        size_t bl2 = be2 ? (size_t)(be2 - bp2) : strlen(bp2);
                        process_line_expand(&out, file, line - rbody_lines - 1,
                                            bp2, bl2, out_defs, &macros, diags, 0, false);
                        if (!be2) break;
                        bp2 = be2 + 1;
                    }
                }
            } else if (count_ok && rcount == 0) {
                // zero repeats: body was already blanked, nothing to emit
                // but we still need blank lines to preserve count
                // (they were already blanked during collection above)
            }
            free(rbody.p);
            goto next_outer;
        }

        if (is_repeat_start && !active) {
            // inside false conditional: collect and discard body
            for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            while (*p) {
                const char* bl_start2 = p;
                const char* bl_end2 = strchr(p, '\n');
                size_t bl_len2 = bl_end2 ? (size_t)(bl_end2 - p) : strlen(p);
                const char* qq3 = p;
                while (*qq3 == ' ' || *qq3 == '\t') qq3++;
                const char* a02 = qq3; const char* a12 = a02;
                while (a12 < p + bl_len2 && is_ident_char(*a12)) ++a12;
                sv_t btok2 = { a02, (size_t)(a12 - a02) };
                bool is_end2 = btok2.len && (sv_ieq(btok2, ".endrepeat") || sv_ieq(btok2, "endrepeat"));
                for (size_t i = 0; i < bl_len2; ++i) sbuf_putc(&out, ' ');
                if (bl_end2) sbuf_putc(&out, '\n');
                p = bl_end2 ? bl_end2 + 1 : p + bl_len2;
                line++;
                if (is_end2) break;
            }
            goto next_outer;
        }

        // detect #undef or .undef at start (after whitespace)
        if ((q[0] == '#' || q[0] == '.') &&
            (q[1] == 'u' || q[1] == 'U') && (q[2] == 'n' || q[2] == 'N') &&
            (q[3] == 'd' || q[3] == 'D') && (q[4] == 'e' || q[4] == 'E') &&
            (q[5] == 'f' || q[5] == 'F')) {
            const char* r = skip_ws(q + 6);
            const char* name_b = r;
            if (!is_ident_start(*r) || *r == '.') {
                diag_addf(diags, DIAG_ERROR, file, line, (unsigned)(r - line_start) + 1, 1,
                          "invalid %sundef name", (q[0] == '.') ? "." : "#");
            } else {
                ++r;
                while (is_ident_char(*r)) ++r;
                sv_t name = { name_b, (size_t)(r - name_b) };
                defs_del(out_defs, name);
            }
            // blank the directive line in output to preserve line count
            for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
            if (line_end) sbuf_putc(&out, '\n');
            p = line_end ? line_end + 1 : p + line_len;
            line++;
            goto next_outer;
        }

        // detect #def/.def or #define/.define at start (after whitespace)
        // IMPORTANT: "#define" starts with "#def". Ensure we don't mis-detect
        // "#define" as "#def" (which would parse the name as "ine").
        bool is_def_dir = false;
        const char* def_r = NULL;

        // Prefer the longer keyword first.
        if ((q[0] == '#' || q[0] == '.') &&
            (q[1] == 'd' || q[1] == 'D') && (q[2] == 'e' || q[2] == 'E') &&
            (q[3] == 'f' || q[3] == 'F') && (q[4] == 'i' || q[4] == 'I') &&
            (q[5] == 'n' || q[5] == 'N') && (q[6] == 'e' || q[6] == 'E')) {
            is_def_dir = (q[0] == '.');
            def_r = q + 7;
        } else if ((q[0] == '#' || q[0] == '.') &&
                   (q[1] == 'd' || q[1] == 'D') && (q[2] == 'e' || q[2] == 'E') && (q[3] == 'f' || q[3] == 'F')) {
            // Only match "#def"/".def" as a whole token.
            const char* after = q + 4;
            if (after >= (line_start + line_len) || pp_is_ws(*after) || *after == '\r') {
                is_def_dir = (q[0] == '.');
                def_r = after;
            }
        }
        if (def_r) {
            const char* r = skip_ws(def_r);
            // parse name
            const char* name_b = r;
            if (!is_ident_start(*r) || *r == '.') {
                diag_addf(diags, DIAG_ERROR, file, line, (unsigned)(r - line_start) + 1, 1,
                          "invalid %sdef name", is_def_dir ? "." : "#");
            } else {
                ++r;
                while (is_ident_char(*r)) ++r;
                sv_t name = { name_b, (size_t)(r - name_b) };
                r = skip_ws(r);
                sv_t val = { r, (size_t)((line_start + line_len) - r) };
                while (val.len && (val.p[val.len-1] == ' ' || val.p[val.len-1] == '\t' || val.p[val.len-1] == '\r')) val.len--;
                // 'cpu' is a reserved/predefined symbol set by the .cpu directive.
                if (sv_ieq(name, "cpu")) {
                    diag_addf(diags, DIAG_ERROR, file, line,
                              (unsigned)(name.p - line_start) + 1,
                              (unsigned)(name.len ? name.len : 1),
                              "Cannot redefine reserved symbol cpu\nUse .cpu directive instead");
                    for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
                    if (line_end) sbuf_putc(&out, '\n');
                    p = line_end ? line_end + 1 : p + line_len;
                    line++;
                    goto next_outer;
                }

                const char* existing = defs_get(out_defs, name);
    if (existing) {
        // Duplicate .def/#def: keep the first definition, flag the new one, and blank the line
        diag_addf(diags, DIAG_ERROR, file, line,
                  (unsigned)(name.p - line_start) + 1,
                  (unsigned)(name.len ? name.len : 1),
                  "duplicate %sdef '%.*s'", is_def_dir ? "." : "#", (int)name.len, name.p);
        for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
        if (line_end) sbuf_putc(&out, '\n');
    } else {
        char* k = xstrdup_n(name.p, name.len);
        char* v = (val.len == 0) ? xstrdup_n("1", 1) : xstrdup_n(val.p, val.len);
        if (!k || !v || !defs_set(out_defs, k, v)) {
            diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                      "out of memory storing %sdef", is_def_dir ? "." : "#");
        }
        free(k); free(v);

        // Emit an assembler-visible constant definition.
        //
        // IMPORTANT: The assembler core does NOT consume ".def" as an
        // assembler directive. "#define" / ".def" are preprocessor features.
        //
        // The assembler *does* accept constant definitions using:
        //
        //   NAME = expr
        //   NAME EQU expr
        //
        // We use the '=' form because it is compact and already handled by
        // the parser.
        //
        // We preserve *line length* by padding with spaces.
        size_t synth_len = line_len ? line_len : 1;
        char* linebuf = (char*)malloc(synth_len);
        if (!linebuf) {
            diag_addf(diags, DIAG_ERROR, file, line, 1, 1,
                      "out of memory expanding %sdef", is_def_dir ? "." : "#");
            for (size_t i = 0; i < line_len; ++i) sbuf_putc(&out, ' ');
            if (line_end) sbuf_putc(&out, '\n');
        } else {
            memset(linebuf, ' ', synth_len);
            size_t pos = 0;

            // NAME
            for (size_t i = 0; i < name.len && pos < synth_len; ++i)
                linebuf[pos++] = name.p[i];

            // " = "
            if (pos < synth_len) linebuf[pos++] = ' ';
            if (pos < synth_len) linebuf[pos++] = '=';
            if (pos < synth_len) linebuf[pos++] = ' ';

            // VALUE (if empty, treat as "1")
            if (val.len == 0) {
                if (pos < synth_len) linebuf[pos++] = '1';
            } else {
                for (size_t i = 0; i < val.len && pos < synth_len; ++i)
                    linebuf[pos++] = val.p[i];
            }

            for (size_t i = 0; i < synth_len; ++i) sbuf_putc(&out, linebuf[i]);
            if (line_end) sbuf_putc(&out, '\n');
            free(linebuf);
        }
    }
}
        } else {
            // normal line or macro call (possibly with leading label), with recursive macro expansion.
            size_t out_len_before = out.len;
            process_line_expand(&out, file, line, p, line_len, out_defs, &macros, diags, 0, false);
            if (line_end) {
                // process_line_expand always emits at least one '\n' for normal lines; for safety,
                // ensure we end this input line with a newline if it had one and expansion didn't.
                if (out.len == 0 || out.p[out.len - 1] != '\n') sbuf_putc(&out, '\n');
            }
            /* Count newlines emitted. If more than 1 was emitted (macro
               expanded to multiple lines), emit a line-number reset marker
               so the assembler stays in sync with the source line numbers. */
            {
                size_t nl_count = 0;
                for (size_t _i = out_len_before; _i < out.len; _i++)
                    if (out.p[_i] == '\n') nl_count++;
                if (nl_count > 1)
                    pp_emit_line_mark(&out, line + 1);
            }
        }

        p = line_end ? line_end + 1 : p + line_len;
        line++;

    next_outer:
        ;
    }

    macros_free(&macros);
    // Macro-expanded lines may themselves contain preprocessor directives
    // (.if/.else/.endif and #if variants). Because macro expansion emits text
    // directly into the output stream, those directives must be re-processed.
    //
    // We detect macro-expanded lines via the internal 0x1E marker. If present,
    // scrub markers to spaces (preserving column alignment) and run a second
    // preprocessor pass over the already-flattened stream with includes disabled.
    char* ret = out.p ? out.p : xstrdup_n("", 0);

    if (ret) {
        bool has_macro_marks = false;
        for (char* s = ret; *s; ++s) {
            if (*s == '\x1e') { has_macro_marks = true; break; }
        }

        if (has_macro_marks) {
            for (char* s = ret; *s; ++s) {
                if (*s == '\x1e') *s = ' ';
                /* Leave PP_LINE_MARK (\x1f) intact — pass2 passes them through
                   unchanged so asm.c can consume them for line tracking. */
            }

            // Includes are not re-expanded in pass 2. This is critical for the
            // GTK editor mode (which typically runs with expand_includes=false)
            // and to avoid re-entering include processing for already-flattened
            // sources.
            char* pass2 = preproc_run_ex(file, ret, diags, out_defs,
                                         io_hex_prefix, hex_prefix_cli, bin_prefix,
                                         false, include_paths, include_path_count);
            free(ret);
            ret = pass2;
        }
    }

    free(inc.p);
    return ret;
}

// Expand a single source line, handling:
// - optional leading label definition in front of a macro call ("label: MACRO ..." or ".label MACRO ...")
// - macro argument substitution (\\1..\\9 and \\name)
// - recursive macro calls, with a depth limit
// - $name substitution (skipping string literals)

/* ---------------------------------------------------------------------------
 * colon_split_next: find the next statement separator ':' in a line.
 *
 * Rules:
 *   - ':' at position 0 or immediately after an identifier (label def) is NOT
 *     a separator — e.g. "label:" or ".label:" are labels.
 *   - ':' inside a string literal (single or double quoted) is NOT a separator.
 *   - ':' after a comment character (';') is NOT a separator.
 *   - All other ':' are statement separators.
 *
 * Returns pointer to the ':' separator, or NULL if none found.
 * out_label_end: if the line starts with a label "foo:", points past the ':'
 * so the caller can handle "label: LDA #1 : STA $10" correctly.
 * --------------------------------------------------------------------------- */

static bool process_line_expand(sbuf_t* out, const char* file, unsigned line_no,
                                const char* line, size_t line_len,
                                const defs_t* defs, const macros_t* macros,
                                diags_t* diags,
                                unsigned depth,
                                bool from_macro) {
    if (depth > 32) {
        diag_addf(diags, DIAG_ERROR, file, line_no, 1, 1, "macro expansion too deep (possible recursion)");
        // still emit a blanked line to keep output moving
        for (size_t i = 0; i < line_len; ++i) sbuf_putc(out, ' ');
        sbuf_putc(out, '\n');
        return false;
    }

    sv_t macro_tok = {0};
    size_t prefix_len = 0;
    if (!scan_macro_invocation(line, line_len, &macro_tok, &prefix_len)) {
        // empty/whitespace line
        if (from_macro) sbuf_putc(out, '\x1e');
        sbuf_puts_n(out, line, line_len);
        sbuf_putc(out, '\n');
        return true;
    }

    const macro_entry_t* me = NULL;
    if (macro_tok.len && is_ident_start(macro_tok.p[0]) && macro_tok.p[0] != '.') {
        me = macros_get(macros, macro_tok);
    }

    if (!me) {
        // Normal line: only $ substitution
        sbuf_t tmp = {0};
        if (!substitute_defs_into(&tmp, line, line_len, defs)) {
            free(tmp.p);
            return false;
        }
        if (from_macro) sbuf_putc(out, '\x1e');
        sbuf_puts_n(out, tmp.p ? tmp.p : "", tmp.len);
        sbuf_putc(out, '\n');
        free(tmp.p);
        return true;
    }

    // Macro invocation. Extract the original prefix (label + whitespace) if present.
    const char* prefix = line;
    size_t prefix_n = prefix_len;

    // Parse actual args after macro name.
    const char* rest = macro_tok.p + macro_tok.len;
    // allow whitespace between name and args
    rest = skip_ws(rest);
    size_t val_count = 0;
    char** vals = NULL;
    if (rest < line + line_len) {
        vals = split_commas_trim(rest, (size_t)((line + line_len) - rest), &val_count);
    }

    if (me->arg_count != 0 && val_count != me->arg_count) {
        unsigned col = (unsigned)(macro_tok.p - line) + 1;
        diag_addf(diags, DIAG_ERROR, file, line_no, col, (unsigned)macro_tok.len,
                  "macro '%.*s' expects %zu argument(s) but got %zu",
                  (int)macro_tok.len, macro_tok.p, me->arg_count, val_count);
    }

    // Ensure arg values array has at least arg_count entries; missing values become empty.
    if (me->arg_count > val_count) {
        char** nv = (char**)realloc(vals, me->arg_count * sizeof(char*));
        if (!nv) {
            free_split(vals, val_count);
            return false;
        }
        for (size_t i = val_count; i < me->arg_count; ++i) nv[i] = xstrdup_n("", 0);
        vals = nv;
        val_count = me->arg_count;
    }

    // Expand body line-by-line.
    bool first_line = true;
    const char* bp = me->body;
    while (*bp) {
        const char* be = strchr(bp, '\n');
        size_t bl = be ? (size_t)(be - bp) : strlen(bp);

        sbuf_t t1 = {0};
        substitute_macro_line(&t1, bp, bl, me->arg_names, me->arg_count, vals, val_count);

        sbuf_t t2 = {0};
        substitute_defs_into(&t2, t1.p ? t1.p : "", t1.len, defs);

        // Recursively expand any macro calls in the expanded line.
        // For the first expanded line, preserve the original label prefix.
        if (first_line && prefix_n) {
            sbuf_t joined = {0};
            sbuf_puts_n(&joined, prefix, prefix_n);
            sbuf_puts_n(&joined, t2.p ? t2.p : "", t2.len);
            process_line_expand(out, file, line_no, joined.p ? joined.p : "", joined.len, defs, macros, diags, depth + 1, true);
            free(joined.p);
        } else {
            process_line_expand(out, file, line_no, t2.p ? t2.p : "", t2.len, defs, macros, diags, depth + 1, true);
        }

        first_line = false;
        free(t1.p);
        free(t2.p);
        if (!be) break;
        bp = be + 1;
    }

    free_split(vals, val_count);
    return true;
}
