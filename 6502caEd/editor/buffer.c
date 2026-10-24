#include <limits.h>
#include <stdint.h>
#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate a line string, stripping any trailing CR or LF.
 * Used when loading a file so the buffer never stores newline chars.
 * ret: heap-allocated string, or NULL on OOM */
static char* dup_line_strip_newline(const char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
  char* out = (char*)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, s, n);
  out[n] = 0;
  return out;
}

/* Ensure that slot `line` exists in the buffer, allocating empty strings
 * for any new slots.  Used by buffer_insert_char to handle the case where
 * the cursor is moved beyond the current line count.
 * ret: false on OOM */
static bool ensure_line_exists(buffer_t* b, size_t line) {
  if (line < b->count) return true;
  size_t new_count = line + 1;
  char** new_lines = (char**)realloc(b->lines, new_count * sizeof(char*));
  if (!new_lines) return false;
  /* init new slots */
  for (size_t i = b->count; i < new_count; i++) new_lines[i] = NULL;
  b->lines = new_lines;
  for (size_t i = b->count; i < new_count; i++) {
    b->lines[i] = (char*)malloc(1);
    if (!b->lines[i]) return false;
    b->lines[i][0] = 0;
  }
  b->count = new_count;
  return true;
}

/* Initialise a buffer with a single empty line. */
bool buffer_init_empty(buffer_t* b) {
  if (!b) return false;
  memset(b, 0, sizeof(*b));
  b->lines = (char**)malloc(sizeof(char*));
  if (!b->lines) return false;
  b->lines[0] = (char*)malloc(1);
  if (!b->lines[0]) { free(b->lines); memset(b, 0, sizeof(*b)); return false; }
  b->lines[0][0] = 0;
  b->count = 1;
  b->modified = false;
  return true;
}

/* Load a text file line by line using fgets.  Each line is stripped of its
 * trailing newline before storage.  If the file is empty, one empty line
 * is created so the buffer is always non-empty. */
bool buffer_load(buffer_t* b, const char* path) {
  memset(b, 0, sizeof(*b));
  FILE* f = fopen(path, "rb");
  if (!f) return false;

  char tmp[8192];
  while (fgets(tmp, sizeof(tmp), f)) {
    char* ln = dup_line_strip_newline(tmp);
    if (!ln) { fclose(f); buffer_free(b); return false; }

    char** newp = (char**)realloc(b->lines, (b->count + 1) * sizeof(char*));
    if (!newp) { free(ln); fclose(f); buffer_free(b); return false; }
    b->lines = newp;
    b->lines[b->count++] = ln;
  }

  fclose(f);
  if (b->count == 0) {
    /* Ensure at least one line exists */
    if (!ensure_line_exists(b, 0)) { buffer_free(b); return false; }
  }
  b->modified = false;
  return true;
}

/* Write all lines to path, appending '\n' after each one. */
bool buffer_save(const buffer_t* b, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  for (size_t i = 0; i < b->count; i++) {
    const char* s = b->lines[i] ? b->lines[i] : "";
    if (fwrite(s, 1, strlen(s), f) != strlen(s)) { fclose(f); return false; }
    /* Preserve trailing newline for all lines (simple policy) */
    if (fputc('\n', f) == EOF) { fclose(f); return false; }
  }
  fclose(f);
  return true;
}

/* Free every line string, the lines array, and zero the struct. */
void buffer_free(buffer_t* b) {
  if (!b) return;
  for (size_t i = 0; i < b->count; i++) free(b->lines[i]);
  free(b->lines);
  memset(b, 0, sizeof(*b));
}

/* Return the string for line i, or "" if out of range. */
const char* buffer_get_line(const buffer_t* b, size_t i) {
  if (!b || i >= b->count || !b->lines[i]) return "";
  return b->lines[i];
}

/* Return the byte length of line i. */
size_t buffer_line_len(const buffer_t* b, size_t i) {
  return strlen(buffer_get_line(b, i));
}

/* Insert ch at byte offset col on the given line.
 * The line is reallocated to make room; col is clamped to line length. */
bool buffer_insert_char(buffer_t* b, size_t line, size_t col, char ch) {
  if (!ensure_line_exists(b, line)) return false;

  char* s = b->lines[line];
  size_t len = strlen(s);
  if (col > len) col = len;

  char* news = (char*)realloc(s, len + 2);
  if (!news) return false;
  s = news;
  memmove(s + col + 1, s + col, (len - col) + 1);
  s[col] = ch;
  b->lines[line] = s;
  b->modified = true;
  return true;
}

/* Delete n bytes starting at offset from on the given line.
 * Shrinks the heap allocation after removal. */
static bool line_delete_range(buffer_t* b, size_t line, size_t from, size_t n) {
  char* s = b->lines[line];
  size_t len = strlen(s);
  if (from > len) return true;
  if (from + n > len) n = len - from;
  memmove(s + from, s + from + n, (len - (from + n)) + 1);
  char* news = (char*)realloc(s, strlen(s) + 1);
  if (news) b->lines[line] = news;
  b->modified = true;
  return true;
}

/* Delete the character before (line, *io_col).
 * When col == 0, joins the current line onto the end of the previous line
 * and removes the current line from the array. */
bool buffer_backspace(buffer_t* b, size_t* io_line, size_t* io_col) {
  size_t line = *io_line;
  size_t col  = *io_col;

  if (line >= b->count) return true;
  size_t len = buffer_line_len(b, line);

  if (col > len) col = len;

  if (col > 0) {
    /* delete previous char */
    if (!line_delete_range(b, line, col - 1, 1)) return false;
    *io_col = col - 1;
    return true;
  }

  /* col == 0: join with previous line by appending current content to it */
  if (line == 0) return true;

  size_t prev_len = buffer_line_len(b, line - 1);
  const char* cur = buffer_get_line(b, line);

  char* prev = b->lines[line - 1];
  size_t cur_len = strlen(cur);
  if (prev_len > SIZE_MAX - cur_len - 1) return false; /* overflow guard */
  char* merged = (char*)realloc(prev, prev_len + cur_len + 1);
  if (!merged) return false;
  memcpy(merged + prev_len, cur, cur_len + 1);
  b->lines[line - 1] = merged;

  /* Slide all subsequent lines up by one. */
  free(b->lines[line]);
  for (size_t i = line; i + 1 < b->count; i++) b->lines[i] = b->lines[i + 1];
  b->count--;
  if (b->count == 0) b->count = 1; /* should not happen */
  char** new_lines = (char**)realloc(b->lines, b->count * sizeof(char*));
  if (new_lines) b->lines = new_lines;

  b->modified = true;
  *io_line = line - 1;
  *io_col  = prev_len;
  return true;
}

/* Split the current line at col: the right portion becomes a new line
 * inserted immediately below, and the cursor moves to its start. */
bool buffer_newline(buffer_t* b, size_t* io_line, size_t* io_col) {
  size_t line = *io_line;
  size_t col  = *io_col;

  if (!ensure_line_exists(b, line)) return false;

  char* s = b->lines[line];
  size_t len = strlen(s);
  if (col > len) col = len;

  /* Save the right part of the split. */
  char* right = dup_line_strip_newline(s + col);
  if (!right) return false;

  /* Truncate the left part in-place. */
  s[col] = 0;
  char* left_shrink = (char*)realloc(s, col + 1);
  if (left_shrink) b->lines[line] = left_shrink;

  /* Insert the new line after the current one, shifting all following lines down. */
  char** newp = (char**)realloc(b->lines, (b->count + 1) * sizeof(char*));
  if (!newp) { free(right); return false; }
  b->lines = newp;
  for (size_t i = b->count; i > line + 1; i--) b->lines[i] = b->lines[i - 1];
  b->lines[line + 1] = right;
  b->count++;

  b->modified = true;
  *io_line = line + 1;
  *io_col  = 0;
  return true;
}

/* Replace all occurrences of needle with repl across every line.
 * For each affected line, counts occurrences, computes the new length,
 * allocates a fresh buffer, and copies with substitutions applied. */
bool buffer_replace_all(buffer_t* b, const char* needle, const char* repl, size_t* out_count) {
  if (out_count) *out_count = 0;
  if (!b || !needle || !needle[0] || !repl) return false;
  size_t nlen = strlen(needle);
  size_t rlen = strlen(repl);
  size_t total = 0;

  for (size_t li = 0; li < b->count; li++) {
    char* s = b->lines[li];
    if (!s) continue;
    const char* p = s;
    if (!strstr(p, needle)) continue;  /* quick early-out */

    /* Count occurrences to calculate the new line length. */
    size_t occ = 0;
    const char* q = p;
    while ((q = strstr(q, needle)) != NULL) {
      occ++;
      q += nlen;
    }
    if (!occ) continue;

    size_t oldlen = strlen(s);
    size_t newlen = oldlen + occ * (rlen - nlen);
    char* ns = (char*)malloc(newlen + 1);
    if (!ns) return false;

    /* Copy with substitutions. */
    char* dst = ns;
    const char* src = s;
    while (1) {
      const char* hit = strstr(src, needle);
      if (!hit) {
        size_t tail = strlen(src);
        memcpy(dst, src, tail);
        dst += tail;
        break;
      }
      size_t head = (size_t)(hit - src);
      memcpy(dst, src, head);
      dst += head;
      if (rlen) {
        memcpy(dst, repl, rlen);
        dst += rlen;
      }
      src = hit + nlen;
    }
    *dst = 0;

    free(b->lines[li]);
    b->lines[li] = ns;
    b->modified = true;
    total += occ;
  }
  if (out_count) *out_count = total;
  return true;
}
