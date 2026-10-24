#include "symdb.h"
#include "../core/util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Return true if ch may begin an identifier (letter or underscore). */
static int is_ident0(int ch) { return isalpha((unsigned char)ch) || ch == '_'; }
/* Return true if ch may continue an identifier (letter, digit, or underscore). */
static int is_ident(int ch) { return isalnum((unsigned char)ch) || ch == '_'; }

/* Return true if the token s[0..n) is a recognised dot-directive keyword.
 * Used to distinguish dot-labels (.loop) from directives (.if, .org, etc.). */
static int is_dot_directive_tok(const char* s, size_t n) {
  // s includes leading '.'
  static const char* dirs[] = {
      ".ORG", ".BYTE", ".WORD", ".DWORD",
      ".IF", ".IFDEF", ".IFNDEF", ".ELSE", ".ELIF", ".ELSEIF", ".ENDIF",
      ".ASSERT", ".WARNING", ".ERROR",
      ".DEF", ".DEFINE",
      ".INCLUDE",
      ".PROC", ".ENDPROC",
      ".MACRO", ".ENDMACRO", ".ENDM",
  };
  for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
    if (sv_ieq((sv_t){s, n}, dirs[i])) return 1;
  }
  return 0;
}

/* Return true if the dot-directive defines a new symbol name immediately after it
 * (.def, .define, .proc entry point, or .macro name). */
static int is_dot_defining_directive_tok(const char* s, size_t n) {
  // .DEF/.DEFINE define a symbol; .PROC defines a symbol (the proc entry point)
  return sv_ieq((sv_t){s, n}, ".def") || sv_ieq((sv_t){s, n}, ".define") ||
         sv_ieq((sv_t){s, n}, ".proc") || sv_ieq((sv_t){s, n}, ".macro");
}


/* Copy n bytes from src to dst, converting each to uppercase. */
static void to_upper_n(char* dst, const char* src, size_t n) {
  for (size_t i = 0; i < n; i++) dst[i] = (char)toupper((unsigned char)src[i]);
}

/* Allocate an uppercase heap copy of s[0..n), stripping a leading '.' and
 * trailing ':' (normalises label and directive tokens to a common form).
 * ret: heap string, or NULL if n becomes 0 after stripping or on OOM */
static char* dup_upper_norm(const char* s, size_t n) {
  // normalize: strip leading '.' and trailing ':'
  if (n && s[0] == '.') { s++; n--; }
  if (n && s[n - 1] == ':') n--;
  if (!n) return NULL;
  char* out = (char*)malloc(n + 1);
  if (!out) return NULL;
  to_upper_n(out, s, n);
  out[n] = 0;
  return out;
}

/* Initialise an empty symbol database. */
void symdb_init(symdb_t* db) {
  memset(db, 0, sizeof(*db));
}

/* Free all symbol names and per-line arrays, then zero the struct. */
void symdb_free(symdb_t* db) {
  if (!db) return;
  for (size_t i = 0; i < db->def_count; i++) free(db->defs[i].name);
  free(db->defs);
  free(db->def_start);
  free(db->def_end);
  free(db->def_is_dup);
  memset(db, 0, sizeof(*db));
}

/* Grow the per-line arrays to cover at least `lines` entries.
 * New slots are initialised to 0/false. */
static bool ensure_line_cap(symdb_t* db, size_t lines) {
  if (db->line_cap >= lines) return true;
  size_t newcap = db->line_cap ? db->line_cap : 64;
  while (newcap < lines) newcap *= 2;
  size_t* ns = (size_t*)realloc(db->def_start, newcap * sizeof(size_t));
  size_t* ne = (size_t*)realloc(db->def_end, newcap * sizeof(size_t));
  bool* nd = (bool*)realloc(db->def_is_dup, newcap * sizeof(bool));
  if (!ns || !ne || !nd) {
    free(ns); free(ne); free(nd);
    return false;
  }
  // init new slots
  for (size_t i = db->line_cap; i < newcap; i++) {
    ns[i] = 0; ne[i] = 0; nd[i] = false;
  }
  db->def_start = ns;
  db->def_end = ne;
  db->def_is_dup = nd;
  db->line_cap = newcap;
  return true;
}

/* Append a symbol definition to the defs array, growing it if needed.
 * Takes ownership of name (heap string). */
static bool defs_push(symdb_t* db, char* name, size_t line1, int col1, bool is_macro) {
  if (db->def_count == db->def_cap) {
    size_t nc = db->def_cap ? db->def_cap * 2 : 128;
    symdef_t* nd = (symdef_t*)realloc(db->defs, nc * sizeof(symdef_t));
    if (!nd) return false;
    db->defs = nd;
    db->def_cap = nc;
  }
  db->defs[db->def_count++] = (symdef_t){ name, line1, col1, is_macro };
  return true;
}

/* Return true if a symbol named up (uppercase) is already in the database. */
static bool defs_find(const symdb_t* db, const char* up) {
  for (size_t i = 0; i < db->def_count; i++) {
    if (!strcmp(db->defs[i].name, up)) return true;
  }
  return false;
}

/* Return true if s[0..n) is the keyword EQU (case-insensitive, exactly 3 chars). */
static bool is_equ_tok(const char* s, size_t n) {
  if (n != 3) return false;
  return (toupper((unsigned char)s[0]) == 'E' &&
          toupper((unsigned char)s[1]) == 'Q' &&
          toupper((unsigned char)s[2]) == 'U');
}

/* Rebuild the symbol database from the buffer contents.
 * Scans every line for label definitions, EQU constants, .def directives,
 * .proc and .macro names, and @-local labels.  Per-line start/end spans
 * and duplicate flags are also updated for use by the syntax highlighter.
 * in:  buf — buffer to scan */
void symdb_rebuild(symdb_t* db, const buffer_t* buf) {
  if (!db || !buf) return;

  // clear defs
  for (size_t i = 0; i < db->def_count; i++) free(db->defs[i].name);
  db->def_count = 0;

  if (!ensure_line_cap(db, buf->count ? buf->count : 1)) return;
  for (size_t i = 0; i < buf->count; i++) {
    db->def_start[i] = db->def_end[i] = 0;
    db->def_is_dup[i] = false;
  }

  for (size_t li = 0; li < buf->count; li++) {
    const char* line = buffer_get_line(buf, li);
    size_t len = strlen(line);
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len) continue;
    if (line[i] == ';') continue;

    // Dot-start: could be a directive (e.g. .if/.include/.def) or a dot-label definition (.label)
if (line[i] == '.') {
  size_t j = i + 1;
  if (j < len && is_ident0(line[j])) {
    j++;
    while (j < len && is_ident(line[j])) j++;
    // token includes leading dot
    size_t tok_n = j - i;

    // If this is a known dot-directive, it is NOT a label definition.
    if (is_dot_directive_tok(line + i, tok_n)) {
      // But some dot-directives define a symbol name after them (e.g. .def NAME value).
      if (is_dot_defining_directive_tok(line + i, tok_n)) {
        bool def_is_macro = sv_ieq((sv_t){line + i, tok_n}, ".MACRO");
        size_t k = j;
        while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
        if (k < len && is_ident0(line[k])) {
          size_t n0 = k;
          size_t n1 = k + 1;
          while (n1 < len && is_ident(line[n1])) n1++;
          char* up = dup_upper_norm(line + n0, n1 - n0);
          if (up) {
            bool dup = defs_find(db, up);
            db->def_start[li] = n0;
            db->def_end[li] = n1;
            db->def_is_dup[li] = dup;
            if (!dup) defs_push(db, up, li + 1, (int)n0 + 1, def_is_macro);
            else free(up);
          }
        }
      }
      continue;
    }

    // Otherwise treat as dot-label definition: .label
    char* up = dup_upper_norm(line + i, tok_n);
    if (up) {
      bool dup = defs_find(db, up);
      db->def_start[li] = i;
      db->def_end[li] = j;
      db->def_is_dup[li] = dup;
      if (!dup) defs_push(db, up, li + 1, (int)i + 1, false);
      else free(up);
    }
  }
  continue;
}


    // @ local label: @ident or @ident:
    if (line[i] == '@' && (i + 1) < len && is_ident0(line[i + 1])) {
      size_t j = i + 1;
      while (j < len && is_ident(line[j])) j++;
      size_t end = (j < len && line[j] == ':') ? j + 1 : j;
      // Store with the @ prefix so it appears in the symbol list
      char* up = dup_upper_norm(line + i, end - i);
      if (up) {
        bool dup = defs_find(db, up);
        db->def_start[li] = i;
        db->def_end[li] = (j < len && line[j] == ':') ? j + 1 : j;
        db->def_is_dup[li] = dup;
        if (!dup) defs_push(db, up, li + 1, (int)i + 1, false);
        else free(up);
      }
      continue;
    }

    // Potential: label:  or  NAME EQU/=
    if (!is_ident0(line[i])) continue;
    size_t j = i + 1;
    while (j < len && is_ident(line[j])) j++;

    // label:
    if (j < len && line[j] == ':') {
      size_t end = j + 1;
      char* up = dup_upper_norm(line + i, end - i);
      if (up) {
        bool dup = defs_find(db, up);
        db->def_start[li] = i;
        db->def_end[li] = end;
        db->def_is_dup[li] = dup;
        if (!dup) defs_push(db, up, li + 1, (int)i + 1, false);
        else free(up);
      }
      continue;
    }

    // NAME EQU ...  or  NAME = ...
    size_t k = j;
    while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
    if (k >= len) continue;
    bool is_def = false;
    size_t op_end = k;
    if (line[k] == '=') {
      is_def = true;
      op_end = k + 1;
    } else if (is_ident0(line[k])) {
      size_t t0 = k;
      size_t t1 = k + 1;
      while (t1 < len && is_ident(line[t1])) t1++;
      if (is_equ_tok(line + t0, t1 - t0)) {
        is_def = true;
        op_end = t1;
      }
    }
    if (is_def) {
      char* up = dup_upper_norm(line + i, j - i);
      if (up) {
        bool dup = defs_find(db, up);
        db->def_start[li] = i;
        db->def_end[li] = j; // highlight just the name
        db->def_is_dup[li] = dup;
        if (!dup) defs_push(db, up, li + 1, (int)i + 1, false);
        else free(up);
      }
      continue;
    }

    // bare "macro NAME" / "MACRO NAME" (no leading dot)
    {
      size_t kw_n = j - i;
      int is_macro_kw = (kw_n == 5 &&
                         sv_ieq((sv_t){line + i, kw_n}, "MACRO"));
      if (is_macro_kw) {
        size_t k2 = j;
        while (k2 < len && (line[k2] == ' ' || line[k2] == '	')) k2++;
        if (k2 < len && is_ident0(line[k2])) {
          size_t n0 = k2;
          size_t n1 = k2 + 1;
          while (n1 < len && is_ident(line[n1])) n1++;
          char* up = dup_upper_norm(line + n0, n1 - n0);
          if (up) {
            bool dup = defs_find(db, up);
            db->def_start[li] = n0;
            db->def_end[li] = n1;
            db->def_is_dup[li] = dup;
            if (!dup) defs_push(db, up, li + 1, (int)n0 + 1, true);
            else free(up);
          }
        }
      }
    }
  }
}

/* Return true if s[0..n) names a symbol defined via .macro or bare MACRO.
 * The lookup is case-insensitive; leading '.' and trailing ':' are stripped. */
bool symdb_is_macro(const symdb_t* db, const char* s, size_t n) {
  if (!db || !s || n == 0) return false;
  if (s[0] == '.') { s++; n--; }
  if (n && s[n - 1] == ':') n--;
  if (!n) return false;
  char up[64];
  if (n >= sizeof(up)) return false;
  for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)s[i]);
  up[n] = 0;
  for (size_t i = 0; i < db->def_count; i++) {
    if (db->defs[i].is_macro && !strcmp(db->defs[i].name, up)) return true;
  }
  return false;
}

/* Return true if s[0..n) names any defined symbol (label, constant, macro,
 * or .proc entry point).  Case-insensitive; leading '.' and ':' stripped. */
bool symdb_is_defined(const symdb_t* db, const char* s, size_t n) {
  if (!db || !s || n == 0) return false;
  // normalize: strip leading '.' and trailing ':'
  if (s[0] == '.') { s++; n--; }
  if (n && s[n - 1] == ':') n--;
  if (!n) return false;
  char tmp[128];
  if (n >= sizeof(tmp)) return false; // too long; ignore
  to_upper_n(tmp, s, n);
  tmp[n] = 0;
  return defs_find(db, tmp);
}

/* If line_idx0 (0-based) contains a symbol definition, fill the byte-offset
 * span [*out_start, *out_end) and set *out_dup if it is a duplicate.
 * ret: true if the line has a definition, false otherwise */
bool symdb_line_def(const symdb_t* db, size_t line_idx0, size_t* out_start, size_t* out_end, bool* out_dup) {
  if (!db) return false;
  if (line_idx0 >= db->line_cap) return false;
  size_t s = db->def_start[line_idx0];
  size_t e = db->def_end[line_idx0];
  if (s == 0 && e == 0) return false;
  if (out_start) *out_start = s;
  if (out_end) *out_end = e;
  if (out_dup) *out_dup = db->def_is_dup[line_idx0];
  return true;
}
