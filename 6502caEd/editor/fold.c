/* fold.c — fold map implementation */
#include "fold.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* ── Line classification helpers ──────────────────────────────────────── */

/* Return pointer past leading whitespace (space and tab only). */
static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Return true if the trimmed line is a pure comment (starts with ; or //). */
static bool is_line_comment(const char *raw) {
  const char *p = skip_ws(raw);
  if (p[0] == ';') return true;
  if (p[0] == '/' && p[1] == '/') return true;
  return false;
}

/* Return true if the line begins a macro definition.
 * Matches both ".macro NAME" and bare "macro NAME" forms, and also handles
 * the "label .macro NAME" pattern by scanning forward on the line. */
static bool is_macro_start(const char *raw) {
  const char *p = skip_ws(raw);
  /* optional label before .macro */
  /* look for .macro or bare MACRO keyword */
  if (*p == '.') p++;
  char tmp[8] = {0};
  int i = 0;
  while (i < 7 && (isalpha((unsigned char)p[i]) || p[i]=='_'))
    { tmp[i] = (char)tolower((unsigned char)p[i]); i++; }
  tmp[i] = 0;
  if (strcmp(tmp, "macro") == 0) return true;
  /* also allow  label  .macro  name  pattern */
  /* scan for .macro anywhere on leading token area */
  const char *q = raw;
  while (*q) {
    if (*q == ';' || (*q=='/' && *(q+1)=='/')) break;
    if (*q == '.' || (q == raw && isalpha((unsigned char)*q))) {
      const char *t = (*q=='.') ? q+1 : q;
      char w[8]={0}; int j=0;
      while (j<7 && isalpha((unsigned char)t[j]))
        { w[j]=(char)tolower((unsigned char)t[j]); j++; }
      if (strcmp(w,"macro")==0) return true;
    }
    q++;
  }
  return false;
}

/* Return true if the trimmed line is an end-macro directive
 * (.endmacro, .endm, endmacro, or endm). */
static bool is_macro_end(const char *raw) {
  const char *p = skip_ws(raw);
  if (*p == '.') p++;
  char tmp[12] = {0};
  int i = 0;
  while (i < 11 && isalpha((unsigned char)p[i]))
    { tmp[i] = (char)tolower((unsigned char)p[i]); i++; }
  return strcmp(tmp,"endmacro")==0 || strcmp(tmp,"endm")==0;
}

/* ── Rebuild ───────────────────────────────────────────────────────────── */

/* Rebuild the fold map from scratch by scanning every line.
 * The previous collapsed state for each line is preserved by index so
 * that toggling a fold survives buffer edits that don't move lines.
 *
 * Comment groups: 2 or more consecutive comment-only lines.
 * Macro groups:   from a .macro line (FL_FOLD_START) through to the
 *                 matching .endmacro line (FL_FOLD_END), with all body
 *                 lines marked FL_FOLD_BODY.
 *
 * in:  buf_lines    — array of NUL-terminated line pointers (length line_count)
 *      line_count   — number of lines in the buffer
 *      fold_comments — whether to detect comment groups
 *      fold_macros   — whether to detect macro groups */
/* Rebuild the fold map by scanning every buffer line.
 * Comment groups (2+ consecutive comment lines) and macro blocks
 * (.macro ... .endmacro) are detected and marked.
 * The previous collapsed state is preserved by line index.
 * in:  buf_lines     — array of source line pointers
 *      line_count    — number of lines
 *      fold_comments — detect comment groups
 *      fold_macros   — detect macro blocks */
void fold_map_rebuild(fold_map_t *fm,
                      const char **buf_lines, size_t line_count,
                      bool fold_comments, bool fold_macros) {
  /* Resize the lines array if the line count has changed. */
  if (fm->count != line_count) {
    free(fm->lines);
    fm->lines = line_count
        ? (fold_line_t *)calloc(line_count, sizeof(fold_line_t))
        : NULL;
    fm->count = line_count;
  }

  if (!fm->lines || !line_count) return;

  /* Save previous collapsed states so that edits don't silently expand folds. */
  bool *was_collapsed = (bool*)calloc(line_count, sizeof(bool));
  for (size_t i = 0; i < line_count; i++)
    was_collapsed[i] = fm->lines[i].collapsed;

  memset(fm->lines, 0, line_count * sizeof(fold_line_t));

  size_t i = 0;

  while (i < line_count) {
    const char *raw = buf_lines[i];

    /* ── Consecutive line-comment group ── */
    if (fold_comments && is_line_comment(raw)) {
      size_t end = i;
      while (end + 1 < line_count && is_line_comment(buf_lines[end+1]))
        end++;
      size_t span = end - i + 1;
      if (span >= 2) {
        /* Mark start, body, and end lines. */
        fm->lines[i].kind       = FL_FOLD_START;
        fm->lines[i].group_end  = end;
        fm->lines[i].group_size = (int)span;
        fm->lines[i].is_macro   = false;
        fm->lines[i].collapsed  = was_collapsed[i];
        for (size_t j = i+1; j < end; j++) {
          fm->lines[j].kind     = FL_FOLD_BODY;
          fm->lines[j].is_macro = false;
        }
        fm->lines[end].kind     = FL_FOLD_END;
        fm->lines[end].is_macro = false;
        i = end + 1;
        continue;
      }
    }

    /* ── Macro block ── */
    if (fold_macros && is_macro_start(raw)) {
      /* Scan forward for the matching .endmacro. */
      size_t end = i + 1;
      while (end < line_count && !is_macro_end(buf_lines[end]))
        end++;
      /* end points to .endmacro, or line_count if the block is unclosed. */
      if (end < line_count && end > i) {
        fm->lines[i].kind       = FL_FOLD_START;
        fm->lines[i].group_end  = end;
        fm->lines[i].group_size = (int)(end - i + 1);
        fm->lines[i].is_macro   = true;
        fm->lines[i].collapsed  = was_collapsed[i];
        for (size_t j = i+1; j < end; j++) {
          fm->lines[j].kind     = FL_FOLD_BODY;
          fm->lines[j].is_macro = true;
        }
        fm->lines[end].kind     = FL_FOLD_END;
        fm->lines[end].is_macro = true;
        i = end + 1;
        continue;
      }
    }

    i++;
  }

  free(was_collapsed);
}

/* Free the lines array and reset the count. */
void fold_map_free(fold_map_t *fm) {
  free(fm->lines);
  fm->lines = NULL;
  fm->count = 0;
}

/* Toggle the collapsed state of the fold group starting at line idx.
 * ret: true if idx is a FL_FOLD_START, false otherwise */
bool fold_toggle(fold_map_t *fm, size_t idx) {
  if (!fm->lines || idx >= fm->count) return false;
  if (fm->lines[idx].kind != FL_FOLD_START) return false;
  fm->lines[idx].collapsed = !fm->lines[idx].collapsed;
  return true;
}

/* ── Visual ↔ buffer index translation ───────────────────────────────── */

/* Helper: return true if buffer line i is hidden (inside a collapsed fold). */
static bool line_is_hidden(const fold_map_t *fm, size_t i) {
  if (fm->lines[i].kind != FL_FOLD_BODY && fm->lines[i].kind != FL_FOLD_END)
    return false;
  /* Walk back to find the owning FL_FOLD_START. */
  size_t s = i;
  while (s > 0 && fm->lines[s].kind != FL_FOLD_START) s--;
  return fm->lines[s].collapsed;
}

/* Translate a 0-based visual row (counting only visible lines) to the
 * corresponding buffer line index.
 * Returns fm->count if visual_row is beyond the last visible line. */
size_t fold_visual_to_buf(const fold_map_t *fm, size_t visual_row) {
  if (!fm->lines) return visual_row;
  size_t vis = 0;
  for (size_t i = 0; i < fm->count; i++) {
    if (line_is_hidden(fm, i)) continue;
    if (vis == visual_row) return i;
    vis++;
  }
  return fm->count;
}

/* Translate a buffer line index to its 0-based visual row.
 * Returns SIZE_MAX if the line is hidden inside a collapsed fold. */
size_t fold_buf_to_visual(const fold_map_t *fm, size_t buf_line) {
  if (!fm->lines) return buf_line;
  size_t vis = 0;
  for (size_t i = 0; i < fm->count; i++) {
    bool hidden = line_is_hidden(fm, i);
    if (i == buf_line) return hidden ? SIZE_MAX : vis;
    if (!hidden) vis++;
  }
  return SIZE_MAX;
}

/* Count the number of visible (non-hidden) lines. */
size_t fold_visible_count(const fold_map_t *fm) {
  if (!fm->lines) return fm->count;
  size_t vis = 0;
  for (size_t i = 0; i < fm->count; i++) {
    if (!line_is_hidden(fm, i)) vis++;
  }
  return vis;
}
