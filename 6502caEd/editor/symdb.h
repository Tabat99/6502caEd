#pragma once

#include "buffer.h"
#include <stdbool.h>
#include <stddef.h>

// Lightweight symbol database for on-the-fly editor highlighting.
// Deterministic, no regex. Case-insensitive (stored uppercase).

typedef struct {
  char* name;       // uppercase, heap owned
  size_t def_line1; // 1-based
  int def_col1;     // 1-based
  bool is_macro;    // true if defined via .macro / macro keyword
} symdef_t;

typedef struct {
  symdef_t* defs;
  size_t def_count;
  size_t def_cap;

  // Per-line definition span for highlighting duplicates.
  // If a line defines a symbol, def_start/def_end describe the token span (byte offsets).
  // If none, def_start == def_end == 0.
  size_t* def_start;
  size_t* def_end;
  bool* def_is_dup;
  size_t line_cap; // allocated length for per-line arrays
} symdb_t;

void symdb_init(symdb_t* db);
void symdb_free(symdb_t* db);

// Rebuild symbol table from buffer contents.
// Populates per-line definition spans and duplicate flags.
void symdb_rebuild(symdb_t* db, const buffer_t* buf);

// Check if an identifier (without leading '.' and without trailing ':') is defined.
bool symdb_is_defined(const symdb_t* db, const char* s, size_t n);

// Check if an identifier is defined as a macro.
bool symdb_is_macro(const symdb_t* db, const char* s, size_t n);

// If this line defines a symbol, returns true and fills span + duplicate flag.
bool symdb_line_def(const symdb_t* db, size_t line_idx0, size_t* out_start, size_t* out_end, bool* out_dup);
