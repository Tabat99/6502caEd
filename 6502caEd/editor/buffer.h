#pragma once
#include <stdbool.h>
#include <stddef.h>

/* ── Line-based text buffer ───────────────────────────────────────────────
 * buffer_t stores the editor's in-memory document as an array of
 * heap-allocated C strings, one per line.  Newlines are not stored in
 * the strings themselves; they are written by buffer_save.
 *
 * All editing operations (insert, backspace, newline) take and update
 * a cursor position expressed as a (line, col) pair where col is a
 * 0-based byte offset within the line.
 */
typedef struct {
  char** lines;   /* heap array of NUL-terminated line strings */
  size_t count;   /* number of lines (always >= 1)             */
  bool modified;  /* true if buffer has unsaved changes         */
} buffer_t;

/* Initialise an empty buffer containing one empty line.
 * Must be paired with buffer_free.
 * ret: false on OOM */
bool buffer_init_empty(buffer_t* b);

/* Load a file into the buffer, replacing any previous contents.
 * Lines are read with fgets and newlines are stripped.
 * ret: false if the file cannot be opened */
bool buffer_load(buffer_t* b, const char* path);

/* Save all lines to path, writing a trailing newline after each.
 * ret: false on file-open or write failure */
bool buffer_save(const buffer_t* b, const char* path);

/* Free all heap memory and zero the struct. */
void buffer_free(buffer_t* b);

/* Return a pointer to the NUL-terminated string for line i.
 * Returns "" if i is out of range or the line is NULL. */
const char* buffer_get_line(const buffer_t* b, size_t i);

/* Return the byte length of line i (equivalent to strlen(buffer_get_line(...))). */
size_t buffer_line_len(const buffer_t* b, size_t i);

/* Insert character ch at byte offset col on the given line.
 * col is clamped to the line length.  Sets modified = true.
 * ret: false on OOM */
bool buffer_insert_char(buffer_t* b, size_t line, size_t col, char ch);

/* Delete the character before (line, *io_col).
 * If col == 0, joins the current line onto the end of the previous one.
 * Updates *io_line and *io_col to reflect the new cursor position.
 * ret: false on OOM */
bool buffer_backspace(buffer_t* b, size_t* io_line, size_t* io_col);

/* Split the current line at *io_col, moving the right portion to a new
 * line below.  Updates *io_line and *io_col to the start of the new line.
 * ret: false on OOM */
bool buffer_newline(buffer_t* b, size_t* io_line, size_t* io_col);

/* Replace all occurrences of needle with repl across all lines.
 * out: *out_count — total replacement count (may be NULL)
 * ret: false on OOM or if needle is empty */
bool buffer_replace_all(buffer_t* b, const char* needle, const char* repl, size_t* out_count);
