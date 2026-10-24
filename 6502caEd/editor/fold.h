#pragma once
/* fold.h — Code folding for comment blocks and macro bodies.
 *
 * A fold_map_t is a flat array parallel to buffer->lines[], rebuilt
 * whenever the buffer changes or fold prefs change.
 *
 * Comment fold: 2+ consecutive lines whose trimmed content starts with
 *   ';', '//' or is part of a '/' '*' ... '*' '/' block.
 * Macro fold:   lines between .macro and .endmacro/.endm (exclusive of
 *   the .macro line itself; the .endmacro line is also shown).
 */

#include <stddef.h>
#include <stdbool.h>

typedef enum {
  FL_NONE        = 0,  /* ordinary line                               */
  FL_FOLD_START  = 1,  /* first line of a foldable group              */
  FL_FOLD_BODY   = 2,  /* hidden when collapsed                       */
  FL_FOLD_END    = 3,  /* last line of group (shown even if collapsed)*/
} fold_line_kind_t;

typedef struct {
  fold_line_kind_t kind;
  bool   collapsed;     /* true = body lines hidden                   */
  size_t group_end;     /* for FL_FOLD_START: 0-based index of last   */
                        /* body line (FL_FOLD_BODY or FL_FOLD_END)    */
  int    group_size;    /* total lines in group incl. start           */
  bool   is_macro;      /* true = macro fold, false = comment fold    */
} fold_line_t;

typedef struct {
  fold_line_t *lines;   /* heap-allocated, count == buf line count    */
  size_t       count;
} fold_map_t;

/* Rebuild the fold map from scratch.
 * fold_comments / fold_macros: which fold types to detect.
 * If both are 0, all entries are FL_NONE / not collapsed. */
void fold_map_rebuild(fold_map_t *fm,
                      const char **buf_lines, size_t line_count,
                      bool fold_comments, bool fold_macros);

/* Free heap memory. */
void fold_map_free(fold_map_t *fm);

/* Toggle collapsed state for the group that starts at line `idx`.
 * Returns true if idx is a FL_FOLD_START, false otherwise. */
bool fold_toggle(fold_map_t *fm, size_t idx);

/* Translate a visual row (0-based, skipping hidden lines) to a buffer
 * line index.  Returns buf line count if out of range. */
size_t fold_visual_to_buf(const fold_map_t *fm, size_t visual_row);

/* Translate a buffer line index to a visual row.
 * Returns SIZE_MAX if the line is hidden. */
size_t fold_buf_to_visual(const fold_map_t *fm, size_t buf_line);

/* Count visible (non-hidden) lines. */
size_t fold_visible_count(const fold_map_t *fm);
