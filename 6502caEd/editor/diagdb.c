#include "diagdb.h"
#include "../core/util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Initialise an empty diagnostic database. */
void diagdb_init(diagdb_t* db) {
  memset(db, 0, sizeof(*db));
}

/* Free all message strings and reset counters. */
void diagdb_clear(diagdb_t* db) {
  if (!db) return;
  for (size_t i = 0; i < db->count; i++) free(db->items[i].message);
  db->count = 0;
  db->num_errors = 0;
  db->num_warnings = 0;
}

/* Free the items array and zero the struct. */
void diagdb_free(diagdb_t* db) {
  if (!db) return;
  diagdb_clear(db);
  free(db->items);
  memset(db, 0, sizeof(*db));
}

/* Grow the items array to hold at least `need` entries. */
static bool ensure_cap(diagdb_t* db, size_t need) {
  if (need <= db->cap) return true;
  size_t newcap = db->cap ? db->cap * 2 : 16;
  while (newcap < need) newcap *= 2;
  diag_item_t* p = (diag_item_t*)realloc(db->items, newcap * sizeof(diag_item_t));
  if (!p) return false;
  db->items = p;
  db->cap = newcap;
  return true;
}

/* Return a pointer to the last path component (after the final slash). */
static const char* basename_c(const char* path) {
  const char* b = path;
  for (const char* p = path; *p; p++) {
    if (*p == '/' || *p == '\\') b = p + 1;
  }
  return b;
}

/* Return true if diag_file refers to the same file as current_file.
 * Accepts either an exact path match or a basename-only match so that
 * relative paths in diagnostic output match absolute buffer paths. */
static bool file_matches(const char* diag_file, const char* current_file) {
  if (!current_file || !current_file[0]) return false;
  if (strcmp(diag_file, current_file) == 0) return true;
  return strcmp(basename_c(diag_file), basename_c(current_file)) == 0;
}

/* Add a diagnostic directly (used for editor-side checks that do not go
 * through the assembler output parser).
 * in:  line1, col1 — 1-based source location (clamped to 1 if 0)
 *      kind        — DIAG_WARNING or DIAG_ERROR
 *      message     — copied into the item
 * ret: false on OOM */
/* Add a diagnostic directly (used for editor-side checks that do not go
 * through the assembler output parser).
 * in:  line1, col1 — 1-based source location
 *      kind        — DIAG_WARNING or DIAG_ERROR
 *      message     — copied into the item
 * ret: false on OOM */
bool diagdb_add(diagdb_t* db, size_t line1, size_t col1, diag_kind_t kind, const char* message) {
  if (!db) return false;
  if (!ensure_cap(db, db->count + 1)) return false;

  diag_item_t* it = &db->items[db->count++];
  it->line1 = line1 ? line1 : 1u;
  it->col1  = col1 ? col1 : 1u;
  it->kind = kind;
  it->message = xstrdup(message ? message : "");
  if (!it->message) { db->count--; return false; }

  if (kind == DIAG_ERROR) db->num_errors++;
  else db->num_warnings++;
  return true;
}

/* Parse a single assembler output line of the form:
 *   file:line:col: (error|warning): message
 * and add the result to db if the file matches current_file.
 * Internal use only - not exposed in diagdb.h. */
static bool diagdb_parse_line(diagdb_t* db, const char* current_file, const char* line) {
  /* expected: <file>:<line>:<col>: <kind>: <msg> */
  const char* p = line;
  const char* c1 = strchr(p, ':');
  if (!c1) return false;

  /* Extract file component. */
  char filebuf[512];
  size_t flen = (size_t)(c1 - p);
  if (flen == 0 || flen >= sizeof(filebuf)) return false;
  memcpy(filebuf, p, flen);
  filebuf[flen] = 0;

  if (!file_matches(filebuf, current_file)) return false;

  const char* c2 = strchr(c1 + 1, ':');
  if (!c2) return false;
  const char* c3 = strchr(c2 + 1, ':');
  if (!c3) return false;

  /* Parse line number. */
  char nbuf[32];
  size_t nlen = (size_t)(c2 - (c1 + 1));
  if (nlen == 0 || nlen >= sizeof(nbuf)) return false;
  memcpy(nbuf, c1 + 1, nlen); nbuf[nlen] = 0;
  char* endp = NULL;
  long lno = strtol(nbuf, &endp, 10);
  if (!endp || *endp) return false;

  /* Parse column number. */
  nlen = (size_t)(c3 - (c2 + 1));
  if (nlen == 0 || nlen >= sizeof(nbuf)) return false;
  memcpy(nbuf, c2 + 1, nlen); nbuf[nlen] = 0;
  endp = NULL;
  long col = strtol(nbuf, &endp, 10);
  if (!endp || *endp) return false;

  /* Determine severity and skip to message text. */
  const char* rest = c3 + 1;
  while (*rest == ' ' || *rest == '\t') rest++;

  diag_kind_t kind;
  const char* msg = NULL;
  if (strncmp(rest, "error:", 6) == 0) {
    kind = DIAG_ERROR;
    msg = rest + 6;
  } else if (strncmp(rest, "warning:", 8) == 0) {
    kind = DIAG_WARNING;
    msg = rest + 8;
  } else {
    return false;
  }
  while (*msg == ' ' || *msg == '\t') msg++;

  if (!ensure_cap(db, db->count + 1)) return false;

  diag_item_t* it = &db->items[db->count++];
  it->line1 = (lno < 1) ? 1u : (size_t)lno;
  it->col1  = (col < 1) ? 1u : (size_t)col;
  it->kind = kind;
  it->message = xstrdup(msg);
  if (!it->message) { db->count--; return false; }

  if (kind == DIAG_ERROR) db->num_errors++;
  else db->num_warnings++;

  /* Strip trailing newline from the duplicated message. */
  size_t mlen = strlen(it->message);
  while (mlen && (it->message[mlen-1] == '\n' || it->message[mlen-1] == '\r')) {
    it->message[--mlen] = 0;
  }

  return true;
}

/* Return a pointer to the first diagnostic on 1-based line `line1`,
 * or NULL if no diagnostic exists on that line. */
const diag_item_t* diagdb_first_on_line(const diagdb_t* db, size_t line1) {
  if (!db) return NULL;
  for (size_t i = 0; i < db->count; i++) {
    if (db->items[i].line1 == line1) return &db->items[i];
  }
  return NULL;
}

/* Return the index of the next diagnostic after cur_idx, wrapping to 0
 * at the end.  Returns SIZE_MAX if the database is empty. */
size_t diagdb_next_index(const diagdb_t* db, size_t cur_idx) {
  if (!db || db->count == 0) return (size_t)-1;
  size_t start = (cur_idx == (size_t)-1) ? 0 : (cur_idx + 1);
  if (start >= db->count) start = 0;
  return start;
}

/* Return the index of the previous diagnostic before cur_idx, wrapping
 * to count-1 at the start.  Returns SIZE_MAX if the database is empty. */
size_t diagdb_prev_index(const diagdb_t* db, size_t cur_idx) {
  if (!db || db->count == 0) return (size_t)-1;
  if (cur_idx == (size_t)-1 || cur_idx == 0) return db->count - 1;
  return cur_idx - 1;
}
